async def application(scope, receive, send):
    assert scope['type'] == 'websocket'

    while True:
        try:
            m = await receive()

        except Exception as e:
            await send(
                {
                    'type': 'websocket.send',
                    'text': type(e).__name__,
                }
            )
            await send({'type': 'websocket.close'})
            break

        if m['type'] == 'websocket.connect':
            await send({'type': 'websocket.accept'})

        if m['type'] == 'websocket.receive':
            await send(
                {
                    'type': 'websocket.send',
                    'bytes': m.get('bytes', None),
                    'text': m.get('text', None),
                }
            )

        if m['type'] == 'websocket.disconnect':
            break
