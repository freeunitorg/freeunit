"""Proxy invalid upstream Content-Length regression test.

When a proxied upstream sends a Content-Length that does not parse (a sign,
a list, trailing characters, or a value that overflows nxt_off_t),
nxt_http_proxy_content_length() logs the value at [warn], marks the response
inconsistent and does not forward the field.  The body is framed by the read
to the end of the upstream connection.  The client gets the response chunked
(HTTP/1.1) or delimited by the close (HTTP/1.0), and the connection is closed
after it.  A response that has no body (HEAD, 304) goes out without the field.

test_proxy_bad_cl is driven by the `bad-cl` mode of the Rust mock upstream
(test/fake_upstream/): it sends `Content-Length: notanumber` followed by a
short body, then closes.  The other tests start a small upstream in Python.
Language-module-free (gated only on built-in proxy support).
"""

import os
import select
import socket
import subprocess

import pytest

from conftest import run_process
from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket
from unit import port as port_map

client = ApplicationProto()

# Must match BAD_CL_BODY / BAD_CL_VALUE in test/fake_upstream/src/main.rs.
BAD_CL_BODY = '0123456789'
BAD_CL_VALUE = 'notanumber'

# Reserved fake_upstream port for this case (see test/fake_upstream/README.md).
UPSTREAM_BAD_CL_PORT = port_map.port(7988)

FAKE_UPSTREAM_BIN = '/usr/local/bin/fake_upstream'

_skipif_no_fake_upstream = pytest.mark.skipif(
    not os.path.exists(FAKE_UPSTREAM_BIN),
    reason=f'{FAKE_UPSTREAM_BIN} not installed (build via test/fake_upstream)',
)


def _run_bad_cl(port=UPSTREAM_BAD_CL_PORT):
    proc = subprocess.Popen(
        [FAKE_UPSTREAM_BIN, '--port', str(port), '--mode', 'bad-cl'],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        waitforsocket(port)
    except Exception:
        proc.terminate()
        proc.wait()
        raise
    return proc


@_skipif_no_fake_upstream
def test_proxy_bad_cl(skip_alert):
    # The router logs a warning about the invalid upstream Content-Length;
    # it is [warn] not [alert], but suppress it defensively.
    skip_alert(r'upstream Content-Length .* is invalid')

    proc = _run_bad_cl()
    try:
        assert 'success' in client.conf(
            {
                "listeners": {"*:8080": {"pass": "routes"}},
                "routes": [
                    {
                        "action": {
                            "proxy": f'http://127.0.0.1:{UPSTREAM_BAD_CL_PORT}'
                        }
                    }
                ],
            }
        ), 'bad-cl proxy configuration'

        resp = client.get(port=8080)

        # The response is relayed with its body intact...
        assert resp['status'] == 200, f'unexpected status: {resp}'
        assert resp['body'] == BAD_CL_BODY, f'body not relayed intact: {resp}'

        # ...but the invalid Content-Length marks the response inconsistent, so
        # the connection is closed rather than kept alive -- bounding the
        # unusable framing to a single connection (no keep-alive desync).
        assert (
            resp['headers'].get('Connection') == 'close'
        ), f'inconsistent response must close the connection: {resp}'
    finally:
        proc.terminate()
        proc.wait()


# The small upstream below.  It answers with the status and the
# Content-Length fields that the request target selects, sends BODY unless
# the request is HEAD or the status has no body, and closes.
UPSTREAM_PORT = port_map.port(7999)

BODY = b'A' * 10 + b'B' * 20

INVALID_VALUES = {
    '/plus': ['+10'],
    '/list': ['10, 200'],
    '/trail': ['10abc'],
    '/overflow': ['99999999999999999999'],
}


def run_upstream(server_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('127.0.0.1', server_port))
    sock.listen(10)

    while True:
        conn, _ = sock.accept()

        data = b''
        while b'\r\n\r\n' not in data:
            if not select.select([conn], [], [], 5)[0]:
                break

            part = conn.recv(4096)
            if not part:
                break

            data += part

        line = data.split(b'\r\n', 1)[0].decode().split(' ')
        if len(line) < 2:
            # The connection that waitforsocket() opens sends nothing.
            conn.close()
            continue

        method, target = line[:2]
        status = '304 Not Modified' if target == '/304' else '200 OK'
        values = INVALID_VALUES.get(target, ['+10'])

        resp = f'HTTP/1.1 {status}\r\n'
        for value in values:
            resp += f'Content-Length: {value}\r\n'
        resp = (resp + '\r\n').encode()

        if method != 'HEAD' and target != '/304':
            resp += BODY

        conn.sendall(resp)
        conn.close()


@pytest.fixture
def upstream(skip_alert):
    skip_alert(r'upstream Content-Length .* is invalid')

    run_process(run_upstream, UPSTREAM_PORT)
    waitforsocket(UPSTREAM_PORT)

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {"action": {"proxy": f'http://127.0.0.1:{UPSTREAM_PORT}'}}
            ],
        }
    ), 'proxy configuration'


def _exchange(request):
    # Send one request without "Connection: close" and read until the
    # server closes the connection.
    sock = socket.create_connection(('127.0.0.1', port_map.port(8080)))
    sock.settimeout(10)
    sock.sendall(request.encode())

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
        pass
    finally:
        sock.close()

    head, _, body = data.partition(b'\r\n\r\n')
    return head.lower(), body, closed


def _dechunk(raw):
    body = b''
    while True:
        line, raw = raw.split(b'\r\n', 1)
        size = int(line, 16)
        if size == 0:
            assert raw == b'\r\n', f'bytes after the last chunk: {raw!r}'
            return body
        body += raw[:size]
        assert raw[size:size + 2] == b'\r\n', f'bad chunk: {raw!r}'
        raw = raw[size + 2:]


@pytest.mark.parametrize('target', list(INVALID_VALUES))
def test_proxy_bad_cl_http11(upstream, target):
    head, body, closed = _exchange(
        f'GET {target} HTTP/1.1\r\nHost: localhost\r\n\r\n'
    )

    assert head.startswith(b'http/1.1 200'), head
    assert b'content-length' not in head, head
    assert b'transfer-encoding: chunked' in head, head
    assert _dechunk(body) == BODY
    assert closed, 'connection must be closed after the response'


@pytest.mark.parametrize('target', list(INVALID_VALUES))
def test_proxy_bad_cl_http10(upstream, target):
    head, body, closed = _exchange(
        f'GET {target} HTTP/1.0\r\nHost: localhost\r\n'
        'Connection: keep-alive\r\n\r\n'
    )

    assert head.startswith(b'http/1.1 200'), head
    assert b'content-length' not in head, head
    assert b'transfer-encoding' not in head, head
    assert b'connection: keep-alive' not in head, head
    assert body == BODY
    assert closed, 'connection must be closed after the response'


@pytest.mark.parametrize(
    'method, target, status',
    [('HEAD', '/plus', b'200'), ('GET', '/304', b'304')],
)
def test_proxy_bad_cl_no_body(upstream, method, target, status):
    head, body, closed = _exchange(
        f'{method} {target} HTTP/1.1\r\nHost: localhost\r\n\r\n'
    )

    assert head.startswith(b'http/1.1 ' + status), head
    assert b'content-length' not in head, head
    assert b'transfer-encoding' not in head, head
    assert body == b''
    assert closed, 'connection must be closed after the response'
