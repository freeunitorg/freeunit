"""Bytes after a chunked body must not grow the router buffer cache.

With chunked_transform on, a chunked request body is kept in a file
buffer.  When bytes of the next request follow the body in the same read,
the router copies them into a new buffer whose size depends on their
length.  The buffer must be released when the request that was read
from it ends, or when the connection closes.  It must not leave a cached
block for each distinct length in the router.

The keep-alive test sends many chunked requests on one connection.  The
read that ends each body also brings the start of the next request.  The
test reads the router VmRSS while the connection is still open.

The sweep test opens one connection for each distinct number of bytes.
On even sizes the bytes are a complete pipelined request with
"Connection: close", and the test checks that both responses arrive.  On
odd sizes the bytes are the start of a request that never completes, and
the client closes after the first response.  The test compares the router
VmRSS before and after the run.
"""

import re
import socket
import sys
import time
from pathlib import Path

import pytest

from conftest import pid_by_name
from unit import port as port_map
from unit.applications.proto import ApplicationProto

client = ApplicationProto()

N = 6000

# Above header_buffer_size (2048), so each size needs a new buffer.  Below
# large_header_buffer_size (8192), so an incomplete request still gets a
# large header buffer.
SIZES = [2100 + i for i in range(N)]
SUM_KB = sum(SIZES) // 1024

# The body must be larger than what is left of the header buffer, so that
# the following bytes are read into the file buffer, and small enough that
# the body and the following bytes come in one body_buffer_size read.
BODY_SIZE = 4000

# The unfixed router keeps about one block per size: about SUM_KB.
# The fixed router keeps no block per size.
BOUND_KB = SUM_KB // 4

KEEPALIVE_REQUESTS = 6000

LINUX_ONLY = pytest.mark.skipif(
    sys.platform != 'linux', reason='reads the router /proc/<pid>/status'
)


def router_status_kb(field):
    pid = pid_by_name('unit: router')
    assert pid is not None, 'router pid'
    text = Path(f'/proc/{pid}/status').read_text(encoding='utf-8')
    return int(re.search(rf'{field}:\s+(\d+) kB', text).group(1))


@pytest.fixture(autouse=True)
def setup_method_fixture():
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
            "applications": {},
            "settings": {"http": {"chunked_transform": True}},
        }
    ), 'chunked_transform configuration'


def chunked_request():
    body = b'b' * BODY_SIZE

    return (
        b'POST /r HTTP/1.1\r\nHost: localhost\r\n'
        b'Transfer-Encoding: chunked\r\n\r\n'
        + (b'%x\r\n' % len(body))
        + body
        + b'\r\n0\r\n\r\n'
    )


def keepalive_split(i):
    # The number of bytes of request i + 1 that are sent with the end of
    # request i: the header and a part of the chunk.  More than
    # header_buffer_size (2048), so that the router needs a new buffer, and
    # less than the whole request, so that the rest of the body of request
    # i + 1 is read from the socket.
    return 2100 + i % 1900


@LINUX_ONLY
def test_chunked_rest_buffer_keepalive():
    request = chunked_request()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(10)
    sock.connect(('127.0.0.1', port_map.port(8080)))

    base = router_status_kb('VmRSS')
    total = 0
    sent = 0

    try:
        for i in range(KEEPALIVE_REQUESTS):
            if i == KEEPALIVE_REQUESTS - 1:
                split = 0

            else:
                split = keepalive_split(i)

            # The rest of request i, and the start of request i + 1.
            sock.sendall(request[sent:] + request[:split])
            total += split
            sent = split

            data = b''

            while b'\r\n\r\n' not in data:
                part = sock.recv(4096)
                assert part, f'request {i}: the connection was closed'
                data += part

            assert data.startswith(b'HTTP/1.1 200 OK\r\n'), (i, data[:200])
            assert data.endswith(b'\r\n\r\n'), (i, data[:200])

        time.sleep(1)
        growth = router_status_kb('VmRSS') - base

    finally:
        sock.close()

    bound = total // 1024 // 8

    print(
        f'chunked rest, keep-alive: {KEEPALIVE_REQUESTS} requests, '
        f'sum {total // 1024} kB, router RSS growth {growth} kB, '
        f'bound {bound} kB'
    )

    # See test_chunked_rest_buffer_cache() for the sanitizer guard.
    if router_status_kb('VmPeak') < 100 * 1024 * 1024:
        assert growth < bound, 'router keeps the buffers of an open connection'


def trailing_bytes(size, complete):
    head = b'GET /p HTTP/1.1\r\nHost: localhost\r\nX-Pad: '

    if complete:
        tail = b'\r\nConnection: close\r\n\r\n'

    else:
        tail = b''

    data = head + b'a' * (size - len(head) - len(tail)) + tail
    assert len(data) == size
    return data


def one_connection(size):
    complete = size % 2 == 0
    req = chunked_request()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(10)
    sock.connect(('127.0.0.1', port_map.port(8080)))
    sock.sendall(req + trailing_bytes(size, complete))

    data = b''

    try:
        while True:
            if not complete and b'\r\n\r\n' in data:
                break

            part = sock.recv(4096)
            if not part:
                break

            data += part

    finally:
        sock.close()

    if complete:
        assert data.count(b'HTTP/1.1 200 OK\r\n') == 2, (
            f'size {size}: two responses',
            data[:200],
        )

    else:
        assert data.startswith(b'HTTP/1.1 200 OK\r\n'), (
            f'size {size}: first response',
            data[:200],
        )


@LINUX_ONLY
def test_chunked_rest_buffer_cache():
    base = router_status_kb('VmRSS')

    for size in SIZES:
        one_connection(size)

    time.sleep(1)
    growth = router_status_kb('VmRSS') - base

    print(
        f'chunked rest: {N} sizes {SIZES[0]}..{SIZES[-1]}, '
        f'sum {SUM_KB} kB, router RSS growth {growth} kB, bound {BOUND_KB} kB'
    )

    # The bound holds only on an uninstrumented build.  AddressSanitizer
    # reserves a shadow of terabytes (VmPeak) and keeps freed memory in its
    # own allocator, so RSS grows under it for the fixed code too.
    if router_status_kb('VmPeak') < 100 * 1024 * 1024:
        assert growth < BOUND_KB, 'router keeps a buffer for each size'
