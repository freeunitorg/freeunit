import os
import re
import select
import socket
import subprocess
import threading
import time

import pytest

from conftest import run_process
from unit.applications.lang.python import ApplicationPython
from unit.utils import waitforsocket

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()
SERVER_PORT = 7999


@pytest.fixture(autouse=True)
def setup_method_fixture():
    run_process(run_server, SERVER_PORT)
    waitforsocket(SERVER_PORT)

    assert 'success' in client.conf(
        {
            "settings": {
                "http": {
                    "chunked_transform": True
                }
            },
            "listeners": {
                "*:8080": {"pass": "routes"},
            },
            "routes": [
                {"action": {"proxy": f'http://127.0.0.1:{SERVER_PORT}'}}
            ],
        }
    ), 'proxy initial configuration'


def run_server(server_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    server_address = ('127.0.0.1', server_port)
    sock.bind(server_address)
    sock.listen(10)

    def recvall(sock):
        buff_size = 4096 * 4096
        data = b''
        while True:
            rlist = select.select([sock], [], [], 0.1)

            if not rlist[0]:
                break

            part = sock.recv(buff_size)
            data += part

            if not part:
                break

        return data

    while True:
        connection, _ = sock.accept()

        req = """HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked"""

        data = recvall(connection).decode()

        m = re.search('\x0d\x0a\x0d\x0a(.*)', data, re.M | re.S)
        if m is not None:
            body = m.group(1)

            for line in re.split('\r\n', body):
                add = ''
                m1 = re.search(r'(.*)\sX\s(\d+)', line)

                if m1 is not None:
                    add = m1.group(1) * int(m1.group(2))
                else:
                    add = line

                req = f'{req}{add}\r\n'

        for chunk in re.split(r'([@#])', req):
            if chunk in ('@', '#'):
                if chunk == '#':
                    time.sleep(0.1)
                continue

            connection.sendall(chunk.encode())

        connection.close()


def chunks(chunks_lst):
    body = '\r\n\r\n'

    for l, c in chunks_lst:
        body = f'{body}{l}\r\n{c}\r\n'

    return f'{body}0\r\n\r\n'


def get_http10(*args, **kwargs):
    return client.get(*args, http_10=True, **kwargs)


def test_proxy_chunked():
    for _ in range(10):
        assert get_http10(body='\r\n\r\n0\r\n\r\n')['status'] == 200


def test_proxy_chunked_body():
    part = '0123456789abcdef'

    assert (
        get_http10(body=chunks([('1000', f'{part} X 256')]))['body']
        == part * 256
    )
    assert (
        get_http10(body=chunks([('100000', f'{part} X 65536')]))['body']
        == part * 65536
    )
    assert (
        get_http10(
            body=chunks([('1000000', f'{part} X 1048576')]),
            read_buffer_size=4096 * 4096,
        )['body']
        == part * 1048576
    )

    assert (
        get_http10(
            body=chunks([('1000', f'{part} X 256'), ('1000', f'{part} X 256')])
        )['body']
        == part * 256 * 2
    )
    assert (
        get_http10(
            body=chunks(
                [
                    ('100000', f'{part} X 65536'),
                    ('100000', f'{part} X 65536'),
                ]
            )
        )['body']
        == part * 65536 * 2
    )
    assert (
        get_http10(
            body=chunks(
                [
                    ('1000000', f'{part} X 1048576'),
                    ('1000000', f'{part} X 1048576'),
                ]
            ),
            read_buffer_size=4096 * 4096,
        )['body']
        == part * 1048576 * 2
    )


def test_proxy_chunked_fragmented():
    part = '0123456789abcdef'

    assert (
        get_http10(
            body=chunks([('1', hex(i % 16)[2:]) for i in range(4096)]),
        )['body']
        == part * 256
    )


def test_proxy_chunked_send():
    assert get_http10(body='\r\n\r\n@0@\r\n\r\n')['status'] == 200
    assert (
        get_http10(body='\r@\n\r\n2\r@\na@b\r\n2\r\ncd@\r\n0\r@\n\r\n')['body']
        == 'abcd'
    )
    assert (
        get_http10(body='\r\n\r\n2\r#\na#b\r\n##2\r\n#cd\r\n0\r\n#\r#\n')[
            'body'
        ]
        == 'abcd'
    )


def test_proxy_chunked_invalid():
    def check_invalid(body):
        assert get_http10(body=body)['status'] != 200

    check_invalid('\r\n\r0')
    check_invalid('\r\n\r\n\r0')
    check_invalid('\r\n\r\n\r\n0')
    check_invalid('\r\nContent-Length: 5\r\n\r\n0\r\n\r\n')
    check_invalid('\r\n\r\n1\r\nXX\r\n0\r\n\r\n')
    check_invalid('\r\n\r\n2\r\nX\r\n0\r\n\r\n')
    check_invalid('\r\n\r\nH\r\nXX\r\n0\r\n\r\n')
    check_invalid('\r\n\r\n0\r\nX')

    resp = get_http10(body='\r\n\r\n65#\r\nA X 100')
    assert resp['status'] == 200, 'incomplete chunk status'
    assert resp['body'][-5:] != '0\r\n\r\n', 'incomplete chunk'

    resp = get_http10(body='\r\n\r\n64#\r\nA X 100')
    assert resp['status'] == 200, 'no zero chunk status'
    assert resp['body'][-5:] != '0\r\n\r\n', 'no zero chunk'

    assert get_http10(body='\r\n\r\n80000000\r\nA X 100')['status'] == 200
    assert (
        get_http10(body='\r\n\r\n10000000000000000\r\nA X 100')['status'] == 502
    )
    assert (
        len(
            get_http10(
                body='\r\n\r\n1000000\r\nA X 1048576\r\n1000000\r\nA X 100',
                read_buffer_size=4096 * 4096,
            )['body']
        )
        >= 1048576
    )
    assert (
        len(
            get_http10(
                body='\r\n\r\n1000000\r\nA X 1048576\r\nXXX\r\nA X 100',
                read_buffer_size=4096 * 4096,
            )['body']
        )
        >= 1048576
    )


def _recvall(sock):
    buff_size = 4096 * 4096
    data = b''
    while True:
        rlist = select.select([sock], [], [], 0.1)
        if not rlist[0]:
            break
        part = sock.recv(buff_size)
        data += part
        if not part:
            break
    return data


def _serve_loop(server_port, handler):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.bind(('127.0.0.1', server_port))
    sock.listen(10)

    while True:
        connection, _ = sock.accept()
        data = _recvall(connection).decode()
        connection.sendall(handler(data).encode())
        connection.close()


def _echo_body(data):
    m = re.search('\x0d\x0a\x0d\x0a(.*)', data, re.M | re.S)
    return m.group(1) if m is not None else ''


def _get_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def _run_proxy_server(handler, port):
    run_process(_serve_loop, port, handler)
    waitforsocket(port)


def _handler_sends_chunked(_data):
    body_content = 'Hello, World!'
    return (
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        f"{len(body_content):x}\r\n{body_content}\r\n0\r\n\r\n"
    )


def _configure_proxy(port, label):
    assert 'success' in client.conf(
        {
            "settings": {"http": {"chunked_transform": True}},
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"proxy": f'http://127.0.0.1:{port}'}}],
        }
    ), label


# ---------------------------------------------------------------------------
# fake_upstream — live Rust HTTP mock (test/fake_upstream/)
# ---------------------------------------------------------------------------

FAKE_UPSTREAM_BIN = '/usr/local/bin/fake_upstream'

# Graceful skip when the Rust mock binary is not built (e.g. local runs
# without a Rust toolchain).  CI installs it via the "Build fake_upstream"
# step in ci.yml.
_skipif_no_fake_upstream = pytest.mark.skipif(
    not os.path.exists(FAKE_UPSTREAM_BIN),
    reason=f'{FAKE_UPSTREAM_BIN} not installed (build via test/fake_upstream)',
)


def _chunked_encode(data: bytes) -> bytes:
    """Encode bytes as a single-chunk chunked body."""
    return f'{len(data):x}\r\n'.encode() + data + b'\r\n0\r\n\r\n'


def _run_fake_upstream(mode: str, port: int):
    proc = subprocess.Popen(
        [FAKE_UPSTREAM_BIN, '--port', str(port), '--mode', mode],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    waitforsocket(port)
    return proc


def _fake_get(body: bytes, **kwargs):
    """Send HTTP/1.1 GET with Transfer-Encoding: chunked to FreeUnit proxy."""
    return client.get(
        headers={'Transfer-Encoding': 'chunked', 'Connection': 'close'},
        body=_chunked_encode(body).decode('latin-1'),
        **kwargs,
    )


@_skipif_no_fake_upstream
def test_proxy_chunked_to_content_length():
    """Chunked request → Content-Length for backends that require it. (Issue #1278)"""
    port = _get_free_port()
    proc = _run_fake_upstream('requires-cl', port)
    try:
        _configure_proxy(port, 'requires-cl backend')
        body = b'test'
        resp = _fake_get(body)
        assert resp['status'] == 200, 'backend must accept after chunked→CL conversion'
        assert resp['body'].encode('latin-1') == body, 'dechunked body must match exactly'
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_chunked_no_duplicate_transfer_encoding():
    """Proxied request must not carry Transfer-Encoding after conversion. (Issue #1088)"""
    port = _get_free_port()
    proc = _run_fake_upstream('no-te', port)
    try:
        _configure_proxy(port, 'no-te backend')
        body = b'data'
        resp = _fake_get(body)
        assert resp['status'] == 200, 'backend must not see Transfer-Encoding header'
        assert resp['body'].encode('latin-1') == body, 'dechunked body must match exactly'
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_chunked_cl_matches_body():
    """Content-Length value must equal actual dechunked body size. (Issue #445, #58)"""
    port = _get_free_port()
    proc = _run_fake_upstream('strict', port)
    try:
        _configure_proxy(port, 'strict backend')
        body = b'hello world'
        resp = _fake_get(body)
        assert resp['status'] == 200, 'strict backend: CL must equal dechunked body length'
        assert resp['body'].encode('latin-1') == body, 'dechunked body must match exactly'
    finally:
        proc.terminate()
        proc.wait()


def test_app_response_chunked_not_duplicated():
    """App's Transfer-Encoding: chunked response is not duplicated. (Issue #1088)"""
    port = _get_free_port()
    _run_proxy_server(_handler_sends_chunked, port)
    _configure_proxy(port, 'chunked-response backend')

    resp = get_http10()
    assert resp['status'] == 200, 'response should be OK'
    assert resp['body'] == 'Hello, World!', 'body must match exactly'


@_skipif_no_fake_upstream
def test_chunked_large_body():
    """Large chunked request (64 KB > body_buffer_size) → correct Content-Length. (Issue #445)"""
    port = _get_free_port()
    proc = _run_fake_upstream('strict', port)
    try:
        _configure_proxy(port, 'strict backend for large body')
        body = b'0123456789abcdef' * 4096  # 64 KB — forces file buffer path
        resp = _fake_get(body, read_buffer_size=65536 * 4)
        assert resp['status'] == 200, 'large chunked request must return 200'
        assert resp['body'].encode('latin-1') == body, 'dechunked 64 KB body must match exactly'
    finally:
        proc.terminate()
        proc.wait()


# ---------------------------------------------------------------------------
# Multi-chunk helpers
# ---------------------------------------------------------------------------

def _chunked_encode_multi(chunks: list) -> bytes:
    """Encode list of byte chunks as chunked body."""
    out = b''
    for c in chunks:
        out += f'{len(c):x}\r\n'.encode() + c + b'\r\n'
    out += b'0\r\n\r\n'
    return out


def _chunked_request(method='GET', body=b'', **kwargs):
    """Send HTTP/1.1 request with Transfer-Encoding: chunked."""
    return client.http(
        method,
        headers={'Transfer-Encoding': 'chunked', 'Connection': 'close'},
        body=body.decode('latin-1'),
        **kwargs,
    )


# ---------------------------------------------------------------------------
# Multi-chunk + memmove path
# ---------------------------------------------------------------------------

@_skipif_no_fake_upstream
def test_proxy_chunked_multi_chunk():
    """5+ chunks of varying size → correct Content-Length. (memmove path, Issue #445)"""
    port = _get_free_port()
    proc = _run_fake_upstream('strict', port)
    try:
        _configure_proxy(port, 'strict backend')
        chunks = [b'X', b'Y' * 17, b'Z' * 3, b'A' * 1024, b'B' * 5]
        expected = b''.join(chunks)
        encoded = _chunked_encode_multi(chunks)
        resp = _chunked_request(body=encoded)
        assert resp['status'] == 200, f'strict backend rejected: {resp["body"]}'
        assert resp['body'].encode('latin-1') == expected, 'multi-chunk body mismatch'
    finally:
        proc.terminate()
        proc.wait()


# ---------------------------------------------------------------------------
# Empty chunked body
# ---------------------------------------------------------------------------

@_skipif_no_fake_upstream
def test_proxy_chunked_empty_body():
    """Empty chunked body (0\\r\\n\\r\\n) → Content-Length: 0. (Issue #58)"""
    port = _get_free_port()
    proc = _run_fake_upstream('strict', port)
    try:
        _configure_proxy(port, 'strict backend')
        resp = _chunked_request(body=b'0\r\n\r\n')
        assert resp['status'] == 200, 'strict backend must accept empty body with CL:0'
        assert resp['body'] == '', 'empty chunked body must produce empty response body'
    finally:
        proc.terminate()
        proc.wait()


# ---------------------------------------------------------------------------
# Chunk extensions
# ---------------------------------------------------------------------------

@_skipif_no_fake_upstream
def test_proxy_chunked_extensions():
    """Chunk extensions (;ext=val) stripped during dechunk. (Issue #1278)"""
    port = _get_free_port()
    proc = _run_fake_upstream('strict', port)
    try:
        _configure_proxy(port, 'strict backend')
        # Single chunk with extension — FreeUnit must strip ;ext=val, body = "test"
        encoded = b'4;ext=val\r\ntest\r\n0\r\n\r\n'
        resp = _chunked_request(body=encoded)
        assert resp['status'] == 200, 'strict backend must accept after extension strip'
        assert resp['body'] == 'test', 'body must be "test" (extension stripped)'
    finally:
        proc.terminate()
        proc.wait()


# ---------------------------------------------------------------------------
# HTTP methods (POST/PUT/DELETE)
# ---------------------------------------------------------------------------

@_skipif_no_fake_upstream
def test_proxy_chunked_post():
    """POST with chunked body → Content-Length. (Issue #58)"""
    port = _get_free_port()
    proc = _run_fake_upstream('strict', port)
    try:
        _configure_proxy(port, 'strict backend')
        body = b'post-data'
        encoded = _chunked_encode(body)
        resp = _chunked_request('POST', encoded)
        assert resp['status'] == 200, 'POST: strict backend must accept'
        assert resp['body'].encode('latin-1') == body, 'POST body mismatch'
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_chunked_put():
    """PUT with chunked body → Content-Length. (Issue #58)"""
    port = _get_free_port()
    proc = _run_fake_upstream('strict', port)
    try:
        _configure_proxy(port, 'strict backend')
        body = b'put-payload'
        encoded = _chunked_encode(body)
        resp = _chunked_request('PUT', encoded)
        assert resp['status'] == 200, 'PUT: strict backend must accept'
        assert resp['body'].encode('latin-1') == body, 'PUT body mismatch'
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_chunked_delete():
    """DELETE with chunked body → Content-Length. (Issue #58)"""
    port = _get_free_port()
    proc = _run_fake_upstream('strict', port)
    try:
        _configure_proxy(port, 'strict backend')
        body = b'delete-body'
        encoded = _chunked_encode(body)
        resp = _chunked_request('DELETE', encoded)
        assert resp['status'] == 200, 'DELETE: strict backend must accept'
        assert resp['body'].encode('latin-1') == body, 'DELETE body mismatch'
    finally:
        proc.terminate()
        proc.wait()


# ---------------------------------------------------------------------------
# Concurrent chunked requests
# ---------------------------------------------------------------------------

@_skipif_no_fake_upstream
def test_proxy_chunked_concurrent():
    """10 concurrent chunked requests → all succeed. (race on buffer chain/mp_pool)"""
    import threading

    port = _get_free_port()
    proc = _run_fake_upstream('strict', port)
    try:
        _configure_proxy(port, 'strict backend')

        errors = []
        errors_lock = threading.Lock()

        def worker(body_bytes):
            try:
                encoded = _chunked_encode(body_bytes)
                resp = _chunked_request(body=encoded)
                if resp['status'] != 200:
                    with errors_lock:
                        errors.append(f'status={resp["status"]} body={resp["body"]!r}')
                elif resp['body'].encode('latin-1') != body_bytes:
                    with errors_lock:
                        errors.append(
                            f'body mismatch: expected {len(body_bytes)}, '
                            f'got {len(resp["body"])}'
                        )
            except Exception as e:
                with errors_lock:
                    errors.append(str(e))

        bodies = [f'chunk-{i}-{i * "x"}'.encode() for i in range(10)]
        threads = [threading.Thread(target=worker, args=(b,)) for b in bodies]

        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30)
            assert not t.is_alive(), f'thread hung: body_len={len(bodies[threads.index(t)])}'

        assert not errors, (
            f'{len(errors)}/10 concurrent requests failed: {errors[0]}'
        )
    finally:
        proc.terminate()
        proc.wait()
