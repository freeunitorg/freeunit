"""An upstream response header that does not complete before the timeout.

The proxy keeps the part of the header that it has read in a 64 KiB buffer.
When the upstream sends no complete header for 60 seconds (proxy_timeout,
not a setting), the client gets 504 and the proxy closes the upstream
connection.  The buffer and the memory of the request must be released
then.

The test reads the resident memory (VmRSS) of the router, which runs one
thread.  It sends two rounds of N = 128 requests that time out, and it
measures the growth from the end of the first round to the end of the
second round.

One round is not enough.  A round holds N buffers at the same time, so it
needs that memory whether the buffers are freed after it or not, and the
allocator can keep the memory resident.  The second round shows the
difference.  If the buffers of the first round were freed, the second round
uses their memory again.  If they were kept, it needs memory for N more
buffers.  Six runs on one machine gave 0, -32, 0 and 0 kB with the buffers
freed, and +9012 and +9012 kB with them kept.

An AddressSanitizer build holds freed memory, so there the test skips.
"""

import select
import socket
import sys
import threading

import pytest

from conftest import pid_by_name
from conftest import run_process
from unit import port as port_map
from unit.applications.proto import ApplicationProto
from unit.option import option
from unit.utils import waitforsocket

client = ApplicationProto()

UPSTREAM_PORT = port_map.port(7999)

# Requests that time out in each round.
N = 128

# The start of a response header, without its end.  Big enough to fill most
# of the header buffer, so the buffer memory is resident.
PARTIAL = (
    'HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Pad: ' + 'a' * (60 * 1024)
).encode()

# The proxy timeout of the header read, in seconds.
PROXY_TIMEOUT = 60


def run_server(server_port):
    def respond(conn):
        try:
            data = b''
            while b'\r\n\r\n' not in data:
                if not select.select([conn], [], [], 5)[0]:
                    return

                part = conn.recv(4096)
                if not part:
                    return

                data += part

            conn.sendall(PARTIAL)

            # Send nothing more.  Wait until the proxy closes.
            while select.select([conn], [], [], PROXY_TIMEOUT + 30)[0]:
                if not conn.recv(4096):
                    break

        except OSError:
            pass

        finally:
            conn.close()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('127.0.0.1', server_port))
    sock.listen(N * 2)

    while True:
        conn, _ = sock.accept()
        threading.Thread(target=respond, args=(conn,), daemon=True).start()


@pytest.fixture(autouse=True)
def setup_method_fixture():
    run_process(run_server, UPSTREAM_PORT)
    waitforsocket(UPSTREAM_PORT)

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {"action": {"proxy": f'http://127.0.0.1:{UPSTREAM_PORT}'}}
            ],
            "settings": {"listen_threads": 1},
        }
    ), 'proxy configuration'

    yield

    # The no-restart suite keeps /settings between tests.
    assert 'success' in client.conf_delete('settings/listen_threads')


def batch(target, timeout):
    """Send N requests at the same time.  Return the status lines."""

    socks = []

    for _ in range(N):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect(('127.0.0.1', port_map.port(8080)))
        sock.sendall(
            f'GET {target} HTTP/1.1\r\n'
            'Host: localhost\r\n'
            'Connection: close\r\n'
            '\r\n'.encode()
        )
        socks.append(sock)

    statuses = []

    for sock in socks:
        data = b''

        try:
            while True:
                part = sock.recv(65536)
                if not part:
                    break

                data += part

        except OSError:
            pass

        finally:
            sock.close()

        statuses.append(data[:12].decode(errors='replace'))

    return statuses


def rss(pid):
    with open(f'/proc/{pid}/status', encoding='utf-8') as f:
        for line in f:
            if line.startswith('VmRSS:'):
                return int(line.split()[1])

    return None


@pytest.mark.skipif(
    sys.platform != 'linux', reason='reads the router /proc/<pid>/status'
)
def test_proxy_header_timeout_releases_buffer():
    if option.configure_flag['asan']:
        pytest.skip('AddressSanitizer holds freed memory, so the RSS grows')

    router = pid_by_name('unit: router')
    assert router is not None, 'router pid'

    rounds = []

    for _ in range(2):
        statuses = batch('/hold', PROXY_TIMEOUT + 30)
        assert statuses == ['HTTP/1.1 504'] * N, statuses[:3]

        rounds.append(rss(router))

    growth = rounds[1] - rounds[0]

    print(
        f'router VmRSS kB: after the first round {rounds[0]}, after the '
        f'second round {rounds[1]} (growth {growth})'
    )

    # With the buffers kept, the second round adds N buffers of 64 KiB:
    # about 9000 kB.  With them freed, it adds nothing.  3000 kB is a
    # third of the first and far above the second.
    assert growth < 3000, (
        f'router memory grew by {growth} kB in the second round of {N} '
        f'timeouts: {rounds[0]} -> {rounds[1]}'
    )
