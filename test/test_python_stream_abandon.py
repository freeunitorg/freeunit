"""A client that goes away mid-response must not free the worker (#996).

The worker keeps running the request after the client closed the
connection.  Until it sends its last message, the router must count its
process busy: not idle, and not reapable by idle_timeout.
"""

import time

from unit.applications.lang.python import ApplicationPython
from unit.utils import waitforfiles

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()


def test_python_stream_abandon_keeps_worker_busy(temp_dir):
    client.load(
        'stream_abandon',
        processes={'max': 1, 'spare': 0, 'idle_timeout': 1},
    )

    marker = f'{temp_dir}/done'

    sock = client.get(
        headers={
            'Host': 'localhost',
            'X-Duration': '3',
            'X-Marker': marker,
            'Connection': 'close',
        },
        no_recv=True,
    )
    assert sock.recv(4096).startswith(b'HTTP/1.1 200'), 'response started'
    sock.close()

    # The next chunks hit the closed connection.  Sample the idle count
    # past idle_timeout while the worker is still streaming.
    idle = []
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        idle.append(
            client.conf_get('/status/applications/stream_abandon/processes')[
                'idle'
            ]
        )
        time.sleep(0.25)

    assert max(idle) == 0, f'busy worker counted idle: {idle}'

    # The worker finishes the request, then serves the next one.
    assert waitforfiles(marker, timeout=100), 'the worker finished'
    assert client.get()['status'] == 200, 'next request'
