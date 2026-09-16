"""Proxied upstream 1xx interim responses (RFC 9110 Sect. 15.2).

nxt_h1p_peer_header_parse() reads one status line per upstream response and
took the first one as the response.  A 1xx is interim: the final response
follows it on the same connection.  So "100 Continue" or "103 Early Hints"
from the upstream was relayed to the client as the response, and the final
response that came after it was relayed as that response's body -- or, when
the client had pipelined a second request, taken for its answer.

Unit draws a 100 itself: nxt_h1p_peer_header_send() forwards the client's
"Expect: 100-continue" verbatim (no handler in nxt_h1p_fields[] and not
hop-by-hop), and an upstream that honours it answers 100 before the final
response.  A 103 needs no prompting.

The fix drops every 1xx other than 101 and reads on for the final response.
Nothing is relayed to the client: the request body was sent to the upstream in
full, so there is nothing for the client to continue, and the client side of
Unit writes one header per request.  An upstream that sends more than ten
interim responses in one exchange is looping, and the request fails with 502
rather than letting it make Unit parse and store interim headers without bound.

Each case pipelines a GET /ok behind the request under test, as
test_proxy_head.py does: the second response arriving intact is what shows the
final response was not lost or misattributed.  Cases whose exchange ends in an
error, or in a protocol switch, close the client connection instead of
answering the pipelined request; those send the request under test alone.
"""

import select
import socket
import time

import pytest

from conftest import run_process
from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# Reserved in test/fake_upstream/README.md's port registry.
UPSTREAM_PORT = 7977

FINAL = 'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK'

# The same final response with a marker field, to show the final response's own
# fields survive what the interim response left behind.
MARKED_FINAL = (
    'HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Final: yes\r\n\r\nOK'
)

# A 103 with one Link field, the shape a real origin sends.
INTERIM = 'HTTP/1.1 103 Early Hints\r\nLink: </x.css>; rel=preload\r\n\r\n'

# More fields than nxt_http_request_parse_t holds inline (16), so the parser
# puts the rest in an nxt_list allocated from the request pool; dropping the
# interim response has to drop both.
BIG_FIELD_COUNT = 20

# Field names a relayed 200 may carry here: the upstream's own, plus what Unit
# adds.  Any other name in the first response's header block is a field of the
# interim response that was not dropped.
FINAL_FIELDS = {
    'Content-Length', 'X-Final', 'Server', 'Date', 'Connection',
    'Transfer-Encoding',
}

# A 103 whose header block alone is over half of the 64 KiB
# proxy_header_buffer_size.  Two of them before the final response only fit
# if the bytes of a consumed interim response are reclaimed.
BIG_HINTS = 'HTTP/1.1 103 Early Hints\r\n' + (
    ''.join(
        f'Link: </static/{i:04d}.css>; rel=preload; as=style\r\n'
        for i in range(700)
    )
) + '\r\n'

assert 32 * 1024 < len(BIG_HINTS) < 64 * 1024, len(BIG_HINTS)

# What the upstream sends, keyed by the request target Unit forwards.  A list
# is written in separate sends with a pause between them, so each part lands
# in its own read on Unit's side; a string goes out in one write.
UPSTREAM_RESPONSES = {
    # Interim and final in one write: the final response is already in the
    # buffer the interim header was parsed from.
    '/continue': 'HTTP/1.1 100 Continue\r\n\r\n' + FINAL,
    # The final response arrives in a later read than the interim one.
    '/hints': [
        'HTTP/1.1 103 Early Hints\r\n'
        'Link: </style.css>; rel=preload; as=style\r\n\r\n',
        FINAL,
    ],
    # Several interim responses, split across reads.
    '/multi': [
        'HTTP/1.1 100 Continue\r\n\r\n',
        'HTTP/1.1 103 Early Hints\r\nLink: </a.css>; rel=preload\r\n\r\n',
        'HTTP/1.1 103 Early Hints\r\nLink: </b.css>; rel=preload\r\n\r\n'
        + FINAL,
    ],
    '/big': BIG_HINTS + BIG_HINTS + FINAL,
    # More fields than the inline array holds: the ones the parser moved to its
    # list must be dropped with the interim response, not relayed as the final
    # response's own fields.
    '/fields': (
        'HTTP/1.1 103 Early Hints\r\n'
        + ''.join(f'X-Early-{i:02d}: v{i}\r\n' for i in range(BIG_FIELD_COUNT))
        + 'Link: </leak.css>; rel=preload\r\n'
        + '\r\n'
        + MARKED_FINAL
    ),
    # Framing fields on an interim response (RFC 9112 Sect. 6.1 forbids them
    # there) must not reach the final response either.
    '/framing': (
        'HTTP/1.1 103 Early Hints\r\nContent-Length: 3\r\n'
        'Connection: close\r\nLink: </x.css>; rel=preload\r\n\r\n' + FINAL
    ),
    # Exactly the cap, and one past it.
    '/ten': INTERIM * 10 + FINAL,
    '/eleven': INTERIM * 11 + FINAL,
    # 101 is not interim in this sense -- the connection changes protocol -- so
    # it stays the response.
    '/switch': 'HTTP/1.1 101 Switching Protocols\r\n\r\n',
    # An interim response with no final response behind it.
    '/truncated': 'HTTP/1.1 100 Continue\r\n\r\n',
    # 100 only after the body has been read, like a real upstream honouring
    # Expect; the final response reports how many body bytes it got.
    '/expect': None,
    # The trailing request of every pipeline.
    '/ok': FINAL,
}


def run_server(server_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.bind(('127.0.0.1', server_port))
    sock.listen(10)

    def recv_until(conn, data, done):
        while not done(data):
            if not select.select([conn], [], [], 5)[0]:
                break

            part = conn.recv(65536)
            if not part:
                break

            data += part

        return data

    while True:
        conn, _ = sock.accept()

        data = recv_until(conn, b'', lambda d: b'\r\n\r\n' in d)
        request = data.decode('utf-8', errors='ignore')
        target = request.split(' ')[1] if ' ' in request else ''

        if target == '/expect':
            header, _, body = data.partition(b'\r\n\r\n')
            length = 0

            for line in header.split(b'\r\n'):
                if line.lower().startswith(b'content-length:'):
                    length = int(line.split(b':', 1)[1])

            body = recv_until(conn, body, lambda d: len(d) >= length)

            conn.sendall(b'HTTP/1.1 100 Continue\r\n\r\n')
            time.sleep(0.1)

            reply = f'got {len(body)}'.encode()
            conn.sendall(
                b'HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%s'
                % (len(reply), reply)
            )

        else:
            response = UPSTREAM_RESPONSES.get(
                target, 'HTTP/1.1 500 Internal Server Error\r\n\r\n'
            )

            if isinstance(response, str):
                response = [response]

            for i, part in enumerate(response):
                if i > 0:
                    time.sleep(0.1)

                conn.sendall(part.encode())

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
    ), 'interim proxy configuration'


def pipeline(first_request, sentinel=True):
    """Send first_request, with a GET /ok behind it unless sentinel is false,
    and read to EOF."""

    request = first_request

    if sentinel:
        request += (
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


def get(target):
    return pipeline(
        f'GET {target} HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Connection: keep-alive\r\n'
        '\r\n'
    )


def check(raw, body='OK'):
    """Two 200 responses, nothing else: no 1xx status line reaches the client,
    the first response carries the upstream's final body, the pipelined GET
    /ok gets its own answer."""

    assert 'HTTP/1.1 1' not in raw, f'1xx relayed to the client: {raw!r}'

    parts = raw.split('\r\n\r\n')

    assert len(parts) == 3, f'expected two responses, got {raw!r}'

    first, first_body_and_second, second_body = parts

    assert first.startswith('HTTP/1.1 200'), f'first status: {first!r}'
    assert first_body_and_second.startswith(
        body
    ), f'first body: {first_body_and_second!r}'
    assert (
        'HTTP/1.1 200' in first_body_and_second
    ), f'pipelined request lost: {first_body_and_second!r}'
    assert second_body == 'OK', f'pipelined body: {second_body!r}'


def test_proxy_interim_continue_same_read():
    check(get('/continue'))


def test_proxy_interim_early_hints_separate_read():
    check(get('/hints'))


def test_proxy_interim_several():
    check(get('/multi'))


def test_proxy_interim_buffer_reclaimed():
    check(get('/big'))


def test_proxy_interim_expect_continue():
    """A client's Expect is forwarded and answered upstream; the client sees
    only the final response, with the body the upstream actually got."""

    body = 'x' * 4096

    raw = pipeline(
        'POST /expect HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Connection: keep-alive\r\n'
        'Expect: 100-continue\r\n'
        f'Content-Length: {len(body)}\r\n'
        '\r\n' + body
    )

    check(raw, body=f'got {len(body)}')


def test_proxy_interim_control():
    """Control: a plain final response is unchanged."""

    check(get('/ok'))


def test_proxy_interim_fields_not_leaked():
    """A 1xx with more fields than the parser keeps inline: the fields it
    collected -- including the ones it moved to its list -- are dropped with
    the interim response, not relayed as the final response's own."""

    raw = get('/fields')

    check(raw)

    lines = raw.split('\r\n\r\n')[0].split('\r\n')
    names = [line.split(':', 1)[0] for line in lines[1:]]

    assert 'X-Final: yes' in raw, f'final field lost: {raw!r}'
    assert not [
        name for name in names if name not in FINAL_FIELDS
    ], f'interim fields leaked into the final response: {lines!r}'
    assert 'X-Early' not in raw, f'interim field leaked: {raw!r}'


def test_proxy_interim_framing_fields_dropped():
    """Content-Length and Connection on a 1xx are dropped with it, so the final
    response keeps its own framing and its own body."""

    raw = get('/framing')

    check(raw)

    assert 'Content-Length: 3' not in raw, f'1xx framing relayed: {raw!r}'


def test_proxy_interim_101_is_the_response():
    """101 is not interim in this sense -- the connection changes protocol --
    so it is relayed as the response, as it was before."""

    raw = pipeline(
        'GET /switch HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Connection: close\r\n'
        '\r\n',
        sentinel=False,
    )

    assert raw.startswith(
        'HTTP/1.1 101 Switching Protocols'
    ), f'101 not relayed: {raw!r}'


def test_proxy_interim_then_close_is_bad_gateway():
    """An interim response and then the upstream closing is a truncated
    exchange: consuming the 1xx clears peer->status, and the close still has to
    be an error rather than an empty success."""

    raw = pipeline(
        'GET /truncated HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Connection: close\r\n'
        '\r\n',
        sentinel=False,
    )

    assert 'HTTP/1.1 502' in raw, f'expected 502, got: {raw!r}'


def test_proxy_interim_at_the_limit():
    """Ten interim responses are consumed and the final response relayed; the
    cap counts the eleventh."""

    check(get('/ten'))


def test_proxy_interim_over_the_limit():
    """Eleven interim responses are an upstream protocol violation: the request
    fails with 502 instead of parsing interim headers without bound."""

    raw = pipeline(
        'GET /eleven HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Connection: close\r\n'
        '\r\n',
        sentinel=False,
    )

    assert 'HTTP/1.1 502' in raw, f'expected 502 past the cap: {raw!r}'
    assert 'HTTP/1.1 200' not in raw, f'final relayed past the cap: {raw!r}'
