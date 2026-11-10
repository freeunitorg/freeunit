async def application(scope, receive, send):
    assert scope['type'] == 'http'

    length = int(dict(scope['headers']).get(b'x-length', b'300'))

    await send(
        {
            'type': 'http.response.start',
            'status': 200,
            'headers': [
                (b'content-length', b'4'),
                (b'x' * length, b'value'),
            ],
        }
    )

    await send({'type': 'http.response.body', 'body': b'body'})
