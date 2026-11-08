"""Expect: 100-continue (RFC 9110, 10.1.1).

The router sends "100 Continue" when it is about to wait for more of the body.
A part of the body that came with the header does not stop the 100, as in
nginx.  A request that the router answers from its header alone gets its final
status with no 100.  The tests use raw sockets, because the client must wait
for the 100 before it sends the body.
"""

import socket
import ssl
import time

import pytest

from unit.applications.lang.python import ApplicationPython
from unit.applications.tls import ApplicationTLS
from unit.option import option
from unit import port as port_map

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

CONTINUE = b'HTTP/1.1 100 Continue\r\n\r\n'

# The longest wait for a 100.  The wait ends when the 100 arrives, so a long
# value costs nothing.  It covers a slow sanitizer build.
CONTINUE_WAIT = 2

# A test that expects nothing to arrive waits this long.
SILENCE_WAIT = 0.5


def app(name):
    python_dir = f'{option.test_dir}/python'

    return {
        "type": client.get_application_type(),
        "processes": {"spare": 0},
        "path": f'{python_dir}/{name}',
        "working_directory": f'{python_dir}/{name}',
        "module": "wsgi",
    }


@pytest.fixture(autouse=True)
def setup_method_fixture():
    assert 'success' in client.conf(
        {
            "listeners": {
                "*:8080": {"pass": "applications/mirror"},
                "*:8081": {"pass": "routes"},
                "*:8082": {"pass": "applications/header_fields"},
            },
            "routes": [{"action": {"proxy": "http://127.0.0.1:8082"}}],
            "applications": {
                "mirror": app('mirror'),
                "header_fields": app('header_fields'),
            },
            "settings": {"http": {"chunked_transform": True}},
        }
    )


def connect(port=8080, wrap=None):
    sock = socket.create_connection(('127.0.0.1', port_map.port(port)))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    if wrap is not None:
        sock = wrap(sock)

    return sock


def head(expect='100-continue', length=5, version='1.1', extra=''):
    """A POST header section with no body."""

    request = f'POST / HTTP/{version}\r\nHost: localhost\r\n'

    if expect is not None:
        request += f'Expect: {expect}\r\n'

    if length is not None:
        request += f'Content-Length: {length}\r\n'

    return (request + extra + '\r\n').encode()


def recv_for(sock, timeout, size=None):
    """What arrives within timeout seconds, up to size bytes or EOF."""

    data = b''
    end = time.monotonic() + timeout

    while size is None or len(data) < size:
        left = end - time.monotonic()
        if left <= 0:
            break

        sock.settimeout(left)

        try:
            part = sock.recv(4096)
        except socket.timeout:
            break

        if not part:
            break

        data += part

    return data


def recv_continue(sock):
    return recv_for(sock, CONTINUE_WAIT, len(CONTINUE))


def recv_response(sock):
    """One response framed by Content-Length: status line, fields, body."""

    data = b''
    sock.settimeout(10)

    while b'\r\n\r\n' not in data:
        part = sock.recv(4096)
        assert part, f'closed before the header: {data!r}'
        data += part

    header, _, body = data.partition(b'\r\n\r\n')
    lines = header.decode().split('\r\n')

    fields = {}
    for line in lines[1:]:
        name, _, value = line.partition(':')
        fields[name.strip().lower()] = value.strip()

    length = int(fields.get('content-length', 0))

    while len(body) < length:
        part = sock.recv(4096)
        assert part, f'closed in the body: {body!r}'
        body += part

    return lines[0], fields, body


def recv_all(sock):
    return recv_for(sock, 10)


@pytest.mark.parametrize('expect', ['100-continue', '100-Continue'])
def test_expect_continue(expect):
    with connect() as sock:
        sock.sendall(head(expect, extra='Connection: close\r\n'))

        assert recv_continue(sock) == CONTINUE, 'no 100 within CONTINUE_WAIT'

        sock.sendall(b'hello')

        status, _, body = recv_response(sock)
        assert status.startswith('HTTP/1.1 200'), status
        assert body == b'hello'


def test_expect_continue_chunked():
    with connect() as sock:
        sock.sendall(
            head(
                length=None,
                extra='Transfer-Encoding: chunked\r\nConnection: close\r\n',
            )
        )

        assert recv_continue(sock) == CONTINUE, 'no 100 within CONTINUE_WAIT'

        sock.sendall(b'5\r\nhello\r\n0\r\n\r\n')

        status, _, body = recv_response(sock)
        assert status.startswith('HTTP/1.1 200'), status
        assert body == b'hello'


def test_expect_continue_keepalive():
    """Each request on a connection gets its own 100.  The expectation of one
    request does not carry over to the next one."""

    with connect() as sock:

        for body in (b'hello', b'second'):
            sock.sendall(head(length=len(body)))

            assert recv_continue(sock) == CONTINUE, f'no 100 before {body!r}'

            sock.sendall(body)

            status, _, got = recv_response(sock)
            assert status.startswith('HTTP/1.1 200'), status
            assert got == body

        sock.sendall(head(expect=None, extra='Connection: close\r\n'))

        assert recv_for(sock, SILENCE_WAIT) == b'', '100 without Expect'

        sock.sendall(b'third')

        data = recv_all(sock)
        assert data.startswith(b'HTTP/1.1 200'), data
        assert data.endswith(b'\r\n\r\nthird'), data


def test_expect_continue_body_bytes_sent(wait_for_record):
    """$body_bytes_sent does not count the 100."""

    assert 'success' in client.conf(
        {
            'path': f'{option.temp_dir}/access.log',
            'format': '$uri $status $body_bytes_sent',
        },
        'access_log',
    )

    with connect() as sock:
        sock.sendall(
            b'POST /bbs HTTP/1.1\r\nHost: localhost\r\n'
            b'Expect: 100-continue\r\nContent-Length: 5\r\n'
            b'Connection: close\r\n\r\n'
        )

        assert recv_continue(sock) == CONTINUE, 'no 100 within CONTINUE_WAIT'

        sock.sendall(b'hello')
        recv_all(sock)

    found = wait_for_record(r'^/bbs 200 (\d+)$', 'access.log')
    assert found is not None, 'no access log record'
    assert found.group(1) == '5', '$body_bytes_sent'


def test_expect_continue_proxy():
    """The router meets the expectation, so a proxied upstream does not get
    the field.  An application gets it."""

    def check(port, seen):
        with connect(port) as sock:
            sock.sendall(head(extra='Connection: close\r\n'))

            assert recv_continue(sock) == CONTINUE, f'no 100 on {port}'

            sock.sendall(b'hello')

            status, fields, _ = recv_response(sock)
            assert status.startswith('HTTP/1.1 200'), status

            names = fields['all-headers'].split(',')
            assert ('HTTP_EXPECT' in names) == seen, f'{port}: {names}'

    check(port=8082, seen=True)
    check(port=8081, seen=False)


def test_expect_continue_tls():
    if not option.available['modules'].get('openssl'):
        pytest.skip('requires openssl')

    tls = ApplicationTLS()
    tls.certificate()

    assert 'success' in client.conf(
        {
            "pass": "applications/mirror",
            "tls": {"certificate": "default"},
        },
        'listeners/*:8080',
    )

    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE

    with connect(wrap=context.wrap_socket) as sock:
        sock.sendall(head(extra='Connection: close\r\n'))

        assert recv_continue(sock) == CONTINUE, 'no 100 within CONTINUE_WAIT'

        sock.sendall(b'hello')

        status, _, body = recv_response(sock)
        assert status.startswith('HTTP/1.1 200'), status
        assert body == b'hello'


@pytest.mark.parametrize(
    'request_head',
    [
        # RFC 9110: an HTTP/1.0 expectation is ignored.
        head(version='1.0', extra='Connection: close\r\n'),
        # Only "100-continue" is known; the others are ignored, as nginx
        # does.
        head(expect='100-continue-please', extra='Connection: close\r\n'),
        head(expect='foo', extra='Connection: close\r\n'),
    ],
    ids=['http10', 'other-value', 'unknown'],
)
def test_expect_continue_ignored(request_head):
    with connect() as sock:
        sock.sendall(request_head)

        assert recv_for(sock, SILENCE_WAIT) == b'', 'answered before the body'

        sock.sendall(b'hello')

        data = recv_all(sock)
        assert data.startswith(b'HTTP/1.1 200'), data
        assert CONTINUE not in data, data


@pytest.mark.parametrize(
    'request_head, status',
    [
        # No body.
        (head(length=None, extra='Connection: close\r\n'), 200),
        (head(length=0, extra='Connection: close\r\n'), 200),
        # Rejected from the header alone.
        (head(length=None, extra='Transfer-Encoding: gzip\r\n'), 501),
        (head(extra='Transfer-Encoding: chunked\r\n'), 400),
    ],
    ids=['no-length', 'zero-length', 'te-unsupported', 'te-and-length'],
)
def test_expect_continue_final_status(request_head, status):
    """The router answers from the header alone, with no 100."""

    with connect() as sock:
        sock.sendall(request_head)

        data = recv_all(sock)
        assert data.startswith(f'HTTP/1.1 {status}'.encode()), data
        assert CONTINUE not in data, data


def test_expect_continue_max_body_size():
    """A body over max_body_size gets 413 at once, with no 100."""

    assert 'success' in client.conf('10', 'settings/http/max_body_size')

    with connect() as sock:
        sock.sendall(head(length=11))

        data = recv_all(sock)
        assert data.startswith(b'HTTP/1.1 413'), data
        assert CONTINUE not in data, data


def test_expect_continue_length_required():
    """With chunked_transform off, a chunked body gets 411 at once, with no
    100."""

    assert 'success' in client.conf('false', 'settings/http/chunked_transform')

    with connect() as sock:
        sock.sendall(head(length=None, extra='Transfer-Encoding: chunked\r\n'))

        data = recv_all(sock)
        assert data.startswith(b'HTTP/1.1 411'), data
        assert CONTINUE not in data, data


def test_expect_continue_part_of_body():
    """A part of the body comes with the header.  The router sends the 100
    while it waits for the rest, as nginx does."""

    with connect() as sock:
        sock.sendall(head(length=10, extra='Connection: close\r\n') + b'hello')

        assert recv_continue(sock) == CONTINUE, 'no 100 after part of the body'

        sock.sendall(b'world')

        status, _, body = recv_response(sock)
        assert status.startswith('HTTP/1.1 200'), status
        assert body == b'helloworld'


def test_expect_continue_chunk_framing():
    """Only the framing of the first chunk comes with the header.  The client
    waits for the 100 before it sends the chunk data."""

    with connect() as sock:
        sock.sendall(
            head(
                length=None,
                extra='Transfer-Encoding: chunked\r\nConnection: close\r\n',
            )
            + b'5\r\n'
        )

        assert recv_continue(sock) == CONTINUE, 'no 100 after chunk framing'

        sock.sendall(b'hello\r\n0\r\n\r\n')

        status, _, body = recv_response(sock)
        assert status.startswith('HTTP/1.1 200'), status
        assert body == b'hello'
