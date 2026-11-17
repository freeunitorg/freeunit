"""A WebSocket frame whose buffer cannot be allocated.

The router allocates the buffer for the rest of a frame when the frame
header arrives.  The memory pool refuses 4 GiB or more.  The router must
then close the WebSocket with code 1011 and keep running.
"""

import socket
import ssl
import struct
import sys

import pytest
from packaging import version

from conftest import pid_by_name
from unit.applications.lang.python import ApplicationPython
from unit.applications.tls import ApplicationTLS
from unit.applications.websockets import ApplicationWebsocket
from unit import port as port_map
from unit.option import option

prerequisites = {
    'modules': {'python': lambda v: version.parse(v) >= version.parse('3.5')}
}

client = ApplicationPython(load_module='asgi')
tls = ApplicationTLS()
ws = ApplicationWebsocket()


def tls_socket():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('127.0.0.1', port_map.port(8080)))

    return ctx.wrap_socket(sock)


@pytest.mark.parametrize('use_tls', [False, True], ids=['plain', 'tls'])
def test_asgi_websockets_frame_alloc_fails(use_tls, wait_for_record):
    if sys.maxsize <= 2**32:
        pytest.skip('requires a 64-bit size_t')

    if use_tls and not option.available['modules']['openssl']:
        pytest.skip('requires openssl')

    client.load('websockets/mirror')

    if use_tls:
        tls.certificate()

        assert 'success' in client.conf(
            {'certificate': 'default'}, 'listeners/*:8080/tls'
        ), 'tls listener'

    assert 'success' in client.conf(
        {'http': {'websocket': {'max_frame_size': 8 << 30}}}, 'settings'
    ), 'max_frame_size 8 GiB'

    router = pid_by_name('router')

    def upgrade():
        sock = tls_socket() if use_tls else None
        resp, sock, _ = ws.upgrade(sock=sock)
        assert resp['status'] == 101, 'upgrade'

        return sock

    sock = upgrade()

    # Only the header of a masked binary frame that announces 5 GiB.
    sock.sendall(b'\x82\xff' + struct.pack('!Q', 5 << 30) + b'\x00' * 4)

    frame = ws.frame_read(sock, read_timeout=10)
    sock.close()

    assert frame['opcode'] == ws.OP_CLOSE, 'close opcode'
    assert frame['code'] == 1011, 'close code'
    assert wait_for_record(r'websocket error 1011: Out of memory'), 'log'

    sock = upgrade()

    ws.frame_write(sock, ws.OP_TEXT, 'still here')

    # frame_read() waits in select(), which does not see the bytes that
    # an SSL socket has already decrypted.  Read the short echo directly.
    expect = b'\x81\x0astill here'
    echo = b''
    sock.settimeout(10)

    while len(echo) < len(expect):
        chunk = sock.recv(len(expect) - len(echo))
        if not chunk:
            break

        echo += chunk

    sock.close()

    assert echo == expect, 'mirror'

    assert pid_by_name('router') == router, 'router pid'
