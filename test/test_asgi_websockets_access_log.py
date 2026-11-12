"""The access log record of a WebSocket request names that request.

The request fields point into the buffers that hold the request header.
The record is written when the session ends, so these buffers must stay
with the request until then: not given to another connection, not freed,
and not overwritten by frame bytes that the client sent with the handshake.
Each test prints what it saw (run with -s).
"""

import socket
import time

from packaging import version

from unit import port as port_map
from unit.applications.lang.python import ApplicationPython
from unit.applications.websockets import ApplicationWebsocket

prerequisites = {
    'modules': {'python': lambda v: version.parse(v) >= version.parse('3.5')}
}

client = ApplicationPython(load_module='asgi')
ws = ApplicationWebsocket()

WS_URL = '/ws-upgrade-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
WS_UA = 'ws-client-agent-BBBBBBBBBBBBBBBB'
WS_RECORD = f'GET {WS_URL} HTTP/1.1 "{WS_UA}"'


def setup_conf(temp_dir):
    client.load('websockets/mirror', name='mirror')

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "match": {"uri": "/ws-*"},
                    "action": {"pass": "applications/mirror"},
                },
                {"action": {"return": 204}},
            ],
            "access_log": {
                "path": f'{temp_dir}/access.log',
                "format": '$request_line "$header_user_agent"',
            },
            "settings": {"http": {"websocket": {"keepalive_interval": 0}}},
            "applications": client.conf_get('applications'),
        }
    ), 'configuration'


def connect():
    sock = socket.create_connection(('127.0.0.1', port_map.port(8080)))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(30)
    return sock


def upgrade(pipelined=b''):
    key = ws.key()
    sock = connect()
    sock.sendall(
        (
            f'GET {WS_URL} HTTP/1.1\r\n'
            'Host: localhost\r\n'
            f'User-Agent: {WS_UA}\r\n'
            'Upgrade: websocket\r\n'
            'Connection: Upgrade\r\n'
            f'Sec-WebSocket-Key: {key}\r\n'
            'Sec-WebSocket-Version: 13\r\n\r\n'
        ).encode()
        + pipelined
    )

    resp = b''
    while b'\r\n\r\n' not in resp:
        resp += sock.recv(4096)

    assert resp.startswith(b'HTTP/1.1 101'), resp

    if pipelined:
        return sock, resp.split(b'\r\n\r\n', 1)[1]

    return sock


def close_ws(sock):
    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())
    frame = ws.frame_read(sock)
    assert frame['opcode'] == ws.OP_CLOSE
    sock.close()


def plain_request(sock, i, partial=False):
    head = (
        f'GET /zz-other-{i:04d}-ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ HTTP/1.1\r\n'
        'Host: localhost\r\n'
        f'User-Agent: zz-other-agent-{i:04d}\r\n'
    )
    if partial:
        sock.sendall(head.encode())
    else:
        sock.sendall(f'{head}Connection: close\r\n\r\n'.encode())


def read_records(temp_dir, count):
    path = f'{temp_dir}/access.log'

    for _ in range(100):
        try:
            with open(path, 'rb') as f:
                lines = f.read().split(b'\n')[:-1]
        except FileNotFoundError:
            lines = []

        if len(lines) >= count:
            return lines

        time.sleep(0.1)

    return lines


def ws_record(lines):
    """The record of the upgrade: the one that is not a plain request.  A
    connection closed with a partial header gives a record with "-" as
    the request line and its own User-Agent."""
    plain = [l for l in lines if b'zz-other-' in l]
    rest = [l for l in lines if l not in plain]
    return rest, len(plain)


def test_asgi_websockets_access_log_reuse(temp_dir):
    """Later requests on new connections reuse the released header buffer."""
    setup_conf(temp_dir)

    sock = upgrade()

    n = 64
    for i in range(n):
        s = connect()
        plain_request(s, i)
        while s.recv(4096):
            pass
        s.close()

    close_ws(sock)

    lines = read_records(temp_dir, n + 1)
    rest, nplain = ws_record(lines)
    print(f'reuse: plain records {nplain}, other records {rest}')

    assert rest == [WS_RECORD.encode()]


def test_asgi_websockets_access_log_freed(temp_dir):
    """Many connections hold header buffers and close in reverse order, so
    the engine cache is full when the released buffer comes back: then it
    is freed, and the record reads freed heap (a report under ASan)."""
    setup_conf(temp_dir)

    sock = upgrade()

    n = 256
    socks = []
    for i in range(n):
        s = connect()
        plain_request(s, i, partial=True)
        socks.append(s)

    # Let the router read every partial header.
    time.sleep(1)

    for s in reversed(socks):
        s.close()
        time.sleep(0.005)

    time.sleep(1)

    close_ws(sock)

    for _ in range(100):
        lines = read_records(temp_dir, 1)
        rest, nplain = ws_record(lines)
        if rest:
            break
        time.sleep(0.1)

    print(f'freed: plain records {nplain}, other records {rest}')

    assert rest == [WS_RECORD.encode()]


def test_asgi_websockets_access_log_pipelined(temp_dir):
    """A frame sent in the same packet as the handshake is answered, and
    it does not overwrite the request line that the record reads."""
    setup_conf(temp_dir)

    payload = b'P' * 100
    sock, rest = upgrade(ws.frame_to_send(ws.OP_TEXT, payload))

    echo = b'\x81' + bytes([len(payload)]) + payload
    while len(rest) < len(echo):
        chunk = sock.recv(4096)
        assert chunk, 'connection closed before the echo'
        rest += chunk

    assert rest == echo, 'echo of the pipelined frame'

    close_ws(sock)

    lines = read_records(temp_dir, 1)
    rest, nplain = ws_record(lines)
    print(f'pipelined: other records {rest}')

    assert rest == [WS_RECORD.encode()]
