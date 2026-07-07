"""Proxy Content-Length *overrun* relay regression test.

An upstream that sends more body bytes than its Content-Length advertises must
not have the excess relayed downstream: FreeUnit already sent that same
Content-Length to the client, so forwarding the surplus is what would enable
response splitting on a kept-alive connection. The hardening in
nxt_h1p_peer_body_process truncates the buf chain to the advertised length,
flags the response inconsistent (disabling keep-alive) and closes the
connection.

This test asserts the mechanism that prevents splitting -- the client receives
exactly the advertised bytes and no more -- rather than driving a second
pipelined request; a single request with `Connection: close` is enough to
observe the truncation.

Driven by the `overrun-cl` mode of the Rust mock upstream
(test/fake_upstream/): it declares `Content-Length: 100` but writes 150 bytes.
Deliberately language-module-free (gated only on the built-in proxy support) so
it runs on the minimal test build.
"""

import os
import socket
import subprocess

import pytest

from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# Deterministic body: byte at global offset i is PATTERN[i % 16] — mirrors the
# fake_upstream PATTERN so the test regenerates the exact advertised bytes.
PATTERN = '0123456789abcdef'

# Must match OVERRUN_DECLARED / OVERRUN_EXCESS in test/fake_upstream/src/main.rs.
DECLARED = 100
EXCESS = 50

# Reserved fake_upstream ports for these cases (see test/fake_upstream/README.md).
UPSTREAM_OVERRUN_PORT = 7989
UPSTREAM_OVERRUN_KA_PORT = 7987

FAKE_UPSTREAM_BIN = '/usr/local/bin/fake_upstream'

_skipif_no_fake_upstream = pytest.mark.skipif(
    not os.path.exists(FAKE_UPSTREAM_BIN),
    reason=f'{FAKE_UPSTREAM_BIN} not installed (build via test/fake_upstream)',
)


def _run(port, mode):
    proc = subprocess.Popen(
        [FAKE_UPSTREAM_BIN, '--port', str(port), '--mode', mode],
        stdout=subprocess.DEVNULL,
        # DEVNULL, not PIPE: nothing reads this stream, and an unread PIPE can
        # deadlock the child if it ever fills the OS pipe buffer.
        stderr=subprocess.DEVNULL,
    )
    # Tear the process down if it never binds — otherwise a waitforsocket
    # timeout would raise before the caller's try/finally and leak it.
    try:
        waitforsocket(port)
    except Exception:
        proc.terminate()
        proc.wait()
        raise
    return proc


def _run_overrun_cl(port=UPSTREAM_OVERRUN_PORT):
    return _run(port, 'overrun-cl')


@_skipif_no_fake_upstream
def test_proxy_overrun_cl(skip_alert):
    # The router logs a warning about the upstream overrun; don't let it fail
    # the run on the alert check.
    skip_alert(r'upstream sent .* body bytes past Content-Length')

    proc = _run_overrun_cl()
    try:
        assert 'success' in client.conf(
            {
                "listeners": {"*:8080": {"pass": "routes"}},
                "routes": [
                    {
                        "action": {
                            "proxy": f'http://127.0.0.1:{UPSTREAM_OVERRUN_PORT}'
                        }
                    }
                ],
            }
        ), 'overrun-cl proxy configuration'

        resp = client.get(port=8080)

        assert resp['status'] == 200, 'status'
        assert (
            resp['headers']['Content-Length'] == str(DECLARED)
        ), 'relayed Content-Length unchanged'

        # The excess bytes must be dropped: the client sees exactly the
        # advertised length, never DECLARED + EXCESS.
        expected = (PATTERN * (DECLARED // len(PATTERN) + 1))[:DECLARED]
        assert len(resp['body']) == DECLARED, (
            f'relayed body not truncated to Content-Length: '
            f'got {len(resp["body"])} bytes'
        )
        assert resp['body'] == expected, 'relayed body mismatch'
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_overrun_cl_keepalive(skip_alert):
    # Same overrun, but the upstream response is keep-alive-able (no
    # `Connection: close`). This isolates the *inconsistent* flag: the only
    # thing that can close the downstream connection here is FreeUnit deciding
    # the response is inconsistent -- not a relayed upstream close. A regression
    # that dropped the inconsistent flag would leave the connection open (the
    # recv below would then time out rather than see EOF).
    skip_alert(r'upstream sent .* body bytes past Content-Length')

    proc = _run(UPSTREAM_OVERRUN_KA_PORT, 'overrun-cl-ka')
    try:
        assert 'success' in client.conf(
            {
                "listeners": {"*:8080": {"pass": "routes"}},
                "routes": [
                    {
                        "action": {
                            "proxy": f'http://127.0.0.1:{UPSTREAM_OVERRUN_KA_PORT}'
                        }
                    }
                ],
            }
        ), 'overrun-cl-ka proxy configuration'

        # Keep-alive request (no Connection: close), read the raw response
        # ourselves so we can observe whether the server closes the socket.
        sock = client.get(port=8080, no_recv=True)
        sock.settimeout(10)

        data = b''
        closed = False
        try:
            while True:
                part = sock.recv(4096)
                if not part:
                    closed = True
                    break
                data += part
        except socket.timeout:
            closed = False
        finally:
            sock.close()

        body = data.split(b'\r\n\r\n', 1)[1] if b'\r\n\r\n' in data else b''
        expected = ((PATTERN * (DECLARED // len(PATTERN) + 1))[:DECLARED]).encode()

        assert data[:12] == b'HTTP/1.1 200', f'status line: {data[:40]!r}'
        assert body == expected, f'relayed body mismatch: {body!r}'
        assert closed, 'inconsistent response must close the downstream connection'
    finally:
        proc.terminate()
        proc.wait()
