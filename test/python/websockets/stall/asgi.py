# Copyright (C) FreeUnit contributors.

import time


async def application(scope, receive, send):
    if scope['type'] == 'websocket':
        while True:
            m = await receive()
            if m['type'] == 'websocket.connect':
                await send({'type': 'websocket.accept'})

            if m['type'] == 'websocket.receive':
                if m.get('text') == 'stall':
                    # Block the event loop, so libunit reads nothing from
                    # the port queue while the router fills it.
                    time.sleep(2)
                    continue

                await send(
                    {
                        'type': 'websocket.send',
                        'bytes': m.get('bytes', None),
                        'text': m.get('text', None),
                    }
                )

            if m['type'] == 'websocket.disconnect':
                break
