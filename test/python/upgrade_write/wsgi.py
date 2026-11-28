# Copyright (C) FreeUnit contributors.

# A WebSocket handshake gets 101, then 400 pieces of 64 KiB, written
# after the client has had time to go away.  The application has no
# WebSocket callbacks, so it is not told that the client left.  Any other
# request gets "ok".

import time


def application(environ, start_response):
    if environ.get('HTTP_UPGRADE', '').lower() != 'websocket':
        start_response('200 OK', [('Content-Length', '2')])
        return [b'ok']

    start_response('101 Switching Protocols', [])

    def body():
        yield b''
        time.sleep(0.5)

        piece = b'a' * 65536

        for _ in range(400):
            yield piece

    return body()
