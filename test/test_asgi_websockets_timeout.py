"""limits.timeout must not end a quiet, established WebSocket (#422)."""

from packaging import version

from unit.applications.lang.python import ApplicationPython
from unit.applications.websockets import ApplicationWebsocket

prerequisites = {
    'modules': {'python': lambda v: version.parse(v) >= version.parse('3.5')}
}

client = ApplicationPython(load_module='asgi')
ws = ApplicationWebsocket()


def test_asgi_websockets_timeout_quiet_session(skip_alert):
    skip_alert(r'socket close\(\d+\) failed')

    assert 'success' in client.conf(
        {'http': {'websocket': {'keepalive_interval': 0}}}, 'settings'
    ), 'clear keepalive_interval'

    client.load('websockets/mirror', limits={'timeout': 1})

    resp, sock, _ = ws.upgrade()
    assert resp['status'] == 101, 'upgrade'

    # frame_read() would spin on EOF.
    sock.settimeout(3)

    try:
        early = sock.recv(4096)
    except TimeoutError:
        early = None

    assert early is None, f'nothing arrives on a quiet session: {early!r}'

    ws.frame_write(sock, ws.OP_TEXT, 'still here')
    frame = ws.frame_read(sock, read_timeout=2)

    assert frame['opcode'] == ws.OP_TEXT, 'session survived the deadline'
    assert frame['data'].decode('utf-8') == 'still here', 'mirror'

    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())
    frame = ws.frame_read(sock)
    assert frame['opcode'] == ws.OP_CLOSE, 'close'

    sock.close()
