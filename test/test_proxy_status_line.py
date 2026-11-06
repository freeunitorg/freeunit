"""Status line of a proxied upstream response (RFC 9112, 4).

status-line = HTTP-version SP status-code SP [ reason-phrase ]

The status code is exactly three digits, between two spaces.  A reply that
does not follow this gives 502.
"""

import select
import socket

import pytest

from conftest import run_process
from unit import port as port_map
from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# Shared with test_proxy.py and test_proxy_chunked.py, see
# test/fake_upstream/README.md.
UPSTREAM_PORT = port_map.port(7999)

BODY = '0123456789'

# Upstream status line for each target.
STATUS_LINES = {
    '/ok': 'HTTP/1.1 200 OK',
    '/no-reason': 'HTTP/1.1 200 ',
    '/no-space': 'HTTP/1.1 200',
    '/http10': 'HTTP/1.0 200 OK',
    '/four-digits': 'HTTP/1.1 2049 OK',
    '/no-sep': 'HTTP/1.1 200OK',
    '/tab': 'HTTP/1.1\t200 OK',
    '/two-spaces': 'HTTP/1.1  200 OK',
}


def run_server(server_port):
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

        request = data.decode('utf-8', errors='ignore')
        target = request.split(' ')[1] if ' ' in request else ''

        line = STATUS_LINES.get(target, 'HTTP/1.1 500 Error')

        try:
            conn.sendall(
                f'{line}\r\nContent-Length: {len(BODY)}\r\n'
                f'Connection: close\r\n\r\n{BODY}'.encode()
            )
        except OSError:
            pass

        conn.close()


@pytest.fixture(autouse=True)
def setup_method_fixture():
    run_process(run_server, UPSTREAM_PORT)
    waitforsocket(UPSTREAM_PORT)

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {"action": {"proxy": f'http://127.0.0.1:{UPSTREAM_PORT}'}}
            ],
        }
    ), 'proxy configuration'


@pytest.mark.parametrize(
    'target', ['/ok', '/no-reason', '/no-space', '/http10']
)
def test_proxy_status_line_valid(target):
    resp = client.get(url=target)

    assert resp['status'] == 200, f'status: {resp}'
    assert resp['body'] == BODY, f'body: {resp}'


@pytest.mark.parametrize(
    'target', ['/four-digits', '/no-sep', '/tab', '/two-spaces']
)
def test_proxy_status_line_invalid(target):
    assert client.get(url=target)['status'] == 502, 'status'
