"""Connection: close on a response that the router marks inconsistent.

nxt_h1p_request_close() closes the connection after a response marked
inconsistent (r->inconsistent).  When the mark is set before the header is
sent, nxt_h1p_request_header_send() also puts "Connection: close" in the
header, so a keep-alive client does not send its next request on a
connection that the server closes.  The "limit_req" headers stage uses the
mark for a 429 sent before the body; the proxy uses it for an upstream
header that it cannot trust.

Here the upstream sends two Content-Length fields, and the proxy marks the
response inconsistent when it reads the upstream header.  The upstream is
plain Python, so no language module and no fake_upstream are necessary.
Port 7999 is the one of the plain-Python upstreams of test_proxy.py and
test_proxy_chunked.py; each test starts and stops its own upstream.
"""

import socket

import pytest

from conftest import run_process
from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket
from unit import port as port_map

client = ApplicationProto()
SERVER_PORT = port_map.port(7999)


def run_server(server_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('127.0.0.1', server_port))
    sock.listen(5)

    while True:
        connection, _ = sock.accept()

        data = b''

        while b'\r\n\r\n' not in data:
            part = connection.recv(4096)
            if not part:
                break

            data += part

        if data.startswith(b'GET /dup '):
            connection.sendall(
                b'HTTP/1.1 200 OK\r\n'
                b'Content-Length: 2\r\n'
                b'Content-Length: 2\r\n'
                b'\r\n'
                b'ok'
            )

        else:
            connection.sendall(
                b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok'
            )

        connection.close()


@pytest.fixture(autouse=True)
def setup_method_fixture():
    run_process(run_server, SERVER_PORT)
    waitforsocket(SERVER_PORT)

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {"action": {"proxy": f'http://127.0.0.1:{SERVER_PORT}'}}
            ],
            "applications": {},
        }
    ), 'proxy configuration'


def test_proxy_inconsistent_close(skip_alert):
    # The duplicate is logged at [warn]; suppress it defensively.
    skip_alert(r'upstream sent duplicate Content-Length')

    # A consistent response keeps the connection of a keep-alive client.
    resp, sock = client.get(start=True, connection_close=False, framed=True)
    sock.close()

    assert resp['status'] == 200, resp
    assert resp['body'] == 'ok', resp
    assert 'Connection' not in resp['headers'], resp

    # An inconsistent one closes it, and says so.  The read ends at the
    # close; without it, it ends at the timeout.
    resp, sock = client.get(
        url='/dup', start=True, connection_close=False, read_timeout=5
    )

    try:
        assert sock.recv(1) == b'', 'the connection is closed'

    finally:
        sock.close()

    assert resp['status'] == 200, resp
    assert resp['body'] == 'ok', resp
    assert resp['headers'].get('Connection') == 'close', resp
