"""Application-queue doorbells on the bare libunit app (#708).

An "external" application that allocates its own contexts gets one
doorbell per idle context: a burst of N one-second requests to a process
with N contexts finishes in about a second, not N seconds.

It uses src/test/nxt_unit_app_test.c ("/sleep" holds a context for 1s),
built by `./configure --tests && make tests`.
"""

import socket
import time
from pathlib import Path

import pytest

from unit.applications.proto import ApplicationProto
from unit.option import option

client = ApplicationProto()


def _executable():
    return f'{option.current_dir}/build/unit_app_test'


@pytest.fixture(autouse=True)
def setup_method_fixture():
    if not Path(_executable()).is_file():
        pytest.skip('build/unit_app_test not built (configure --tests)')


def _load(contexts):
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "applications/libunit"}},
            "applications": {
                "libunit": {
                    "type": "external",
                    "processes": 1,
                    "working_directory": option.temp_dir,
                    "executable": _executable(),
                    "arguments": ["-t", str(contexts)],
                }
            },
        }
    )


def _burst(n, url):
    """n requests written back to back on pre-connected sockets, so that
    they reach the router while earlier ones are still queued."""

    req = f'GET {url} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'
    socks = [socket.create_connection(('127.0.0.1', 8080)) for _ in range(n)]

    start = time.monotonic()

    for s in socks:
        s.sendall(req.encode())

    statuses = []

    for s in socks:
        s.settimeout(30)
        data = b''

        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk

        s.close()
        statuses.append(int(data.split(b' ', 2)[1]) if data else 0)

    return time.monotonic() - start, statuses


def test_external_contexts_parallel():
    _load(8)

    # Let all eight contexts register (NEW_PORT) and block on the queue.
    assert client.get()['status'] == 200
    time.sleep(1)

    elapsed, statuses = _burst(8, '/sleep')

    assert statuses == [200] * 8
    assert elapsed < 2.5, f'8 x 1s on 8 contexts took {elapsed:.2f}s'
