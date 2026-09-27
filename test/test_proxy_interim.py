"""Proxied upstream 1xx interim responses (RFC 9110 Sect. 15.2).

The proxy drops every 1xx except 101 and sends only the final response.
More than ten 1xx responses, or one bigger than the header buffer, give 502.

Most cases send a second request (GET /ok) on the same connection.  If its
response is correct, the first final response was not lost.  Cases that end
with an error or 101 send one request only.
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

# A final response with its own field.
MARKED_FINAL = (
    'HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Final: yes\r\n\r\nOK'
)

# A typical 103.
INTERIM = 'HTTP/1.1 103 Early Hints\r\nLink: </x.css>; rel=preload\r\n\r\n'

# More than the 16 inline fields of the parser.
BIG_FIELD_COUNT = 20

# The only fields the final response may have.
FINAL_FIELDS = {
    'Content-Length', 'X-Final', 'Server', 'Date', 'Connection',
    'Transfer-Encoding',
}

# A 103 bigger than half of the 64 KiB header buffer.  Two of them fit only
# if the proxy frees the bytes of each dropped 1xx.
BIG_HINTS = 'HTTP/1.1 103 Early Hints\r\n' + (
    ''.join(
        f'Link: </static/{i:04d}.css>; rel=preload; as=style\r\n'
        for i in range(700)
    )
) + '\r\n'

assert 32 * 1024 < len(BIG_HINTS) < 64 * 1024, len(BIG_HINTS)

# A 103 bigger than the header buffer.
OVERSIZED_HINTS = 'HTTP/1.1 103 Early Hints\r\n' + (
    ''.join(f'X-Pad-{i:02d}: {"a" * 1000}\r\n' for i in range(70))
) + '\r\n'

assert len(OVERSIZED_HINTS) > 64 * 1024, len(OVERSIZED_HINTS)

# Upstream response for each target.  A string is sent in one write.  Each
# item of a list is sent separately, with a pause, so Unit reads it separately.
UPSTREAM_RESPONSES = {
    # 1xx and final response in one read.
    '/continue': 'HTTP/1.1 100 Continue\r\n\r\n' + FINAL,
    # Final response in a later read.
    '/hints': [
        'HTTP/1.1 103 Early Hints\r\n'
        'Link: </style.css>; rel=preload; as=style\r\n\r\n',
        FINAL,
    ],
    # Several 1xx responses in several reads.
    '/multi': [
        'HTTP/1.1 100 Continue\r\n\r\n',
        'HTTP/1.1 103 Early Hints\r\nLink: </a.css>; rel=preload\r\n\r\n',
        'HTTP/1.1 103 Early Hints\r\nLink: </b.css>; rel=preload\r\n\r\n'
        + FINAL,
    ],
    '/big': BIG_HINTS + BIG_HINTS + FINAL,
    # The empty line of the 1xx is split between two reads.
    '/split': [INTERIM[:-1], '\n' + FINAL],
    # Fields of the 1xx must not appear in the final response.
    '/fields': (
        'HTTP/1.1 103 Early Hints\r\n'
        + ''.join(f'X-Early-{i:02d}: v{i}\r\n' for i in range(BIG_FIELD_COUNT))
        + 'Link: </leak.css>; rel=preload\r\n'
        + '\r\n'
        + MARKED_FINAL
    ),
    # Framing fields on a 1xx (not allowed, RFC 9112, 6.1) are dropped too.
    '/framing': (
        'HTTP/1.1 103 Early Hints\r\nContent-Length: 3\r\n'
        'Connection: close\r\nLink: </x.css>; rel=preload\r\n\r\n' + FINAL
    ),
    # Exactly the cap, and one past it.
    '/ten': INTERIM * 10 + FINAL,
    '/eleven': INTERIM * 11 + FINAL,
    # 101 changes the protocol, so it is the response.
    '/switch': 'HTTP/1.1 101 Switching Protocols\r\n\r\n',
    # A 1xx without a final response.
    '/truncated': 'HTTP/1.1 100 Continue\r\n\r\n',
    # A 1xx bigger than the header buffer.
    '/oversized': OVERSIZED_HINTS + FINAL,
    # The second request.
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
            # Read the body, send 100, then send how many bytes were read.
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

            try:
                for i, part in enumerate(response):
                    if i > 0:
                        time.sleep(0.1)

                    conn.sendall(part.encode())

            except OSError:
                # Unit closed the connection before reading it all.
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
    ), 'interim proxy configuration'


def pipeline(first_request, sentinel=True):
    """Send first_request, then GET /ok if sentinel is true.  Read to EOF."""

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
    """Exactly two 200 responses, with the expected bodies, and no 1xx."""

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


def get_alone(target):
    return pipeline(
        f'GET {target} HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Connection: close\r\n'
        '\r\n',
        sentinel=False,
    )


@pytest.mark.parametrize(
    'target',
    [
        '/ok',  # control
        '/continue',  # interim and final in one read
        '/hints',  # final in a later read
        '/multi',  # several, split across reads
        '/big',  # fits only if the 1xx bytes are freed
        '/split',  # empty line of the 1xx in two reads
        '/framing',  # framing fields of the 1xx are dropped
        '/ten',  # exactly the cap
    ],
)
def test_proxy_interim_consumed(target):
    raw = get(target)

    check(raw)

    assert 'Content-Length: 3' not in raw, f'1xx framing relayed: {raw!r}'


def test_proxy_interim_expect_continue():
    """The upstream answers Expect with 100; the client gets only the final
    response."""

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


def test_proxy_interim_fields_not_leaked():
    """Fields of a 1xx, also those after the 16 inline fields, are dropped."""

    raw = get('/fields')

    check(raw)

    lines = raw.split('\r\n\r\n')[0].split('\r\n')
    names = [line.split(':', 1)[0] for line in lines[1:]]

    assert 'X-Final: yes' in raw, f'final field lost: {raw!r}'
    assert not [
        name for name in names if name not in FINAL_FIELDS
    ], f'interim fields leaked into the final response: {lines!r}'


def test_proxy_interim_101_is_the_response():
    """101 changes the protocol, so it is the response."""

    raw = get_alone('/switch')

    assert raw.startswith(
        'HTTP/1.1 101 Switching Protocols'
    ), f'101 not relayed: {raw!r}'


@pytest.mark.parametrize(
    'target',
    [
        '/truncated',  # upstream closes after a 1xx
        '/eleven',  # one past the cap
        '/oversized',  # a 1xx bigger than the buffer
    ],
)
def test_proxy_interim_bad_gateway(target):
    raw = get_alone(target)

    assert raw.startswith('HTTP/1.1 502'), f'expected 502: {raw[:200]!r}'
    assert 'HTTP/1.1 200' not in raw, f'final relayed: {raw[:200]!r}'
