"""A WebSocket connection over TLS that closes with data unsent.

The TLS shutdown of the router can wait for the socket.  In that time the
router must not read for the connection.
"""

import socket
import ssl
import time

import pytest
from packaging import version

from conftest import pid_by_name
from unit.applications.lang.python import ApplicationPython
from unit.applications.tls import ApplicationTLS
from unit.applications.websockets import ApplicationWebsocket
from unit import port as port_map

prerequisites = {
    'modules': {
        'python': lambda v: version.parse(v) >= version.parse('3.5'),
        'openssl': 'any',
    }
}

client = ApplicationPython(load_module='asgi')
tls = ApplicationTLS()
ws = ApplicationWebsocket()


def context():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    return ctx


def tls_socket(rcvbuf=None):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    if rcvbuf is not None:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)

    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.connect(('127.0.0.1', port_map.port(8080)))

    return context().wrap_socket(sock)


@pytest.mark.parametrize('interval', [0, 30], ids=['ka0', 'ka30'])
def test_asgi_websockets_tls_send_timeout_frame_header(interval, skip_alert):
    """The send timeout ends the request while bytes of a frame header
    arrive.  The router then shuts TLS down, and it must not read for the
    closed request while the shutdown waits."""

    # The shutdown of a connection whose last TLS write was not completed
    # fails, and the router logs it.  That alert is not checked here.
    skip_alert(r'SSL_shutdown\(\d+\) failed')

    client.load('websockets/send_bulk', processes=1)

    tls.certificate()

    assert 'success' in client.conf(
        {'certificate': 'default'}, 'listeners/*:8080/tls'
    ), 'tls listener'

    assert 'success' in client.conf(
        {
            'http': {
                'send_timeout': 2,
                'websocket': {'keepalive_interval': interval},
            }
        },
        'settings',
    ), 'settings'

    router = int(pid_by_name('router'))

    # An empty PONG frame, sent one byte at a time.
    pong = b'\x8a\x80\x00\x00\x00\x00'

    for i in range(2):
        resp, sock, _ = ws.upgrade(sock=tls_socket(rcvbuf=4096))
        assert resp['status'] == 101, 'upgrade'

        # 9 MiB: more than the kernel holds for a client that does not
        # read, so that the send timer runs.
        ws.frame_write(sock, ws.OP_TEXT, '36 262144')
        time.sleep(0.5)

        end = time.time() + 3.5
        sent = 0

        try:
            while time.time() < end:
                sock.sendall(pong[sent % 6 : sent % 6 + 1])
                sent += 1
                time.sleep(0.001)

        except OSError:
            pass

        time.sleep(0.5)
        sock.close()

        assert pid_by_name('router') == str(router), f'router after {i + 1}'

    sock = tls_socket()
    sock.sendall(
        b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'
    )
    resp = client.recvall(sock, read_timeout=10)
    sock.close()

    assert resp.startswith(b'HTTP/1.1 200 '), 'answered'
    assert resp.endswith(b'\r\n\r\nok'), 'body'
