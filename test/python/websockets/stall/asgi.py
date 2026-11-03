# Copyright (C) FreeUnit contributors.

import os
import time
from pathlib import Path


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
                    gate = os.environ.get('UNIT_TEST_STALL_GATE')
                    if gate is None:
                        time.sleep(2)
                    else:
                        Path(gate + '.started').touch()
                        deadline = time.monotonic() + 60
                        while not Path(gate + '.release').exists():
                            if time.monotonic() >= deadline:
                                raise RuntimeError('stall gate not released')
                            time.sleep(0.01)
                        Path(gate + '.released').touch()
                        # The timeout test has already closed the socket;
                        # do not echo the queued flood during teardown.
                        return
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
