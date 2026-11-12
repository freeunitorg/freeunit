# Copyright (C) FreeUnit contributors.

# A WebSocket text message "<count> <size>" asks for <count> binary
# messages of <size> bytes each.  They are sent at once, whether the client
# reads them or not.  With a third word "close", the application then
# closes the WebSocket and returns.  An HTTP request gets "ok".


async def application(scope, receive, send):
    if scope['type'] == 'http':
        await send(
            {
                'type': 'http.response.start',
                'status': 200,
                'headers': [(b'content-length', b'2')],
            }
        )
        await send({'type': 'http.response.body', 'body': b'ok'})
        return

    if scope['type'] != 'websocket':
        return

    while True:
        m = await receive()

        if m['type'] == 'websocket.connect':
            await send({'type': 'websocket.accept'})

        elif m['type'] == 'websocket.receive':
            words = m['text'].split()
            count, size = int(words[0]), int(words[1])
            payload = b'x' * size

            for _ in range(count):
                await send({'type': 'websocket.send', 'bytes': payload})

            if words[2:] == ['close']:
                await send({'type': 'websocket.close', 'code': 1000})
                return

        elif m['type'] == 'websocket.disconnect':
            break
