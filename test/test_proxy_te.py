"""How the proxy frames an upstream response by its Transfer-Encoding.

nxt_h1p_peer_transfer_encoding() used to take only the exact value "chunked"
as chunked.  RFC 9112 Sect. 6.1 makes coding names case-insensitive and the
value a comma-separated list, so "Chunked" or "gzip, chunked" was not
recognised, and the body was framed by Content-Length or by connection close
instead: a response framing desync.

The proxy decodes only chunked, and never forwards Transfer-Encoding to the
client.  So the whole list must be exactly one "chunked", in any letter case,
with optional whitespace and empty elements.  Any other Transfer-Encoding is a
502, and so is chunked together with Content-Length (RFC 9112 Sect. 6.3).  In
no case is the body framed by Content-Length.

The upstream is a plain Python socket server; each case is picked by the
request target.
"""

import select
import socket

import pytest

from conftest import run_process
from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# Reserved in test/fake_upstream/README.md's port registry.
UPSTREAM_PORT = 7976

BODY = 'hello, world'

# A chunked body whose chunk-size lines and CRLFs would leak into the relayed
# body if the proxy did not decode it.
CHUNKED_BODY = '5\r\nhello\r\n7\r\n, world\r\n0\r\n\r\n'


def resp(*fields, body=CHUNKED_BODY):
    head = ''.join(f'{f}\r\n' for f in fields)
    return f'HTTP/1.1 200 OK\r\n{head}Connection: close\r\n\r\n{body}'


UPSTREAM_RESPONSES = {
    '/lower': resp('Transfer-Encoding: chunked'),
    '/upper': resp('Transfer-Encoding: CHUNKED'),
    '/mixed': resp('Transfer-Encoding: Chunked'),
    '/ows': resp('Transfer-Encoding: ,\t chunked ,'),
    # Content-Length would cut the body after the first chunk-size line.
    '/mixed-cl': resp('Transfer-Encoding: Chunked', 'Content-Length: 3'),
    '/lower-cl': resp('Transfer-Encoding: chunked', 'Content-Length: 3'),
    '/gzip-chunked': resp('Transfer-Encoding: gzip, chunked'),
    '/gzip-chunked-cl': resp(
        'Transfer-Encoding: gzip, Chunked', 'Content-Length: 3'
    ),
    '/chunked-gzip': resp('Transfer-Encoding: chunked, gzip'),
    '/two-lines': resp(
        'Transfer-Encoding: gzip', 'Transfer-Encoding: chunked'
    ),
    '/chunked-twice': resp(
        'Transfer-Encoding: chunked', 'Transfer-Encoding: chunked'
    ),
    '/chunked-param': resp('Transfer-Encoding: chunked;a=b'),
    '/empty': resp('Transfer-Encoding: ,'),
    '/identity': resp(
        'Transfer-Encoding: identity', 'Content-Length: 3', body=BODY
    ),
}


def run_server(server_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.bind(('127.0.0.1', server_port))
    sock.listen(10)

    def recv_request(conn):
        data = b''

        while b'\r\n\r\n' not in data:
            if not select.select([conn], [], [], 5)[0]:
                break

            part = conn.recv(4096)
            if not part:
                break

            data += part

        return data.decode('utf-8', errors='ignore')

    while True:
        conn, _ = sock.accept()

        request = recv_request(conn)
        target = request.split(' ')[1] if ' ' in request else ''

        conn.sendall(
            UPSTREAM_RESPONSES.get(
                target, 'HTTP/1.1 500 Internal Server Error\r\n\r\n'
            ).encode()
        )
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


def get(target):
    return client.get(url=target, headers={'Host': 'localhost',
                                           'Connection': 'close'})


@pytest.mark.parametrize('target', ['/lower', '/upper', '/mixed', '/ows'])
def test_proxy_te_chunked(target):
    resp = get(target)

    assert resp['status'] == 200, 'status'
    assert resp['body'] == BODY, 'chunked body decoded'
    assert 'Content-Length' not in resp['headers'], 'no Content-Length'


@pytest.mark.parametrize(
    'target',
    [
        '/mixed-cl',
        '/lower-cl',
        '/gzip-chunked',
        '/gzip-chunked-cl',
        '/chunked-gzip',
        '/two-lines',
        '/chunked-twice',
        '/chunked-param',
        '/empty',
        '/identity',
    ],
)
def test_proxy_te_bad_gateway(target):
    resp = get(target)

    assert resp['status'] == 502, 'status'
    assert 'hello' not in resp['body'], 'upstream body not relayed'
