"""How the proxy frames an upstream response by its Transfer-Encoding.

nxt_h1p_peer_transfer_encoding() used to accept only the exact value
"chunked".  RFC 9112 Sect. 6.1 makes coding names case-insensitive and the
value a comma-separated list.  So "Chunked" or "gzip, chunked" was not seen
as chunked.  The body was then framed by Content-Length or by connection
close, and the chunk framing reached the client as body bytes.

The proxy decodes only chunked, and it never forwards Transfer-Encoding to
the client.  So the whole list must be one "chunked", in any letter case.
Whitespace and empty list elements around it are allowed.  Every other
Transfer-Encoding gives 502.  Chunked together with Content-Length also
gives 502 (RFC 9112 Sect. 6.3).  Content-Length never frames a body that
has a Transfer-Encoding.

Neither 502 applies to a response the same Sect. 6.3 says has no body: a
response to HEAD, and any 204 or 304, ends at the first empty line whatever
Transfer-Encoding and Content-Length say.  There is no body to decode there,
so those responses are completed as bodyless and relayed normally.  The
keep-alive half of that path is pinned in test_proxy_head.py, whose pipeline
idiom the bodyless cases below reuse.

The upstream is a plain Python socket server.  The request target selects
the case.
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


def bodyless(status, *fields):
    """A response with no body at all, whatever its header fields claim."""

    head = ''.join(f'{f}\r\n' for f in fields)
    return f'HTTP/1.1 {status}\r\n{head}Connection: close\r\n\r\n'


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
    # Bodyless final responses carrying a Transfer-Encoding the proxy cannot
    # decode.  Every one of them writes only what is here and closes: none
    # sends the body its own header names.
    '/204-gzip': bodyless('204 No Content', 'Transfer-Encoding: gzip'),
    '/304-identity': bodyless('304 Not Modified', 'Transfer-Encoding: identity'),
    '/head-param': bodyless('200 OK', 'Transfer-Encoding: chunked;a=b'),
    # Exact "chunked" reached the bodyless path before this change too, so this
    # one guards against the move breaking it.
    '/204-chunked': bodyless('204 No Content', 'Transfer-Encoding: chunked'),
    # The other 502: chunked and Content-Length together.  A 304 keeps its
    # Content-Length (RFC 9110 Sect. 9.3.2 for HEAD, the cached length here).
    '/304-chunked-cl': bodyless(
        '304 Not Modified', 'Transfer-Encoding: chunked', 'Content-Length: 10'
    ),
    # The trailing request of every pipeline; this one does have a body.
    '/ok': 'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK',
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


def pipeline(method, target):
    """Send "<method> <target>" and a GET /ok back to back, read to EOF.

    Both requests leave in one write, so the second is already sitting in the
    listener's read buffer when the first response is generated: it can only be
    answered if the connection survives that response.  That is how a
    mis-framed bodyless response shows itself -- nxt_h1p_peer_closed() sets
    r->truncated and r->inconsistent, and nxt_h1p_request_close() then drops
    the client keep-alive.  Copied from test_proxy_head.py, which pins the same
    path without a Transfer-Encoding in play.
    """

    request = (
        f'{method} {target} HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Connection: keep-alive\r\n'
        '\r\n'
        'GET /ok HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Connection: close\r\n'
        '\r\n'
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(10)

    try:
        sock.connect(('127.0.0.1', 8080))
        sock.sendall(request.encode())

        data = b''
        while True:
            try:
                part = sock.recv(4096)
            except socket.timeout:
                break

            if not part:
                break

            data += part

    finally:
        sock.close()

    return data.decode('utf-8', errors='ignore')


def check_bodyless(raw, status):
    """Assert the exact wire shape: bodyless header, then the whole GET /ok.

    Splitting on the header terminator pins both halves at once -- a 502 in
    place of the first response, a body leaking out of it, or a missing second
    response all change the number of parts or their content.
    """

    parts = raw.split('\r\n\r\n')

    assert len(parts) == 3, f'expected two responses, got {raw!r}'

    first, second, body = parts

    assert first.startswith(
        f'HTTP/1.1 {status}'
    ), f'first response status line: {first!r}'
    assert (
        'transfer-encoding' not in first.lower()
    ), f'Transfer-Encoding relayed to the client: {first!r}'
    assert (
        'close' not in first.lower()
    ), f'keep-alive dropped on a bodyless response: {first!r}'

    assert second.startswith(
        'HTTP/1.1 200'
    ), f'pipelined request lost: {second!r}'
    assert body == 'OK', f'pipelined response body: {body!r}'

    return first


def test_proxy_te_204_undecodable():
    """204 with "Transfer-Encoding: gzip": bodyless, not 502."""

    first = check_bodyless(pipeline('GET', '/204-gzip'), 204)

    assert (
        'content-length' not in first.lower()
    ), f'Content-Length added to a 204: {first!r}'


def test_proxy_te_304_undecodable():
    """304 with "Transfer-Encoding: identity": bodyless, not 502."""

    check_bodyless(pipeline('GET', '/304-identity'), 304)


def test_proxy_te_head_undecodable():
    """HEAD answered with "Transfer-Encoding: chunked;a=b": bodyless, not 502.

    A coding with parameters is not the bare "chunked" the proxy decodes, so
    on any other status this is a 502 -- see test_proxy_te_bad_gateway's
    /chunked-param.
    """

    check_bodyless(pipeline('HEAD', '/head-param'), 200)


def test_proxy_te_204_chunked():
    """204 with exact "chunked": unchanged by the check ordering.

    This case took the normal branch before the 502 checks moved and still
    completes as bodyless, so it guards the move itself.
    """

    check_bodyless(pipeline('GET', '/204-chunked'), 204)


def test_proxy_te_304_chunked_content_length():
    """304 with chunked and Content-Length: bodyless, not 502.

    The chunked-plus-Content-Length 502 predates this change, and it fired
    above the bodyless branch.  RFC 9112 Sect. 6.3 ends this response at the
    first empty line, so the two fields frame nothing and cannot disagree.
    """

    first = check_bodyless(pipeline('GET', '/304-chunked-cl'), 304)

    # RFC 9110 Sect. 9.3.2 / Sect. 15.4.5: a 304 reports the length the
    # cached representation has.
    assert (
        'Content-Length: 10' in first
    ), f'304 keeps Content-Length: {first!r}'


def test_proxy_te_bad_gateway_still_fires():
    """Control: an undecodable Transfer-Encoding on a status that has a body.

    test_proxy_te_bad_gateway covers the whole matrix; this pins the one shape
    the bodyless cases above are the exemption from, over the same pipeline,
    so the 502 and the exemption are read side by side.
    """

    raw = pipeline('GET', '/gzip-chunked')

    assert raw.startswith('HTTP/1.1 502'), f'status: {raw!r}'
    assert 'hello' not in raw, f'upstream body not relayed: {raw!r}'
