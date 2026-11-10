async def application(scope, receive, send):
    assert scope['type'] == 'websocket'

    while True:
        m = await receive()
        if m['type'] == 'websocket.connect':
            # Iterate the tuple so every slot is touched.  An over-allocated
            # scope['subprotocols'] tuple that kept a NULL slot (an empty
            # Sec-WebSocket-Protocol segment) crashes the worker right here.
            joined = ','.join(scope['subprotocols'])

            await send(
                {
                    'type': 'websocket.accept',
                    'headers': [(b'x-subprotocols', joined.encode())],
                }
            )

        if m['type'] == 'websocket.disconnect':
            break
