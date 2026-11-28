"""The WebSocket read handler keeps a partial frame header that it did not
allocate.

A partial frame header waits in c->read for the next read.  When the next
read returns no data, the handler must not free that buffer with
nxt_mp_free(c->mem_pool, b): the bytes of the header are lost, and the
buffer can come from another pool.  Each test prints what it saw (run
with -s).
"""

import socket
import ssl
import time

from packaging import version

from unit import port as port_map
from unit.applications.lang.python import ApplicationPython
from unit.applications.tls import ApplicationTLS
from unit.applications.websockets import ApplicationWebsocket
from unit.log import Log
from unit.status import Status

prerequisites = {
    'modules': {'python': lambda v: version.parse(v) >= version.parse('3.5')}
}

client = ApplicationPython(load_module='asgi')
ws = ApplicationWebsocket()
tls = ApplicationTLS()

ALERT = r'freed pointer is out of pool'


def handshake(key):
    return (
        'GET / HTTP/1.1\r\n'
        'Host: localhost\r\n'
        'Upgrade: websocket\r\n'
        'Connection: Upgrade\r\n'
        f'Sec-WebSocket-Key: {key}\r\n'
        'Sec-WebSocket-Version: 13\r\n\r\n'
    ).encode()


def read_upgrade(sock):
    resp = b''
    while b'\r\n\r\n' not in resp:
        chunk = sock.recv(4096)
        assert chunk, 'connection closed before the 101 response'
        resp += chunk

    assert resp.startswith(b'HTTP/1.1 101'), resp

    return resp.split(b'\r\n\r\n', 1)[1]


def read_exact(sock, size, data=b''):
    """Read with recv() only: select() does not see the bytes that the TLS
    library has already taken from the socket."""
    while len(data) < size:
        chunk = sock.recv(4096)
        assert chunk, f'connection closed after {data!r}'
        data += chunk

    return data


def echo_of(message):
    return b'\x81' + bytes([len(message)]) + message


def test_asgi_websockets_wrong_pool_free():
    """The first byte of a frame comes in the same read as the handshake,
    then the client closes the connection."""
    client.load('websockets/mirror')

    Status.init()

    n = 32

    for _ in range(n):
        sock = socket.create_connection(('127.0.0.1', port_map.port(8080)))
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(10)

        # The handshake and the first byte of a masked text frame.
        sock.sendall(handshake(ws.key()) + b'\x81')

        read_upgrade(sock)

        # The next read of the frame header returns end of file.
        sock.close()

    # Wait until the router has closed every connection.
    for _ in range(200):
        if Status.get('/connections/closed') >= n:
            break

        time.sleep(0.1)

    closed = Status.get('/connections/closed')
    found = Log.findall(ALERT)
    print(f'connections closed {closed}, alerts "{ALERT}": {len(found)}')

    assert closed >= n, 'connections closed'
    assert found == [], 'no out-of-pool free after a partial-frame close'


def test_asgi_websockets_partial_header_exact_read():
    """A read that fills the 135-byte buffer exactly ends inside the header
    of the second frame.  The next read of the router then gets no data,
    and the 4 header bytes must stay for the read after it."""
    client.load('websockets/mirror')

    _, sock, _ = ws.upgrade()
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(10)

    first = b'a' * 125
    second = b'second frame'

    frame1 = ws.frame_to_send(ws.OP_TEXT, first)
    frame2 = ws.frame_to_send(ws.OP_TEXT, second)

    assert len(frame1) + 4 == 135

    sock.sendall(frame1 + frame2[:4])

    data = read_exact(sock, len(echo_of(first)))
    assert data == echo_of(first), 'echo of the first frame'

    time.sleep(0.5)
    sock.sendall(frame2[4:])

    data = read_exact(sock, len(echo_of(second)))
    print(f'exact read: second echo {data!r}')

    assert data == echo_of(second), 'echo of the second frame'

    sock.close()


def test_asgi_websockets_tls_split_frame_header(require):
    """A frame header split over two TLS records reaches the application."""
    require({'modules': {'openssl': 'any'}})

    client.load('websockets/mirror', name='mirror')

    tls.certificate()

    assert 'success' in client.conf(
        {
            "pass": "applications/mirror",
            "tls": {"certificate": "default"},
        },
        'listeners/*:8080',
    )

    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE

    raw = socket.create_connection(('127.0.0.1', port_map.port(8080)))
    sock = context.wrap_socket(raw)
    sock.settimeout(10)

    sock.sendall(handshake(ws.key()))
    rest = read_upgrade(sock)

    message = b'split frame header'
    frame = ws.frame_to_send(ws.OP_TEXT, message)

    # One byte in the first TLS record, the rest in a second one.
    for part in (frame[:1], frame[1:]):
        sock.sendall(part)
        time.sleep(0.5)

    start = time.monotonic()
    data = read_exact(sock, len(echo_of(message)), rest)
    print(f'tls: echo {data!r} in {time.monotonic() - start:.2f} s')

    assert data == echo_of(message), 'echo of the split frame'

    sock.close()
