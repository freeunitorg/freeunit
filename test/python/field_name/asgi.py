async def application(scope, receive, send):
    assert scope['type'] == 'http'

    await send(
        {
            'type': 'http.response.start',
            'status': 200,
            'headers': [
                (b'content-type', b'text/plain'),
                (b'x-fnup', b'3'),
            ],
        }
    )

    await send({'type': 'http.response.body', 'body': b'0123456789'})
