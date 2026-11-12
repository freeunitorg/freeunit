"""Data of the application that a WebSocket client did not take.

When the client goes away, the router must release that data once, and it
must not use the closed request.  The limit "shm" of one segment (10 MiB)
makes a loss of shared memory visible: the application then waits for
memory and does not answer.
"""

import os
import signal
import socket
import struct
import time

import pytest
from packaging import version

from conftest import pid_by_name
from unit.applications.lang.python import ApplicationPython
from unit.applications.websockets import ApplicationWebsocket
from unit import port as port_map

prerequisites = {
    'modules': {'python': lambda v: version.parse(v) >= version.parse('3.5')}
}

client = ApplicationPython(load_module='asgi')
client_wsgi = ApplicationPython()
ws = ApplicationWebsocket()


def keepalive_interval(value):
    assert 'success' in client.conf(
        {'http': {'websocket': {'keepalive_interval': value}}}, 'settings'
    ), 'keepalive_interval'


def ws_open():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    sock.connect(('127.0.0.1', port_map.port(8080)))

    resp, sock, _ = ws.upgrade(sock=sock)
    assert resp['status'] == 101, 'upgrade'

    return sock


def reset(sock):
    sock.setsockopt(
        socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)
    )
    sock.close()


def answered(app_client, message):
    sock = app_client.get(no_recv=True)
    resp = app_client.recvall(sock, read_timeout=10)
    sock.close()

    assert resp.startswith(b'HTTP/1.1 200 '), message
    assert resp.endswith(b'\r\n\r\nok'), 'body'


def test_asgi_websockets_unsent_frames_released():
    """Each client is sent 10 MiB, reads nothing and resets the
    connection.  The frames that it did not take give their shared memory
    back to the application."""

    client.load('websockets/send_bulk', limits={'shm': 1}, processes=1)
    keepalive_interval(0)

    for i in range(3):
        sock = ws_open()

        ws.frame_write(sock, ws.OP_TEXT, '40 262144')

        # The application fills its memory, and the client reads nothing.
        time.sleep(1)

        reset(sock)

        answered(client, f'answered after {i + 1}')


@pytest.mark.parametrize(
    'text', ['36 262144', '3 3145728 close'], ids=['open', 'ended']
)
def test_asgi_websockets_unsent_frames_closed_request(text):
    """The default keep-alive interval: the router closes the connection
    one pass of the event loop after the request.  The client takes a part
    of the data and ends its side while the router is stopped, so that the
    router sees the end and room for more data at once.  The router must
    not write the rest for the closed request.  In the "ended" case the
    application closes the WebSocket first, so the last buffer of the
    request is in the unsent data too.  That case sends the same 9 MiB
    in 3 messages: one write takes a few buffers only, and with small
    messages it blocks long before the last buffer."""

    client.load('websockets/send_bulk', limits={'shm': 1}, processes=1)

    router = int(pid_by_name('router'))

    for i in range(3):
        sock = ws_open()

        # 9 MiB, so that data stays unsent after the client takes 3 MiB:
        # the kernel holds less than 6 MiB for a client that does not read.
        ws.frame_write(sock, ws.OP_TEXT, text)
        time.sleep(1)

        os.kill(router, signal.SIGSTOP)

        try:
            sock.settimeout(0.5)
            got = 0

            try:
                while got < 3 * 1048576:
                    data = sock.recv(1048576)
                    if not data:
                        break
                    got += len(data)

            except OSError:
                pass

            sock.shutdown(socket.SHUT_WR)
            time.sleep(0.3)

        finally:
            os.kill(router, signal.SIGCONT)

        time.sleep(1)
        sock.close()

        assert pid_by_name('router') == str(router), f'router after {i + 1}'

        answered(client, f'answered after {i + 1}')


def test_asgi_websockets_writes_after_client_left_released():
    """An application that answers a handshake with 101 by itself and is not
    told that the client left keeps writing.  The router has no connection
    for these writes, so it must give their shared memory back.  Here the
    writes are 25 MiB."""

    client_wsgi.load('upgrade_write', limits={'shm': 1}, processes=1)

    reset(ws_open())

    answered(client_wsgi, 'answered')
