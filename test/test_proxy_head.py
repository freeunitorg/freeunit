"""Proxied responses that RFC 9112 Sect. 6.3 defines as having no body.

nxt_h1p_peer_header_read_done() used to arm the upstream body framing from the
upstream headers alone: Content-Length became h1p->remainder, Transfer-Encoding
armed the chunked parser.  A response to HEAD, and any 204 or 304 response,
carries no body no matter what those headers say, so the upstream -- which
nxt_h1p_peer_header_send() always asks to "Connection: close" -- closed with the
remainder still outstanding.  nxt_h1p_peer_closed() read that as a truncated
body and set r->truncated/r->inconsistent, and nxt_h1p_request_close() then
dropped the client keep-alive, losing any pipelined follow-up request.

Each case pipelines two requests in a single write: the bodyless one, then a
plain GET.  The second response arriving at all is the regression assertion.

1xx is out of scope here: an upstream 1xx is an interim response, and nothing
in the peer reader continues the exchange past it (that is pre-existing, see
nxt_h1p_peer_header_parse), so it is left on the path it already had.

These cases also cover the two exits an unrelayed upstream header buffer can
take, but they cannot assert that it is released: a stranded request pool is
invisible from the client side, and a stock build exposes no pool counter --
nxt_debug is never assigned, so even a --debug build emits no "mp ... release"
lines.  That was measured instead with a throwaway build that forces nxt_debug
on and counts pools reaching a zero retain per request; see the commit message.

See freeunitorg/freeunit#283 for the downstream half of the same rule.
"""

import select
import socket

import pytest

from conftest import run_process
from unit.applications.proto import ApplicationProto
from unit.option import option
from unit.utils import waitforsocket

client = ApplicationProto()

# Reserved in test/fake_upstream/README.md's port registry; 7980-7982
# belong to test/fake_otlp.
UPSTREAM_PORT = 7978

# What the upstream answers, keyed by the request target Unit forwards.
# Every one of these closes the connection right after writing what is here:
# none of them ever sends the body its own headers advertise.
UPSTREAM_RESPONSES = {
    '/cl10': 'HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n',
    '/chunked': 'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n',
    '/204': 'HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\n',
    '/304': 'HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n',
    # Header block and 10 junk bytes in a single write, so the bytes land in
    # the same read as the header.  A response to HEAD has no body, so they
    # must be dropped rather than relayed -- and the buffer holding them takes
    # a different exit from nxt_h1p_peer_header_read_done() than the cases
    # above, which is where an earlier revision of this change stranded the
    # request pool.
    '/cl10junk': (
        'HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nJUNKJUNKJU'
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
    ), 'bodyless proxy configuration'


def pipeline(method, target):
    """Send "<method> <target>" and a GET /ok back to back, read to EOF.

    Both requests leave in one write, so the second is already sitting in the
    listener's read buffer when the first response is generated: it can only be
    answered if the connection survives that response.
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


def check_pipeline(raw, status):
    """Assert the exact wire shape: bodyless header, then the whole GET /ok.

    Splitting on the header terminator pins both halves at once -- a body
    leaking out of the first response, a chunked terminator appended to it, or
    a missing second response all change the number of parts or their content.
    """

    parts = raw.split('\r\n\r\n')

    assert len(parts) == 3, f'expected two responses, got {raw!r}'

    first, second, body = parts

    assert first.startswith(
        f'HTTP/1.1 {status}'
    ), f'first response status line: {first!r}'
    assert (
        'close' not in first.lower()
    ), f'keep-alive dropped on a bodyless response: {first!r}'
    assert (
        'transfer-encoding' not in first.lower()
    ), f'chunked framing on a bodyless response: {first!r}'

    assert second.startswith(
        'HTTP/1.1 200'
    ), f'pipelined request lost: {second!r}'
    assert body == 'OK', f'pipelined response body: {body!r}'

    return first


def test_proxy_head_content_length():
    """HEAD: the upstream advertises a body and sends none, then closes."""

    first = check_pipeline(pipeline('HEAD', '/cl10'), 200)

    # RFC 9110 Sect. 9.3.2: a HEAD response keeps the length the equivalent
    # GET would report.
    assert 'Content-Length: 10' in first, f'HEAD keeps Content-Length: {first!r}'


def test_proxy_head_chunked():
    """HEAD: the upstream announces chunked and sends no terminating chunk."""

    check_pipeline(pipeline('HEAD', '/chunked'), 200)


def test_proxy_head_trailing_bytes():
    """HEAD: the upstream writes the header and 10 junk bytes together.

    A response to HEAD has no body, so those bytes are not one: they must not
    reach the client and must not be counted as a body.  They also put the
    header buffer on the branch that used to hand it to
    nxt_h1p_peer_body_process(), whose completion released it -- so this is the
    case where simply returning early strands the buffer and, with it, the
    request pool.  That leak is not observable from here (see the module
    docstring); what is checked is that the bytes are dropped and the
    connection survives.
    """

    first = check_pipeline(pipeline('HEAD', '/cl10junk'), 200)

    assert 'Content-Length: 10' in first, f'HEAD keeps Content-Length: {first!r}'


def test_proxy_get_204():
    """204: Content-Length is advertised, no body follows, upstream closes."""

    first = check_pipeline(pipeline('GET', '/204'), 204)

    # RFC 9110 Sect. 8.6 forbids Content-Length on a 204; #283 drops it.
    assert (
        'content-length' not in first.lower()
    ), f'Content-Length kept on 204: {first!r}'


def test_proxy_get_304():
    """304: Content-Length describes the cached body and is kept."""

    first = check_pipeline(pipeline('GET', '/304'), 304)

    assert 'Content-Length: 10' in first, f'304 keeps Content-Length: {first!r}'


def test_proxy_head_response_header_variable(wait_for_record):
    """The relayed header bytes stay readable after the response completes.

    peer->fields point their name/value into the buffer the upstream header was
    parsed from, and nxt_http_proxy_header_read() shallow-copies those field
    structs into r->resp; $response_header_* reads them at access-log time.
    This exercises that path on the early-complete branch, where the peer
    connection is already closed by the time the record is written.

    It is coverage, not a use-after-free detector: returning that buffer to the
    engine cache early leaves the pointers dangling but does not reliably
    corrupt them, so this passes either way.  The buffer must simply not be
    freed there -- which is also what the normal path does.
    """

    assert 'success' in client.conf(
        {
            'path': f'{option.temp_dir}/access.log',
            'format': '$request_line "$response_header_content_length"\n',
        },
        'access_log',
    ), 'access_log format'

    check_pipeline(pipeline('HEAD', '/cl10'), 200)

    assert (
        wait_for_record(r'HEAD /cl10 HTTP/1.1 "10"', 'access.log') is not None
    ), 'Content-Length readable from the access log after the response'


def test_proxy_get_body_still_relayed():
    """Control: a status that does define a body still gets one."""

    raw = pipeline('GET', '/ok')

    assert raw.count('HTTP/1.1 200') == 2, f'expected two responses: {raw!r}'
    assert raw.count('\r\n\r\nOK') == 2, f'both bodies relayed: {raw!r}'
