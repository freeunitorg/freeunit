# Copyright (C) FreeUnit contributors.


async def application(scope, receive, send):
    assert (await receive())['type'] == 'websocket.connect'
    await send({'type': 'websocket.accept'})
    assert (await receive())['type'] == 'websocket.receive'
    await send({'type': 'websocket.send', 'bytes': b'x' * (4 * 1024 * 1024)})
    await send({'type': 'websocket.close', 'code': 1008})
    raise RuntimeError('exception after close')
