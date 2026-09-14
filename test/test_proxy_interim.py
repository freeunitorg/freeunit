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
Unit writes one header per request.

Each case pipelines a GET /ok behind the request under test, as
test_proxy_head.py does: the second response arriving intact is what shows the
final response was not lost or misattributed.
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


def pipeline(first_request):
    """Send first_request and a GET /ok back to back, read to EOF."""

    request = first_request + (
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
