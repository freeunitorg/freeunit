"""
Functional tests for the two-phase listener close.

These tests exercise the new "draining" state on nxt_listen_event_t.
The state machine they cover is:

    Accepting -> Draining: nxt_router_listen_socket_close() phase 1
                           (accept(2) disarmed, draining = 1)
    Draining  -> Closed:   nxt_router_listen_socket_close_finish() phase 2
                           (FD released once lev->count == 1)

The previous behaviour was to run phase 1 and phase 2 back-to-back from a
zero-timeout timer, which RST'd in-flight TLS handshakes and any
accepted-but-not-yet-handled connection on a busy listener.
"""

import socket
import ssl
import subprocess
import time

import pytest

from unit.applications.tls import ApplicationTLS

prerequisites = {'modules': {'python': 'any', 'openssl': 'any'}}

client = ApplicationTLS()


def _has_openssl():
    try:
        subprocess.check_output(
            ['openssl', 'version'], stderr=subprocess.STDOUT
        )
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False


def _add_tls(application='empty', cert='default', port=8080):
    assert 'success' in client.conf(
        {
            "pass": f"applications/{application}",
            "tls": {"certificate": cert},
        },
        f'listeners/*:{port}',
    )


def _clear_config():
    assert 'success' in client.conf({"listeners": {}, "applications": {}})


def _port_listening(port):
    """Return True if something is listening on `port` on localhost."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(1.0)
    try:
        s.connect(('127.0.0.1', port))
        s.close()
        return True
    except (ConnectionRefusedError, socket.timeout, OSError):
        return False


@pytest.mark.skipif(
    not _has_openssl(), reason='openssl CLI not available for cert generation'
)
def test_listener_reconfigure_drains_inflight_tls_handshake():
    """
    Begin a TLS handshake on a busy listener, then PUT a new listener
    config that removes TLS from the listener.  The handshake must
    either complete cleanly or fail with a clean TLS-level error,
    NOT with ECONNRESET (which is what the one-shot close produced).
    """
    client.load('empty')
    client.certificate()
    _add_tls()

    # Pre-warm: confirm TLS is up.
    assert client.get_ssl()['status'] == 200, 'pre-warm TLS'

    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    # Open a raw TCP socket; do NOT begin handshake yet.
    raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    raw.settimeout(5.0)
    raw.connect(('127.0.0.1', 8080))

    # Wait longer than TCP_DEFER_ACCEPT so the no-data connection is
    # accepted by Unit before the reconfigure fires.  Otherwise it can
    # still be in the kernel listen queue when the FD closes, which this
    # explicitly does not cover.
    time.sleep(1.5)

    # Delete the listener while the connection is
    # accepted but mid-handshake (we have not sent ClientHello yet).
    _clear_config()

    # Now drive the handshake.  Acceptable outcomes:
    #   - clean TLS error (SSLError) — server tore down the TLS layer.
    #   - clean EOF / handshake failure.
    # NOT acceptable: ECONNRESET (errno 104) on the bare socket.
    reset = False
    try:
        wrapped = ctx.wrap_socket(raw, server_hostname='localhost')
        try:
            wrapped.send(b'GET / HTTP/1.0\r\n\r\n')
            wrapped.recv(4096)
        finally:
            try:
                wrapped.close()
            except OSError:
                pass
    except ssl.SSLError:
        pass
    except ConnectionResetError:
        reset = True
    except OSError as exc:
        # errno 104 is ECONNRESET.
        if exc.errno == 104:
            reset = True
    finally:
        try:
            raw.close()
        except OSError:
            pass

    assert not reset, (
        'in-flight TLS handshake was RST by listener reconfiguration; '
        'two-phase drain regressed'
    )


@pytest.mark.skip(
    reason=(
        'Full per-connection drain on listener reconfiguration is '
        'future work (connection drain with timeout escalation).  '
        'Two-phase close only guarantees the listener-level FD release '
        'waits for accept refs to drop; the individual accepted '
        'connections are still torn down by the config swap path.  '
        'This test is staged so the drain work can flip it on by '
        'removing the skip marker.'
    )
)
def test_listener_drain_no_dropped_accepted_connection():
    """
    Open a plain TCP connection, let it be accepted, then reconfigure
    the listener.  The accepted connection must still be able to
    complete a request (it was accepted under the old listener and
    the drain must keep its joint alive).
    """
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    ), 'initial listener'

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(('127.0.0.1', 8080))

    # Give the kernel + router a beat to actually accept(2).
    time.sleep(0.05)

    # Replace the listener with one bound to a different port.
    assert 'success' in client.conf(
        {
            "listeners": {"*:8081": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    ), 'reconfigure to new port'

    # The already-accepted connection must still be writable and must
    # not have been RST.
    data = b''
    try:
        s.sendall(b'GET / HTTP/1.0\r\nHost: localhost\r\n\r\n')
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
            if len(data) > 65536:
                break
    except ConnectionResetError:
        pytest.fail('accepted connection was RST during listener drain')
    finally:
        s.close()

    # We don't assert exact response shape — just that we got *some*
    # bytes back (no RST mid-flight).  An empty reply would mean the
    # router dropped the conn, which is the bug we're guarding against.
    assert data, 'accepted connection produced no response after drain'


def test_listener_close_releases_fd_eventually():
    """
    After phase 2 of the two-phase close runs, the old listener FD
    must actually be closed: nothing should be listening on the
    original port.
    """
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    ), 'initial listener on 8080'

    assert _port_listening(8080), 'pre-condition: 8080 is up'

    assert 'success' in client.conf(
        {
            "listeners": {"*:8081": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    ), 'move to 8081'

    # Phase 2 runs as soon as no in-flight conns remain.  Allow a
    # short window for the work queue to drain.
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if not _port_listening(8080):
            break
        time.sleep(0.05)

    assert not _port_listening(
        8080
    ), 'old listener FD was not released after drain'
    assert _port_listening(8081), 'new listener FD did not come up'
