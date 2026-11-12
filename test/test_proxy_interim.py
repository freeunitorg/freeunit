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
from unit import port as port_map
from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# Reserved in test/fake_upstream/README.md's port registry.
UPSTREAM_PORT = port_map.port(7977)

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

# The header buffer is 64 KiB.  The first part is a 1xx that is not complete
# and leaves 100 bytes of the buffer free.  The second part is exactly 100
# bytes: the end of the 1xx and the start of the final header.  So a read
# fills the buffer.  The proxy drops the 1xx, which frees space in the
# buffer, and reads again at once.  That read gets nothing, because the
# upstream pauses before the rest of the final header.  The proxy must keep
# the buffer while it waits: the parser points into it.
FILL_FIRST = 64 * 1024 - 100

FILL_MARKER = 'intact-final-value-0123456789'

FILL_HEAD = (
    f'HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Final: {FILL_MARKER}\r\n'
    'X-Pad: '
)


def fill_parts(pad_length):
    """The 1xx ends 4 bytes into the second part.  The X-Pad value of the
    final header starts in the second part and is "pad_length" bytes."""

    hints = 'HTTP/1.1 103 Early Hints\r\nX-Hint-Pad: '
    hints += 'h' * (FILL_FIRST - len(hints)) + '\r\n\r\n'
    response = hints + FILL_HEAD + 'a' * pad_length + '\r\n\r\nOK'
    second = FILL_FIRST + 100

    return [
        response[:FILL_FIRST],
        response[FILL_FIRST:second],
        response[second:],
    ]


# The final header alone fits in the buffer.
FILL_PAD = 4096
FILL = fill_parts(FILL_PAD)

# The final header alone is bigger than the buffer.
FILL_OVERSIZED = fill_parts(64 * 1024)

assert FILL[1].startswith('\r\n\r\n' + FILL_HEAD) and len(FILL[1]) == 100
assert FILL[1].endswith('a') and FILL_OVERSIZED[1] == FILL[1]

# Seconds between the parts of these targets.  The default is 0.1.
PAUSE = {'/fill': 0.4, '/fill-oversized': 0.4}

# 101 for a WebSocket handshake.
WS_SWITCH = (
    'HTTP/1.1 101 Switching Protocols\r\n'
    'Upgrade: websocket\r\nConnection: Upgrade\r\n'
    'Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n'
)

# Targets whose upstream connection stays open after the 101.
WS_HELD = ('/ws', '/ws-frame')

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
    # 101 for a WebSocket handshake, alone and with a server frame after it
    # in the same write.  The proxy cannot pass WebSocket: 502.
    '/ws': WS_SWITCH,
    '/ws-frame': WS_SWITCH + 'hello',
    # An upstream that refuses the handshake.
    '/ws-refuse': 'HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n',
    # A 1xx without a final response.
    '/truncated': 'HTTP/1.1 100 Continue\r\n\r\n',
    # A 1xx bigger than the header buffer.
    '/oversized': OVERSIZED_HINTS + FINAL,
    # The 1xx and the start of the final header fill the header buffer.
    '/fill': FILL,
    # The same, with a final header bigger than the header buffer.
    '/fill-oversized': FILL_OVERSIZED,
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
                        time.sleep(PAUSE.get(target, 0.1))

                    conn.sendall(part.encode())

            except OSError:
                # Unit closed the connection before reading it all.
                pass

            if target in WS_HELD:
                # Keep the connection open, as a WebSocket server does, until
                # Unit closes it or the client frame has had time to arrive.
                select.select([conn], [], [], 3)

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
        sock.connect(('127.0.0.1', port_map.port(8080)))
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


def handshake(target):
    """Send a valid WebSocket handshake.  If a 101 comes back, send one masked
    text frame.  Return what the client read."""

    request = (
        f'GET {target} HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Upgrade: websocket\r\n'
        'Connection: Upgrade\r\n'
        'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n'
        'Sec-WebSocket-Version: 13\r\n'
        '\r\n'
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)

    data = b''

    try:
        sock.connect(('127.0.0.1', port_map.port(8080)))
        sock.sendall(request.encode())

        while b'\r\n\r\n' not in data:
            part = sock.recv(4096)
            if not part:
                break

            data += part

        if data.startswith(b'HTTP/1.1 101'):
            mask = b'\x01\x02\x03\x04'
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(b'hello'))
            sock.sendall(b'\x81\x85' + mask + payload)

        # A 502 keeps the connection open, so stop after a short wait.
        sock.settimeout(1)

        while True:
            part = sock.recv(4096)
            if not part:
                break

            data += part

    except (socket.timeout, ConnectionError):
        pass

    finally:
        sock.close()

    return data.decode('latin-1')


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
    """The upstream sends a 100 for /expect, and the proxy drops it.  The
    client gets one 100 only: the router sends it before it reads the rest
    of the body."""

    body = 'x' * 4096

    raw = pipeline(
        'POST /expect HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Connection: keep-alive\r\n'
        'Expect: 100-continue\r\n'
        f'Content-Length: {len(body)}\r\n'
        '\r\n' + body
    )

    # The first read holds at most header_buffer_size (2048) bytes, so the
    # router always waits for the rest of the body.
    router_continue = 'HTTP/1.1 100 Continue\r\n\r\n'

    assert raw.startswith(router_continue), f'no 100 from the router: {raw!r}'

    check(raw[len(router_continue) :], body=f'got {len(body)}')


def test_proxy_interim_fill_header_buffer():
    """A dropped 1xx frees space in a full header buffer.  The upstream then
    pauses in the final header.  The proxy kept the buffer for the parser
    only while bytes arrived: on a read that got nothing it freed the
    buffer, and the next read parsed freed memory.  Now the buffer is kept,
    and the final response arrives whole."""

    raw = get('/fill')

    check(raw)

    assert f'X-Final: {FILL_MARKER}\r\n' in raw, f'field lost: {raw[:200]!r}'
    assert (
        f'X-Pad: {"a" * FILL_PAD}\r\n' in raw
    ), f'X-Pad value changed: {raw[:200]!r}'


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


@pytest.mark.parametrize('target', ['/ws', '/ws-frame'])
def test_proxy_interim_101_websocket_is_bad_gateway(target):
    """The proxy cannot pass WebSocket.  It used to start the WebSocket frame
    reader of the client connection, and the first client frame crashed the
    router.  The upstream keeps its connection open after the 101, so the
    frame reaches a live proxy request.  The '/ws-frame' target sends server
    bytes in the same read as the 101.  The test cannot see a stranded
    buffer; that was measured with the debug log of the request pool."""

    raw = handshake(target)

    # The router still serves requests.  Check it before the status, so a
    # crash is reported even when the status assertion fails.
    alive = get_alone('/ok').startswith('HTTP/1.1 200')

    assert raw.startswith('HTTP/1.1 502'), f'expected 502: {raw[:200]!r}'
    assert alive, 'router lost after 101'


def test_proxy_interim_websocket_refused_is_relayed():
    raw = handshake('/ws-refuse')

    assert raw.startswith('HTTP/1.1 400'), f'expected 400: {raw[:200]!r}'


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


def test_proxy_interim_fill_oversized():
    """The same start as test_proxy_interim_fill_header_buffer(), but the
    final header does not fit in the buffer.  The proxy answers 502 and
    sends no byte of the partial header."""

    raw = get_alone('/fill-oversized')

    assert raw.startswith('HTTP/1.1 502'), f'expected 502: {raw[:200]!r}'

    for part in ('HTTP/1.1 200', 'X-Final', FILL_MARKER, 'X-Pad', 'a' * 16):
        assert part not in raw, f'partial header relayed: {raw[:200]!r}'
