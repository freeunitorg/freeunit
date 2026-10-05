import asyncio
from urllib.parse import parse_qs


async def application(scope, receive, send):
    assert scope['type'] == 'http'

    query = parse_qs(scope['query_string'].decode())

    delay = int(query.get('delay', ['0'])[0])
    body = query.get('body', [''])[0].encode()

    await send(
        {
            'type': 'http.response.start',
            'status': 200,
            'headers': [(b'x-header', b'1')],
        }
    )

    # Unit sends the response header for a body message without "body".
    # An empty "body" sends nothing.
    await send({'type': 'http.response.body', 'more_body': True})

    await asyncio.sleep(delay)

    await send({'type': 'http.response.body', 'body': body})
