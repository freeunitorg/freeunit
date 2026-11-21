"""
SIGQUIT drains the router, not only the application workers (issue #968).

SIGQUIT is the graceful stop: an application worker finishes the request it
is serving before it exits.  The router has to stay up for that response to
reach the client.  It used to exit as soon as main forwarded the QUIT, so
the worker's reply failed with "sendmsg(...) failed: Broken pipe" and the
client got an empty reply.

On a graceful QUIT the router now stops accepting, closes its idle
keep-alive connections, answers in-flight requests with "Connection: close",
and exits when no client connection is left (or a drain timeout passes).
A request that waits for a process gets 503.  SIGTERM stays the fast exit.
"""

import os
import signal
import socket
import sys
import time

import pytest

from unit import port as port_map
from unit.applications.lang.python import ApplicationPython

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

DELAY = 3
TIMEOUT = 30


@pytest.fixture(autouse=True)
def _require_restart(request):
    if not request.config.getoption('--restart'):
        pytest.skip('signals the unitd master process; needs --restart')


def _connect():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(TIMEOUT)
    sock.connect(('127.0.0.1', port_map.port(8080)))
    return sock


def _send(sock, tag, delay=0, close=False):
    sock.sendall(
        (
            'GET / HTTP/1.1\r\n'
            'Host: localhost\r\n'
            f'X-Tag: {tag}\r\n'
            f'X-Delay: {delay}\r\n'
            + ('Connection: close\r\n' if close else '')
            + '\r\n'
        ).encode()
    )


def _recv_until_eof(sock, timeout=TIMEOUT):
    sock.settimeout(timeout)
    data = b''
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(4096)
        except (ConnectionResetError, socket.timeout):
            break

        if not chunk:
            break

        data += chunk

    return data


def _recv_response(sock):
    """Read one Content-Length-delimited response on a keep-alive conn."""
    data = b''

    while b'\r\n\r\n' not in data:
        chunk = sock.recv(4096)
        assert chunk, f'connection closed before headers: {data!r}'
        data += chunk

    head, body = data.split(b'\r\n\r\n', 1)
    length = 0

    for line in head.split(b'\r\n')[1:]:
        name, value = line.split(b':', 1)
        if name.strip().lower() == b'content-length':
            length = int(value)

    while len(body) < length:
        chunk = sock.recv(4096)
        assert chunk, 'connection closed inside the body'
        body += chunk

    return head, body


def _load(processes=2):
    client.load('requeue_count', processes=processes)

    sock = _connect()
    _send(sock, 'warm', close=True)
    assert _recv_until_eof(sock).startswith(b'HTTP/1.1 200'), 'warm-up'
    sock.close()


def test_sigquit_delivers_inflight_response(unit_pid, skip_alert):
    _load()

    sock = _connect()
    _send(sock, 'inflight', delay=DELAY, close=True)

    time.sleep(1)

    os.kill(unit_pid, signal.SIGQUIT)

    resp = _recv_until_eof(sock)
    sock.close()

    assert resp.startswith(b'HTTP/1.1 200'), (
        f'SIGQUIT must let the in-flight request complete, got {resp!r}'
    )
    assert resp.endswith(b'\r\n\r\ninflight'), resp


def test_sigquit_closes_idle_and_stops_accepting(unit_pid, skip_alert):
    _load()

    idle = _connect()
    _send(idle, 'idle')
    head, body = _recv_response(idle)
    assert head.startswith(b'HTTP/1.1 200') and body == b'idle'

    busy = _connect()
    _send(busy, 'busy', delay=DELAY)

    time.sleep(1)

    start = time.monotonic()
    os.kill(unit_pid, signal.SIGQUIT)

    # The idle keep-alive connection is closed at once, not after the drain.
    assert _recv_until_eof(idle, timeout=DELAY) == b''
    assert time.monotonic() - start < DELAY - 1, 'idle conn closed late'
    idle.close()

    # A connection made after SIGQUIT is not served.
    try:
        late = _connect()
        _send(late, 'late', close=True)
        late_resp = _recv_until_eof(late)
        late.close()
    except OSError:
        late_resp = b''

    assert not late_resp.startswith(b'HTTP/1.1 200'), late_resp

    if sys.platform.startswith('linux'):
        # The listening socket is shut down: new connections are refused.
        for _ in range(20):
            try:
                _connect().close()
            except ConnectionRefusedError:
                break
            time.sleep(0.05)
        else:
            assert False, 'connections are still accepted'

    # The in-flight keep-alive request completes, and the router asks the
    # client to close because it is going away.
    head, body = _recv_response(busy)
    assert head.startswith(b'HTTP/1.1 200'), head
    assert body == b'busy'
    assert b'connection: close' in head.lower(), head
    assert _recv_until_eof(busy, timeout=5) == b''
    busy.close()


def test_sigquit_answers_waiting_request(unit_pid, skip_alert):
    _load(processes=1)

    inflight = _connect()
    _send(inflight, 'inflight', delay=DELAY, close=True)

    time.sleep(1)

    waiting = _connect()
    _send(waiting, 'waiting', close=True)

    time.sleep(0.5)

    start = time.monotonic()
    os.kill(unit_pid, signal.SIGQUIT)

    resp = _recv_until_eof(inflight)
    inflight.close()
    assert resp.endswith(b'\r\n\r\ninflight'), resp

    # The only process exits after its request, and no process starts
    # during the drain: the waiting request is answered, not dropped.
    resp = _recv_until_eof(waiting)
    waiting.close()
    assert resp.startswith(b'HTTP/1.1 503'), resp
    assert time.monotonic() - start < DELAY + 2, 'answered late'


def test_sigterm_cuts_drain_short(unit_pid, skip_alert):
    skip_alert(r'last message send failed')

    _load()

    sock = _connect()
    _send(sock, 'inflight', delay=DELAY + 2, close=True)

    time.sleep(1)

    os.kill(unit_pid, signal.SIGQUIT)
    time.sleep(0.5)

    start = time.monotonic()
    os.kill(unit_pid, signal.SIGTERM)

    resp = _recv_until_eof(sock)
    sock.close()

    assert not resp.startswith(b'HTTP/1.1 200'), resp
    assert time.monotonic() - start < DELAY, 'SIGTERM did not end the drain'


def test_sigquit_drain_timeout(unit_pid, skip_alert, wait_for_record):
    skip_alert(r'last message send failed')

    _load()

    # Longer than the router's drain bound (NXT_ROUTER_DRAIN_TIMEOUT, 8 s).
    sock = _connect()
    _send(sock, 'slow', delay=12, close=True)

    time.sleep(1)

    start = time.monotonic()
    os.kill(unit_pid, signal.SIGQUIT)

    resp = _recv_until_eof(sock)
    elapsed = time.monotonic() - start
    sock.close()

    assert resp == b'', resp
    assert 7 < elapsed < 11, f'drain ended after {elapsed:.1f} s'
    assert wait_for_record(r'still busy after 8000 ms') is not None
