import socket
import struct
import time

import pytest

from unit.applications.lang.python import ApplicationPython

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

# send_timeout is deliberately short so the router's send timer fires while a
# streamed write is still pending (the straddle window this test drives).
SEND_TIMEOUT = 1

# A large single-write body: body_generate returns it as one WSGI list element,
# so the router streams it to the (stalled) client across many re-armed write
# items -- every re-arm captures the conn's socket task in deferred work.
BODY_LEN = 8 * 1024 * 1024

# Bytes the client pulls before it stalls.  Enough to get the response line out
# and leave the bulk of the body buffered in the router with a pending write.
PREFIX_LEN = 16 * 1024

# Must exceed SEND_TIMEOUT so the send timer expires while the write is pending.
STALL_SECS = 1.5

# Debug builds assert the connection-task ownership invariant deterministically;
# keep one end-to-end straddle as ASan smoke coverage for the abort lifecycle.
ITERATIONS = 1

ADDR = ('127.0.0.1', 8080)


@pytest.fixture(autouse=True)
def setup_method_fixture():
    # load() asserts 'success' internally; it wires listener *:8080 ->
    # applications/body_generate.
    client.load('body_generate')

    assert 'success' in client.conf(
        {'http': {'send_timeout': SEND_TIMEOUT}}, 'settings'
    )

    yield

    # The default no-restart suite preserves /settings between tests.
    assert 'success' in client.conf_delete('settings/http/send_timeout')


def _request(length, connection):
    return (
        f'GET / HTTP/1.1\r\n'
        f'Host: localhost\r\n'
        f'X-Length: {length}\r\n'
        f'Connection: {connection}\r\n\r\n'
    ).encode()


def _read_response(sock):
    # Read one HTTP/1.1 response: headers, then exactly Content-Length bytes.
    buf = b''
    while b'\r\n\r\n' not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            return None, b''
        buf += chunk

    head, _, body = buf.partition(b'\r\n\r\n')
    lines = head.split(b'\r\n')
    status = int(lines[0].split()[1])

    length = None
    for line in lines[1:]:
        if line.lower().startswith(b'content-length:'):
            length = int(line.split(b':', 1)[1].strip())

    while length is not None and len(body) < length:
        chunk = sock.recv(4096)
        if not chunk:
            break
        body += chunk

    return status, body if length is None else body[:length]


def _stall_and_abort(i):
    # Open a connection, start streaming the large body, read a small prefix,
    # then stall past send_timeout so the send timer fires while a write item
    # is still pending -- and abort with a RST (SO_LINGER 0) mid-stream.  This
    # is the straddle that, pre-fix, let deferred write items capture the
    # request-pool task.
    sock = socket.create_connection(ADDR)
    sock.settimeout(10)
    try:
        sock.sendall(_request(BODY_LEN, 'keep-alive'))

        got = 0
        while got < PREFIX_LEN:
            chunk = sock.recv(4096)
            if not chunk:
                break
            got += len(chunk)

        assert got > 0, f'streamed prefix on iteration {i}'

        # Let the send_timeout fire mid-stream (write item pending).
        time.sleep(STALL_SECS)

        # Hard RST rather than an orderly FIN, to abort while the router still
        # has the body queued.
        sock.setsockopt(
            socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)
        )
    finally:
        sock.close()


def _keepalive_probe(i):
    # A fresh keep-alive connection issuing several small requests.  It proves
    # the router is still alive and serving correctly after the abort, and adds
    # request-pool allocator churn while exercising keep-alive request resets.
    sock = socket.create_connection(ADDR)
    sock.settimeout(10)
    try:
        for j in range(4):
            sock.sendall(_request(10, 'keep-alive'))
            status, body = _read_response(sock)
            assert status == 200, f'probe status iteration {i}.{j}'
            assert body == b'X' * 10, f'probe body iteration {i}.{j}'
    finally:
        sock.close()


def test_conn_task_race_send_timeout_abort():
    # Regression for freeunit#156: a send-timeout mid-stream abort straddling
    # keep-alive reuse dereferenced a request-pool task through a deferred conn
    # write item after the request pool was freed.  On the ASan sanitize leg a
    # regression surfaces as a heap-use-after-free report (the workflow's
    # "Fail on sanitizer reports" guard is the actual oracle); without ASan it
    # is a rare router crash, caught here by the keep-alive probe's liveness
    # and body-correctness assertions.
    for i in range(ITERATIONS):
        _stall_and_abort(i)
        _keepalive_probe(i)
