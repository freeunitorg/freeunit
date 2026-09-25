import struct
import time

import pytest

from unit.applications.lang.java import ApplicationJava
from unit.applications.websockets import ApplicationWebsocket

prerequisites = {'modules': {'java': 'any'}}

client = ApplicationJava()
ws = ApplicationWebsocket()


@pytest.fixture(autouse=True)
def setup_method_fixture(skip_alert):
    assert 'success' in client.conf(
        {'http': {'websocket': {'keepalive_interval': 0}}}, 'settings'
    ), 'clear keepalive_interval'

    skip_alert(r'socket close\(\d+\) failed')


def close_connection(sock):
    assert client.recvall(sock, read_timeout=0.1) == b'', 'empty soc'

    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())

    check_close(sock)


def check_close(sock, code=1000, no_close=False, frame=None):
    if frame is None:
        frame = ws.frame_read(sock)

    assert frame['fin'], 'close fin'
    assert frame['opcode'] == ws.OP_CLOSE, 'close opcode'
    assert frame['code'] == code, 'close code'

    if not no_close:
        sock.close()


def check_frame(frame, fin, opcode, payload, decode=True):
    if opcode == ws.OP_BINARY or not decode:
        data = frame['data']
    else:
        data = frame['data'].decode('utf-8')

    assert frame['fin'] == fin, 'fin'
    assert frame['opcode'] == opcode, 'opcode'
    assert data == payload, 'payload'


def test_java_websockets_handshake():
    client.load('websockets_mirror')

    resp, sock, key = ws.upgrade()
    sock.close()

    assert resp['status'] == 101, 'status'
    assert resp['headers']['Upgrade'] == 'websocket', 'upgrade'
    assert resp['headers']['Connection'] == 'Upgrade', 'connection'
    assert resp['headers']['Sec-WebSocket-Accept'] == ws.accept(key), 'key'


def test_java_websockets_mirror():
    client.load('websockets_mirror')

    message = 'blah'

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, message)
    frame = ws.frame_read(sock)

    assert message == frame['data'].decode('utf-8'), 'mirror'

    ws.frame_write(sock, ws.OP_TEXT, message)
    frame = ws.frame_read(sock)

    assert message == frame['data'].decode('utf-8'), 'mirror 2'

    sock.close()


def test_java_websockets_no_mask():
    client.load('websockets_mirror')

    message = 'blah'

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, message, mask=False)

    frame = ws.frame_read(sock)

    assert frame['opcode'] == ws.OP_CLOSE, 'no mask opcode'
    assert frame['code'] == 1002, 'no mask close code'

    sock.close()


def test_java_websockets_fragmentation():
    client.load('websockets_mirror')

    message = 'blah'

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, message, fin=False)
    ws.frame_write(sock, ws.OP_CONT, ' ', fin=False)
    ws.frame_write(sock, ws.OP_CONT, message)

    frame = ws.frame_read(sock)

    assert f'{message} {message}' == frame['data'].decode(
        'utf-8'
    ), 'mirror framing'

    sock.close()


def test_java_websockets_frame_fragmentation_invalid():
    client.load('websockets_mirror')

    message = 'blah'

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_PING, message, fin=False)

    frame = ws.frame_read(sock)

    frame.pop('data')
    assert frame == {
        'fin': True,
        'rsv1': False,
        'rsv2': False,
        'rsv3': False,
        'opcode': ws.OP_CLOSE,
        'mask': 0,
        'code': 1002,
        'reason': 'Fragmented control frame',
    }, 'close frame'

    sock.close()


def test_java_websockets_two_clients():
    client.load('websockets_mirror')

    message1 = 'blah1'
    message2 = 'blah2'

    _, sock1, _ = ws.upgrade()
    _, sock2, _ = ws.upgrade()

    ws.frame_write(sock1, ws.OP_TEXT, message1)
    ws.frame_write(sock2, ws.OP_TEXT, message2)

    frame1 = ws.frame_read(sock1)
    frame2 = ws.frame_read(sock2)

    assert message1 == frame1['data'].decode('utf-8'), 'client 1'
    assert message2 == frame2['data'].decode('utf-8'), 'client 2'

    sock1.close()
    sock2.close()


# FAIL https://tools.ietf.org/html/rfc6455#section-4.2.1
@pytest.mark.skip('not yet')
def test_java_websockets_handshake_upgrade_absent():
    client.load('websockets_mirror')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Connection': 'Upgrade',
            'Sec-WebSocket-Key': ws.key(),
            'Sec-WebSocket-Protocol': 'chat',
            'Sec-WebSocket-Version': 13,
        },
    )

    assert resp['status'] == 400, 'upgrade absent'


def test_java_websockets_handshake_case_insensitive():
    client.load('websockets_mirror')

    resp, sock, _ = ws.upgrade(
        headers={
            'Host': 'localhost',
            'Upgrade': 'WEBSOCKET',
            'Connection': 'UPGRADE',
            'Sec-WebSocket-Key': ws.key(),
            'Sec-WebSocket-Protocol': 'chat',
            'Sec-WebSocket-Version': 13,
        }
    )
    sock.close()

    assert resp['status'] == 101, 'status'


@pytest.mark.skip('not yet')
def test_java_websockets_handshake_connection_absent():  # FAIL
    client.load('websockets_mirror')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Upgrade': 'websocket',
            'Sec-WebSocket-Key': ws.key(),
            'Sec-WebSocket-Protocol': 'chat',
            'Sec-WebSocket-Version': 13,
        },
        connection_close=False,
    )

    assert resp['status'] == 400, 'status'


def test_java_websockets_handshake_version_absent():
    client.load('websockets_mirror')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Upgrade': 'websocket',
            'Connection': 'Upgrade',
            'Sec-WebSocket-Key': ws.key(),
            'Sec-WebSocket-Protocol': 'chat',
        },
    )

    assert resp['status'] == 426, 'status'


@pytest.mark.skip('not yet')
def test_java_websockets_handshake_key_invalid():
    client.load('websockets_mirror')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Upgrade': 'websocket',
            'Connection': 'Upgrade',
            'Sec-WebSocket-Key': '!',
            'Sec-WebSocket-Protocol': 'chat',
            'Sec-WebSocket-Version': 13,
        },
    )

    assert resp['status'] == 400, 'key length'

    key = ws.key()
    resp = client.get(
        headers={
            'Host': 'localhost',
            'Upgrade': 'websocket',
            'Connection': 'Upgrade',
            'Sec-WebSocket-Key': [key, key],
            'Sec-WebSocket-Protocol': 'chat',
            'Sec-WebSocket-Version': 13,
        },
    )

    assert (
        resp['status'] == 400
    ), 'key double'  # FAIL https://tools.ietf.org/html/rfc6455#section-11.3.1


def test_java_websockets_handshake_method_invalid():
    client.load('websockets_mirror')

    resp = client.post(
        headers={
            'Host': 'localhost',
            'Upgrade': 'websocket',
            'Connection': 'Upgrade',
            'Sec-WebSocket-Key': ws.key(),
            'Sec-WebSocket-Protocol': 'chat',
            'Sec-WebSocket-Version': 13,
        },
    )

    assert resp['status'] == 400, 'status'


def test_java_websockets_handshake_http_10():
    client.load('websockets_mirror')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Upgrade': 'websocket',
            'Connection': 'Upgrade',
            'Sec-WebSocket-Key': ws.key(),
            'Sec-WebSocket-Protocol': 'chat',
            'Sec-WebSocket-Version': 13,
        },
        http_10=True,
    )

    assert resp['status'] == 400, 'status'


def test_java_websockets_handshake_uri_invalid():
    client.load('websockets_mirror')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Upgrade': 'websocket',
            'Connection': 'Upgrade',
            'Sec-WebSocket-Key': ws.key(),
            'Sec-WebSocket-Protocol': 'chat',
            'Sec-WebSocket-Version': 13,
        },
        url='!',
    )

    assert resp['status'] == 400, 'status'


def test_java_websockets_protocol_absent():
    client.load('websockets_mirror')

    key = ws.key()
    resp, sock, _ = ws.upgrade(
        headers={
            'Host': 'localhost',
            'Upgrade': 'websocket',
            'Connection': 'Upgrade',
            'Sec-WebSocket-Key': key,
            'Sec-WebSocket-Version': 13,
        }
    )
    sock.close()

    assert resp['status'] == 101, 'status'
    assert resp['headers']['Upgrade'] == 'websocket', 'upgrade'
    assert resp['headers']['Connection'] == 'Upgrade', 'connection'
    assert resp['headers']['Sec-WebSocket-Accept'] == ws.accept(key), 'key'


# autobahn-testsuite
#
# The router validates the payload of a text message and the reason of a
# close frame as UTF-8 and fails the connection with 1007 when it is not.


def test_java_websockets_1_1_1__1_1_8():
    client.load('websockets_mirror')

    opcode = ws.OP_TEXT

    _, sock, _ = ws.upgrade()

    def check_length(length, chopsize=None):
        payload = '*' * length

        ws.frame_write(sock, opcode, payload, chopsize=chopsize)

        frame = ws.message_read(sock)
        check_frame(frame, True, opcode, payload)

    check_length(0)  # 1_1_1
    check_length(125)  # 1_1_2
    check_length(126)  # 1_1_3
    check_length(127)  # 1_1_4
    check_length(128)  # 1_1_5
    check_length(65535)  # 1_1_6
    check_length(65536)  # 1_1_7
    check_length(65536, chopsize=997)  # 1_1_8

    close_connection(sock)


def test_java_websockets_1_2_1__1_2_8():
    client.load('websockets_mirror')

    opcode = ws.OP_BINARY

    _, sock, _ = ws.upgrade()

    def check_length(length, chopsize=None):
        payload = b'\xfe' * length

        ws.frame_write(sock, opcode, payload, chopsize=chopsize)

        frame = ws.message_read(sock)
        check_frame(frame, True, opcode, payload)

    check_length(0)  # 1_2_1
    check_length(125)  # 1_2_2
    check_length(126)  # 1_2_3
    check_length(127)  # 1_2_4
    check_length(128)  # 1_2_5
    check_length(65535)  # 1_2_6
    check_length(65536)  # 1_2_7
    check_length(65536, chopsize=997)  # 1_2_8

    close_connection(sock)


def test_java_websockets_2_1__2_6():
    client.load('websockets_mirror')

    op_ping = ws.OP_PING
    op_pong = ws.OP_PONG

    _, sock, _ = ws.upgrade()

    def check_ping(payload, chopsize=None, decode=True):
        ws.frame_write(sock, op_ping, payload, chopsize=chopsize)
        frame = ws.frame_read(sock)

        check_frame(frame, True, op_pong, payload, decode=decode)

    check_ping('')  # 2_1
    check_ping('Hello, world!')  # 2_2
    check_ping(b'\x00\xff\xfe\xfd\xfc\xfb\x00\xff', decode=False)  # 2_3
    check_ping(b'\xfe' * 125, decode=False)  # 2_4
    check_ping(b'\xfe' * 125, chopsize=1, decode=False)  # 2_6

    close_connection(sock)

    # 2_5

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_PING, b'\xfe' * 126)
    check_close(sock, 1002)


def test_java_websockets_2_7__2_9():
    client.load('websockets_mirror')

    # 2_7

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_PONG, '')
    assert client.recvall(sock, read_timeout=0.1) == b'', '2_7'

    # 2_8

    ws.frame_write(sock, ws.OP_PONG, 'unsolicited pong payload')
    assert client.recvall(sock, read_timeout=0.1) == b'', '2_8'

    # 2_9

    payload = 'ping payload'

    ws.frame_write(sock, ws.OP_PONG, 'unsolicited pong payload')
    ws.frame_write(sock, ws.OP_PING, payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_PONG, payload)

    close_connection(sock)


def test_java_websockets_2_10__2_11():
    client.load('websockets_mirror')

    # 2_10

    _, sock, _ = ws.upgrade()

    for i in range(0, 10):
        ws.frame_write(sock, ws.OP_PING, f'payload-{i}')

    for i in range(0, 10):
        frame = ws.frame_read(sock)
        check_frame(frame, True, ws.OP_PONG, f'payload-{i}')

    # 2_11

    for i in range(0, 10):
        opcode = ws.OP_PING
        ws.frame_write(sock, opcode, f'payload-{i}', chopsize=1)

    for i in range(0, 10):
        frame = ws.frame_read(sock)
        check_frame(frame, True, ws.OP_PONG, f'payload-{i}')

    close_connection(sock)


@pytest.mark.skip('not yet')
def test_java_websockets_3_1__3_7():
    client.load('websockets_mirror')

    payload = 'Hello, world!'

    # 3_1

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, payload, rsv1=True)
    check_close(sock, 1002)

    # 3_2

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, payload)
    ws.frame_write(sock, ws.OP_TEXT, payload, rsv2=True)
    ws.frame_write(sock, ws.OP_PING, '')

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    check_close(sock, 1002, no_close=True)

    assert client.recvall(sock, read_timeout=0.1) == b'', 'empty 3_2'
    sock.close()

    # 3_3

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    ws.frame_write(sock, ws.OP_TEXT, payload, rsv1=True, rsv2=True)

    check_close(sock, 1002, no_close=True)

    assert client.recvall(sock, read_timeout=0.1) == b'', 'empty 3_3'
    sock.close()

    # 3_4

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, payload, chopsize=1)
    ws.frame_write(sock, ws.OP_TEXT, payload, rsv3=True, chopsize=1)
    ws.frame_write(sock, ws.OP_PING, '')

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    check_close(sock, 1002, no_close=True)

    assert client.recvall(sock, read_timeout=0.1) == b'', 'empty 3_4'
    sock.close()

    # 3_5

    _, sock, _ = ws.upgrade()

    ws.frame_write(
        sock,
        ws.OP_BINARY,
        b'\x00\xff\xfe\xfd\xfc\xfb\x00\xff',
        rsv1=True,
        rsv3=True,
    )

    check_close(sock, 1002)

    # 3_6

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_PING, payload, rsv2=True, rsv3=True)

    check_close(sock, 1002)

    # 3_7

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_CLOSE, payload, rsv1=True, rsv2=True, rsv3=True)

    check_close(sock, 1002)


def test_java_websockets_4_1_1__4_2_5():
    client.load('websockets_mirror')

    payload = 'Hello, world!'

    # 4_1_1

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, 0x03, '')
    check_close(sock, 1002)

    # 4_1_2

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, 0x04, 'reserved opcode payload')
    check_close(sock, 1002)

    # 4_1_3

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    ws.frame_write(sock, 0x05, '')
    ws.frame_write(sock, ws.OP_PING, '')

    check_close(sock, 1002)

    # 4_1_4

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    ws.frame_write(sock, 0x06, payload)
    ws.frame_write(sock, ws.OP_PING, '')

    check_close(sock, 1002)

    # 4_1_5

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, payload, chopsize=1)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    ws.frame_write(sock, 0x07, payload, chopsize=1)
    ws.frame_write(sock, ws.OP_PING, '')

    check_close(sock, 1002)

    # 4_2_1

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, 0x0B, '')
    check_close(sock, 1002)

    # 4_2_2

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, 0x0C, 'reserved opcode payload')
    check_close(sock, 1002)

    # 4_2_3

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    ws.frame_write(sock, 0x0D, '')
    ws.frame_write(sock, ws.OP_PING, '')

    check_close(sock, 1002)

    # 4_2_4

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    ws.frame_write(sock, 0x0E, payload)
    ws.frame_write(sock, ws.OP_PING, '')

    check_close(sock, 1002)

    # 4_2_5

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, payload, chopsize=1)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    ws.frame_write(sock, 0x0F, payload, chopsize=1)
    ws.frame_write(sock, ws.OP_PING, '')

    check_close(sock, 1002)


def test_java_websockets_5_1__5_20():
    client.load('websockets_mirror')

    # 5_1

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_PING, 'fragment1', fin=False)
    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=True)
    check_close(sock, 1002)

    # 5_2

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_PONG, 'fragment1', fin=False)
    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=True)
    check_close(sock, 1002)

    # 5_3

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False)
    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=True)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, 'fragment1fragment2')

    # 5_4

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False)
    assert client.recvall(sock, read_timeout=0.1) == b'', '5_4'
    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=True)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, 'fragment1fragment2')

    # 5_5

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False, chopsize=1)
    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=True, chopsize=1)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, 'fragment1fragment2')

    # 5_6

    ping_payload = 'ping payload'

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False)
    ws.frame_write(sock, ws.OP_PING, ping_payload)
    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=True)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_PONG, ping_payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, 'fragment1fragment2')

    # 5_7

    ping_payload = 'ping payload'

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False)
    assert client.recvall(sock, read_timeout=0.1) == b'', '5_7'

    ws.frame_write(sock, ws.OP_PING, ping_payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_PONG, ping_payload)

    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=True)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, 'fragment1fragment2')

    # 5_8

    ping_payload = 'ping payload'

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False, chopsize=1)
    ws.frame_write(sock, ws.OP_PING, ping_payload, chopsize=1)
    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=True, chopsize=1)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_PONG, ping_payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, 'fragment1fragment2')

    # 5_9

    ws.frame_write(sock, ws.OP_CONT, 'non-continuation payload', fin=True)
    ws.frame_write(sock, ws.OP_TEXT, 'Hello, world!', fin=True)
    check_close(sock, 1002)

    # 5_10

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_CONT, 'non-continuation payload', fin=True)
    ws.frame_write(sock, ws.OP_TEXT, 'Hello, world!', fin=True)
    check_close(sock, 1002)

    # 5_11

    _, sock, _ = ws.upgrade()

    ws.frame_write(
        sock,
        ws.OP_CONT,
        'non-continuation payload',
        fin=True,
        chopsize=1,
    )
    ws.frame_write(sock, ws.OP_TEXT, 'Hello, world!', fin=True, chopsize=1)
    check_close(sock, 1002)

    # 5_12

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_CONT, 'non-continuation payload', fin=False)
    ws.frame_write(sock, ws.OP_TEXT, 'Hello, world!', fin=True)
    check_close(sock, 1002)

    # 5_13

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_CONT, 'non-continuation payload', fin=False)
    ws.frame_write(sock, ws.OP_TEXT, 'Hello, world!', fin=True)
    check_close(sock, 1002)

    # 5_14

    _, sock, _ = ws.upgrade()

    ws.frame_write(
        sock,
        ws.OP_CONT,
        'non-continuation payload',
        fin=False,
        chopsize=1,
    )
    ws.frame_write(sock, ws.OP_TEXT, 'Hello, world!', fin=True, chopsize=1)
    check_close(sock, 1002)

    # 5_15

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False)
    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=True)
    ws.frame_write(sock, ws.OP_CONT, 'fragment3', fin=False)
    ws.frame_write(sock, ws.OP_TEXT, 'fragment4', fin=True)

    frame = ws.frame_read(sock)

    if frame['opcode'] == ws.OP_TEXT:
        check_frame(frame, True, ws.OP_TEXT, 'fragment1fragment2')
        frame = None

    check_close(sock, 1002, frame=frame)

    # 5_16

    _, sock, _ = ws.upgrade()

    for _ in range(0, 2):
        ws.frame_write(sock, ws.OP_CONT, 'fragment1', fin=False)
        ws.frame_write(sock, ws.OP_TEXT, 'fragment2', fin=False)
        ws.frame_write(sock, ws.OP_CONT, 'fragment3', fin=True)
    check_close(sock, 1002)

    # 5_17

    _, sock, _ = ws.upgrade()

    for _ in range(0, 2):
        ws.frame_write(sock, ws.OP_CONT, 'fragment1', fin=True)
        ws.frame_write(sock, ws.OP_TEXT, 'fragment2', fin=False)
        ws.frame_write(sock, ws.OP_CONT, 'fragment3', fin=True)
    check_close(sock, 1002)

    # 5_18

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False)
    ws.frame_write(sock, ws.OP_TEXT, 'fragment2')
    check_close(sock, 1002)

    # 5_19

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False)
    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=False)
    ws.frame_write(sock, ws.OP_PING, 'pongme 1!')

    time.sleep(1)

    ws.frame_write(sock, ws.OP_CONT, 'fragment3', fin=False)
    ws.frame_write(sock, ws.OP_CONT, 'fragment4', fin=False)
    ws.frame_write(sock, ws.OP_PING, 'pongme 2!')
    ws.frame_write(sock, ws.OP_CONT, 'fragment5')

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_PONG, 'pongme 1!')

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_PONG, 'pongme 2!')

    check_frame(
        ws.frame_read(sock),
        True,
        ws.OP_TEXT,
        'fragment1fragment2fragment3fragment4fragment5',
    )

    # 5_20

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False)
    ws.frame_write(sock, ws.OP_CONT, 'fragment2', fin=False)
    ws.frame_write(sock, ws.OP_PING, 'pongme 1!')

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_PONG, 'pongme 1!')

    time.sleep(1)

    ws.frame_write(sock, ws.OP_CONT, 'fragment3', fin=False)
    ws.frame_write(sock, ws.OP_CONT, 'fragment4', fin=False)
    ws.frame_write(sock, ws.OP_PING, 'pongme 2!')

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_PONG, 'pongme 2!')

    assert client.recvall(sock, read_timeout=0.1) == b'', '5_20'
    ws.frame_write(sock, ws.OP_CONT, 'fragment5')

    check_frame(
        ws.frame_read(sock),
        True,
        ws.OP_TEXT,
        'fragment1fragment2fragment3fragment4fragment5',
    )

    close_connection(sock)


def test_java_websockets_6_1_1__6_4_4():
    client.load('websockets_mirror')

    # 6_1_1

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, '')
    frame = ws.frame_read(sock, read_timeout=3)
    check_frame(frame, True, ws.OP_TEXT, '')

    # 6_1_2

    ws.frame_write(sock, ws.OP_TEXT, '', fin=False)
    ws.frame_write(sock, ws.OP_CONT, '', fin=False)
    ws.frame_write(sock, ws.OP_CONT, '')

    frame = ws.frame_read(sock, read_timeout=3)
    check_frame(frame, True, ws.OP_TEXT, '')

    # 6_1_3

    payload = 'middle frame payload'

    ws.frame_write(sock, ws.OP_TEXT, '', fin=False)
    ws.frame_write(sock, ws.OP_CONT, payload, fin=False)
    ws.frame_write(sock, ws.OP_CONT, '')

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    # 6_2_1

    payload = 'Hello-µ@ßöäüàá-UTF-8!!'

    ws.frame_write(sock, ws.OP_TEXT, payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    # 6_2_2

    ws.frame_write(sock, ws.OP_TEXT, payload[:12], fin=False)
    ws.frame_write(sock, ws.OP_CONT, payload[12:])

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    # 6_2_3

    ws.message(sock, ws.OP_TEXT, payload, fragmention_size=1)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    # 6_2_4

    payload = '\xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5'

    ws.message(sock, ws.OP_TEXT, payload, fragmention_size=1)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    close_connection(sock)

    # 6_3_1

    _, sock, _ = ws.upgrade()

    # Raw bytes: written as str these would be encoded to valid UTF-8.
    payload_1 = b'\xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5'
    payload_2 = b'\xed\xa0\x80'
    payload_3 = b'\x65\x64\x69\x74\x65\x64'

    payload = payload_1 + payload_2 + payload_3

    ws.message(sock, ws.OP_TEXT, payload)
    check_close(sock, 1007)

    # 6_3_2

    _, sock, _ = ws.upgrade()

    ws.message(sock, ws.OP_TEXT, payload, fragmention_size=1)
    check_close(sock, 1007)


def test_java_websockets_7_1_1__7_5_1():
    client.load('websockets_mirror')

    # 7_1_1

    _, sock, _ = ws.upgrade()

    payload = "Hello World!"

    ws.frame_write(sock, ws.OP_TEXT, payload)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    close_connection(sock)

    # 7_1_2

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())
    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())

    check_close(sock)

    # 7_1_3

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())
    check_close(sock, no_close=True)

    ws.frame_write(sock, ws.OP_PING, '')
    assert client.recvall(sock, read_timeout=0.1) == b'', 'empty soc'

    sock.close()

    # 7_1_4

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())
    check_close(sock, no_close=True)

    ws.frame_write(sock, ws.OP_TEXT, payload)
    assert client.recvall(sock, read_timeout=0.1) == b'', 'empty soc'

    sock.close()

    # 7_1_5

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, 'fragment1', fin=False)
    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())
    check_close(sock, no_close=True)

    ws.frame_write(sock, ws.OP_CONT, 'fragment2')
    assert client.recvall(sock, read_timeout=0.1) == b'', 'empty soc'

    sock.close()

    # 7_1_6

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, 'BAsd7&jh23' * 26 * 2**10)
    ws.frame_write(sock, ws.OP_TEXT, payload)
    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())

    client.recvall(sock, read_timeout=1)

    ws.frame_write(sock, ws.OP_PING, '')
    assert client.recvall(sock, read_timeout=0.1) == b'', 'empty soc'

    sock.close()

    # 7_3_1

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_CLOSE, '')
    check_close(sock)

    # 7_3_2

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_CLOSE, 'a')
    check_close(sock, 1002)

    # 7_3_3

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())
    check_close(sock)

    # 7_3_4

    _, sock, _ = ws.upgrade()

    payload = ws.serialize_close(reason='Hello World!')

    ws.frame_write(sock, ws.OP_CLOSE, payload)
    check_close(sock)

    # 7_3_5

    _, sock, _ = ws.upgrade()

    payload = ws.serialize_close(reason='*' * 123)

    ws.frame_write(sock, ws.OP_CLOSE, payload)
    check_close(sock)

    # 7_3_6

    _, sock, _ = ws.upgrade()

    payload = ws.serialize_close(reason='*' * 124)

    ws.frame_write(sock, ws.OP_CLOSE, payload)
    check_close(sock, 1002)

    # 7_5_1

    _, sock, _ = ws.upgrade()

    # A raw reason: 0xed 0xa0 0x80 is a surrogate, which has no valid UTF-8
    # encoding.
    payload = ws.serialize_close() + b'\xce\xba\xe1\xbd\xb9\xcf' \
        b'\x83\xce\xbc\xce\xb5\xed\xa0\x80\x65\x64\x69\x74\x65\x64'

    ws.frame_write(sock, ws.OP_CLOSE, payload)
    check_close(sock, 1007)


def test_java_websockets_7_7_X__7_9_X():
    client.load('websockets_mirror')

    valid_codes = [
        1000,
        1001,
        1002,
        1003,
        1007,
        1008,
        1009,
        1010,
        1011,
        3000,
        3999,
        4000,
        4999,
    ]

    invalid_codes = [0, 999, 1004, 1005, 1006, 1016, 1100, 2000, 2999]

    for code in valid_codes:
        _, sock, _ = ws.upgrade()

        payload = ws.serialize_close(code=code)

        ws.frame_write(sock, ws.OP_CLOSE, payload)
        check_close(sock, code=code)

    for code in invalid_codes:
        _, sock, _ = ws.upgrade()

        payload = ws.serialize_close(code=code)

        ws.frame_write(sock, ws.OP_CLOSE, payload)
        check_close(sock, 1002)


def test_java_websockets_7_13_1__7_13_2():
    client.load('websockets_mirror')

    # 7_13_1

    _, sock, _ = ws.upgrade()

    payload = ws.serialize_close(code=5000)

    ws.frame_write(sock, ws.OP_CLOSE, payload)
    check_close(sock, 1002)

    # 7_13_2

    _, sock, _ = ws.upgrade()

    payload = struct.pack('!I', 65536) + ''.encode('utf-8')

    ws.frame_write(sock, ws.OP_CLOSE, payload)
    check_close(sock, 1002)


def test_java_websockets_9_1_1__9_6_6(is_unsafe, system):
    if not is_unsafe:
        pytest.skip('unsafe, long run')

    client.load('websockets_mirror')

    assert 'success' in client.conf(
        {
            'http': {
                'websocket': {
                    'max_frame_size': 33554432,
                    'keepalive_interval': 0,
                }
            }
        },
        'settings',
    ), 'increase max_frame_size and keepalive_interval'

    _, sock, _ = ws.upgrade()

    op_text = ws.OP_TEXT
    op_binary = ws.OP_BINARY

    def check_payload(opcode, length, chopsize=None):
        if opcode == ws.OP_TEXT:
            payload = '*' * length
        else:
            payload = b'*' * length

        ws.frame_write(sock, opcode, payload, chopsize=chopsize)
        frame = ws.frame_read(sock)
        check_frame(frame, True, opcode, payload)

    def check_message(opcode, f_size):
        if opcode == ws.OP_TEXT:
            payload = '*' * 4 * 2**20
        else:
            payload = b'*' * 4 * 2**20

        ws.message(sock, opcode, payload, fragmention_size=f_size)
        frame = ws.frame_read(sock, read_timeout=5)
        check_frame(frame, True, opcode, payload)

    check_payload(op_text, 64 * 2**10)  # 9_1_1
    check_payload(op_text, 256 * 2**10)  # 9_1_2
    check_payload(op_text, 2**20)  # 9_1_3
    check_payload(op_text, 4 * 2**20)  # 9_1_4
    check_payload(op_text, 8 * 2**20)  # 9_1_5
    check_payload(op_text, 16 * 2**20)  # 9_1_6

    check_payload(op_binary, 64 * 2**10)  # 9_2_1
    check_payload(op_binary, 256 * 2**10)  # 9_2_2
    check_payload(op_binary, 2**20)  # 9_2_3
    check_payload(op_binary, 4 * 2**20)  # 9_2_4
    check_payload(op_binary, 8 * 2**20)  # 9_2_5
    check_payload(op_binary, 16 * 2**20)  # 9_2_6

    if system not in ['Darwin', 'FreeBSD']:
        check_message(op_text, 64)  # 9_3_1
        check_message(op_text, 256)  # 9_3_2
        check_message(op_text, 2**10)  # 9_3_3
        check_message(op_text, 4 * 2**10)  # 9_3_4
        check_message(op_text, 16 * 2**10)  # 9_3_5
        check_message(op_text, 64 * 2**10)  # 9_3_6
        check_message(op_text, 256 * 2**10)  # 9_3_7
        check_message(op_text, 2**20)  # 9_3_8
        check_message(op_text, 4 * 2**20)  # 9_3_9

        check_message(op_binary, 64)  # 9_4_1
        check_message(op_binary, 256)  # 9_4_2
        check_message(op_binary, 2**10)  # 9_4_3
        check_message(op_binary, 4 * 2**10)  # 9_4_4
        check_message(op_binary, 16 * 2**10)  # 9_4_5
        check_message(op_binary, 64 * 2**10)  # 9_4_6
        check_message(op_binary, 256 * 2**10)  # 9_4_7
        check_message(op_binary, 2**20)  # 9_4_8
        check_message(op_binary, 4 * 2**20)  # 9_4_9

    check_payload(op_text, 2**20, chopsize=64)  # 9_5_1
    check_payload(op_text, 2**20, chopsize=128)  # 9_5_2
    check_payload(op_text, 2**20, chopsize=256)  # 9_5_3
    check_payload(op_text, 2**20, chopsize=512)  # 9_5_4
    check_payload(op_text, 2**20, chopsize=1024)  # 9_5_5
    check_payload(op_text, 2**20, chopsize=2048)  # 9_5_6

    check_payload(op_binary, 2**20, chopsize=64)  # 9_6_1
    check_payload(op_binary, 2**20, chopsize=128)  # 9_6_2
    check_payload(op_binary, 2**20, chopsize=256)  # 9_6_3
    check_payload(op_binary, 2**20, chopsize=512)  # 9_6_4
    check_payload(op_binary, 2**20, chopsize=1024)  # 9_6_5
    check_payload(op_binary, 2**20, chopsize=2048)  # 9_6_6

    close_connection(sock)


def test_java_websockets_10_1_1():
    client.load('websockets_mirror')

    _, sock, _ = ws.upgrade()

    payload = '*' * 65536

    ws.message(sock, ws.OP_TEXT, payload, fragmention_size=1300)

    frame = ws.message_read(sock)
    check_frame(frame, True, ws.OP_TEXT, payload)

    close_connection(sock)


# settings


def test_java_websockets_max_frame_size():
    client.load('websockets_mirror')

    assert 'success' in client.conf(
        {'http': {'websocket': {'max_frame_size': 100}}}, 'settings'
    ), 'configure max_frame_size'

    _, sock, _ = ws.upgrade()

    payload = '*' * 94
    opcode = ws.OP_TEXT

    ws.frame_write(sock, opcode, payload)  # frame length is 100

    frame = ws.frame_read(sock)
    check_frame(frame, True, opcode, payload)

    payload = '*' * 95

    ws.frame_write(sock, opcode, payload)  # frame length is 101
    check_close(sock, 1009)  # 1009 - CLOSE_TOO_LARGE


def test_java_websockets_read_timeout():
    client.load('websockets_mirror')

    assert 'success' in client.conf(
        {'http': {'websocket': {'read_timeout': 5}}}, 'settings'
    ), 'configure read_timeout'

    _, sock, _ = ws.upgrade()

    frame = ws.frame_to_send(ws.OP_TEXT, 'blah')
    sock.sendall(frame[:2])

    time.sleep(2)

    check_close(sock, 1001)  # 1001 - CLOSE_GOING_AWAY


def test_java_websockets_keepalive_interval():
    client.load('websockets_mirror')

    assert 'success' in client.conf(
        {'http': {'websocket': {'keepalive_interval': 5}}}, 'settings'
    ), 'configure keepalive_interval'

    _, sock, _ = ws.upgrade()

    frame = ws.frame_to_send(ws.OP_TEXT, 'blah')
    sock.sendall(frame[:2])

    time.sleep(2)

    frame = ws.frame_read(sock)
    check_frame(frame, True, ws.OP_PING, '')  # PING frame

    sock.close()


def test_java_websockets_binary_message_too_big_reason():
    # A fragmented binary message over the session's binary buffer used to
    # report the text-message close reason (issue #435); an unfragmented
    # binary message never reaches the check, so this needs two frames.
    client.load('websockets_binary_toobig')

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_BINARY, '*' * 600, fin=False)
    ws.frame_write(sock, ws.OP_CONT, '*' * 600, fin=True)

    frame = ws.frame_read(sock)

    assert frame['opcode'] == ws.OP_CLOSE, 'close opcode'
    assert frame['code'] == 1009, 'close code'
    assert 'text message' not in frame['reason'], 'reason names the wrong kind'
    assert 'binary' in frame['reason'], 'reason names the binary message'

    sock.close()


# async remote (issue #434)


def test_java_websockets_async_text():
    # The asynchronous remote used to send nothing and never call the
    # SendHandler, so the future from sendText(String) never completed.
    client.load('websockets_async')

    _, sock, _ = ws.upgrade()

    # SendHandler form

    ws.frame_write(sock, ws.OP_TEXT, 'handler:blah')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'handler:blah')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'handler-ok')

    # Future form

    ws.frame_write(sock, ws.OP_TEXT, 'blah')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'blah')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'future-done')

    # sendObject(Object) with an Integer, which is sent as text

    ws.frame_write(sock, ws.OP_TEXT, 'object:123')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, '123')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'future-done')

    # sendObject(Object) with a String, which has no encoder: the future
    # completes with the failure instead of hanging

    ws.frame_write(sock, ws.OP_TEXT, 'object:blah')
    frame = ws.frame_read(sock)
    assert frame['data'].startswith(b'future-fail: '), 'failed future'
    assert b'EncodeException' in frame['data'], 'failed future reason'

    # the session is usable for a further message after each of them

    ws.frame_write(sock, ws.OP_TEXT, 'handler:again')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'handler:again')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'handler-ok')

    close_connection(sock)


def test_java_websockets_async_binary():
    client.load('websockets_async')

    _, sock, _ = ws.upgrade()

    # SendHandler form

    ws.frame_write(sock, ws.OP_BINARY, b'h\x00\x01\x02\xff')
    check_frame(ws.frame_read(sock), True, ws.OP_BINARY, b'h\x00\x01\x02\xff')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'handler-ok')

    # Future form

    ws.frame_write(sock, ws.OP_BINARY, b'\x00\x01\x02\xff')
    check_frame(ws.frame_read(sock), True, ws.OP_BINARY, b'\x00\x01\x02\xff')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'future-done')

    close_connection(sock)


def test_java_websockets_async_large():
    # A message larger than the module's 8 KiB buffers: the text one still
    # leaves in 8 KiB frames on the asynchronous path, the binary one whole.
    client.load('websockets_async')

    _, sock, _ = ws.upgrade()

    payload = '*' * 65536

    ws.frame_write(sock, ws.OP_TEXT, payload)
    check_frame(ws.message_read(sock), True, ws.OP_TEXT, payload)
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'future-done')

    payload = b'*' * 65536

    ws.frame_write(sock, ws.OP_BINARY, payload)
    check_frame(ws.frame_read(sock), True, ws.OP_BINARY, payload)
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'future-done')

    payload = b'h' + b'*' * 65535

    ws.frame_write(sock, ws.OP_BINARY, payload)
    check_frame(ws.frame_read(sock), True, ws.OP_BINARY, payload)
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'handler-ok')

    close_connection(sock)


def test_java_websockets_async_batch():
    # Batched messages are held until the batch is flushed.  flushBatch()
    # used to hand the request a null payload and throw, so disallowing
    # batching failed and the held messages were never sent.
    client.load('websockets_async')

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, 'batch:3')

    for i in range(3):
        check_frame(ws.frame_read(sock), True, ws.OP_TEXT, f'b{i}')

    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'batch-flushed')

    # the session is usable after the flush

    ws.frame_write(sock, ws.OP_TEXT, 'handler:again')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'handler:again')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'handler-ok')

    close_connection(sock)


def test_java_websockets_async_batch_offset():
    # A batched buffer whose position() is not 0: its frame header must give
    # the bytes actually sent, or the frame after it is read as its payload.
    client.load('websockets_async')

    _, sock, _ = ws.upgrade()

    payload = b'o\x00\x01\x02\x03\x04\x05'

    ws.frame_write(sock, ws.OP_BINARY, payload)
    check_frame(ws.frame_read(sock), True, ws.OP_BINARY, payload[3:])
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'after')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'offset-flushed')

    close_connection(sock)


def test_java_websockets_async_nested():
    # A SendHandler that waits on a send of its own, and one that flushes a
    # batch: the nested completion has to run before the handler returns.
    client.load('websockets_async')

    _, sock, _ = ws.upgrade()

    ws.frame_write(sock, ws.OP_TEXT, 'nested:a')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'nested:a')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'nested-b')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'nested-c')
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'nested-done')

    close_connection(sock)


def test_java_websockets_async_threads():
    # Sends from several threads at once: every one of them is echoed and
    # every future completes.  A completion is queued behind a running one
    # only on the same thread; a queue shared by the endpoint could take a
    # second thread's completion after the first had stopped draining it.
    client.load('websockets_async')

    _, sock, _ = ws.upgrade()

    n = 8

    for _ in range(10):
        ws.frame_write(sock, ws.OP_TEXT, f'threads:{n}')

        echoes = set()
        for _ in range(n):
            frame = ws.frame_read(sock)
            assert frame['opcode'] == ws.OP_TEXT, 'echo opcode'
            echoes.add(frame['data'].decode())

        assert echoes == {f't{i}' for i in range(n)}, 'every send echoed'
        check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'threads-done')

    close_connection(sock)


def test_java_websockets_async_16m(is_unsafe):
    if not is_unsafe:
        pytest.skip('unsafe, long run')

    # The asynchronous text path sends a message 8 KiB at a time and starts
    # each part from the completion of the previous one; a 16 MiB message is
    # 2048 parts, which must not nest on the stack.
    client.load('websockets_async')

    assert 'success' in client.conf(
        {
            'http': {
                'websocket': {
                    'max_frame_size': 33554432,
                    'keepalive_interval': 0,
                }
            }
        },
        'settings',
    ), 'increase max_frame_size and keepalive_interval'

    _, sock, _ = ws.upgrade()

    payload = '*' * 16 * 2**20

    ws.frame_write(sock, ws.OP_TEXT, payload)
    check_frame(ws.message_read(sock), True, ws.OP_TEXT, payload)
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'future-done')

    payload = b'*' * 16 * 2**20

    ws.frame_write(sock, ws.OP_BINARY, payload)
    check_frame(ws.frame_read(sock), True, ws.OP_BINARY, payload)
    check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'future-done')

    close_connection(sock)


def check_buffers(remote):
    # Heap buffers whose bytes do not start at their array's first one: a
    # slice with a non-zero arrayOffset() used to be sent from the start of
    # the array, and a read-only buffer failed with ReadOnlyBufferException.
    client.load('websockets_buffers')

    _, sock, _ = ws.upgrade()

    for kind in ('slice', 'readonly'):
        ws.frame_write(sock, ws.OP_TEXT, f'{remote}:{kind}')
        check_frame(ws.frame_read(sock), True, ws.OP_BINARY, b'456789ab')
        check_frame(ws.frame_read(sock), True, ws.OP_TEXT, 'done')

    close_connection(sock)


def test_java_websockets_buffers():
    check_buffers('basic')


def test_java_websockets_async_buffers():
    check_buffers('async')
