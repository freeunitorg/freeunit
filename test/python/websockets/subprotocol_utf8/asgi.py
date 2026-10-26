async def application(scope, receive, send):
    assert scope['type'] == 'websocket'

    while True:
        m = await receive()
        if m['type'] == 'websocket.connect':
            # A subprotocol with a character above U+00FF.  Python keeps
            # such a string as UCS-2, not as UTF-8.
            await send(
                {
                    'type': 'websocket.accept',
                    'subprotocol': 'chat-т',
                }
            )

        if m['type'] == 'websocket.disconnect':
            break
