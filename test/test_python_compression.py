import gzip
import re
import socket
import struct
import threading
import time
import zlib

import pytest

from unit.applications.lang.python import ApplicationPython
from unit.compression import zstd_decompress, zstd_stream_decoder
from unit import port as port_map

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

ENCODINGS = ['gzip', 'deflate', 'zstd', 'br']

# Concurrent responses per test.  The bug needs two responses on one router
# thread, and the router has one thread per CPU.  On 8 CPUs, 24 responses
# made the test fail in each of 10 runs before the fix.
CONCURRENCY = 24
PARTS = 8
SIZE = 4096

TOKEN = re.compile(rb'<(\d{6})>')


def decoder(encoding):
    """Returns a function that decodes a whole body, or skips the test."""

    if encoding == 'gzip':
        return gzip.decompress

    if encoding == 'deflate':
        return zlib.decompress

    if encoding == 'zstd':
        return zstd_decompress

    return pytest.importorskip('brotli').decompress


def setup_compression(encoding, **kwargs):
    client.load('compression_stream', threads=32, **kwargs)

    resp = client.conf(
        {
            "http": {
                "compression": {
                    "types": ["text/plain"],
                    "compressors": [{"encoding": encoding}],
                }
            }
        },
        'settings',
    )

    if 'success' in resp:
        return

    if 'supported compressor' in resp.get('detail', ''):
        pytest.skip(f'unit built without {encoding} compression support')

    pytest.fail(f'could not configure compression: {resp}')


# The app sleeps "delay" seconds between writes, so the responses on one
# router thread take turns.  The stream tests depend on that: a machine that
# finishes a response in one turn can pass them without the fix.  Before the
# fix, 10 of 10 runs failed, with 21 to 24 of 24 bodies bad.
def request_headers(rid, encoding, parts, delay=0.02):
    return {
        'Host': 'localhost',
        'Accept-Encoding': encoding,
        'X-Id': str(rid),
        'X-Parts': str(parts),
        'X-Size': str(SIZE),
        'X-Delay': str(delay),
        'Connection': 'close',
    }


def expected(rid, parts=PARTS):
    token = b'<%06d>' % rid

    return (token * (SIZE // len(token) + 1))[:SIZE] * parts


def dechunk(data):
    body = b''

    while True:
        end = data.find(b'\r\n')
        if end == -1:
            raise ValueError('the response ends before its last chunk')

        size = int(data[:end], 16)
        if size == 0:
            return body

        chunk = data[end + 2 : end + 2 + size]
        if len(chunk) != size:
            raise ValueError('the response ends inside a chunk')

        body += chunk
        data = data[end + 2 + size + 2 :]


def fetch(rid, encoding, extra=None):
    # latin1 maps each byte to one character, so encoding back gives the
    # compressed bytes exactly.
    resp = client.get(
        headers={**request_headers(rid, encoding, PARTS), **(extra or {})},
        raw_resp=True,
        encoding='latin1',
        read_buffer_size=65536,
    )

    head, _, body = resp.partition('\r\n\r\n')
    lines = head.split('\r\n')

    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(':')
        headers[name.strip()] = value.strip()

    return int(lines[0].split()[1]), headers, body.encode('latin1')


def check(rid, encoding, decode, extra=None, want=None):
    """Returns None for a correct response, else what is wrong with it."""

    status, headers, body = fetch(rid, encoding, extra)

    if status != 200:
        return f'status {status}'

    for name, value in (want or {}).items():
        if headers.get(name) != value:
            return f'{name} is {str(headers.get(name))[:40]!r}'

    if headers.get('Content-Encoding') != encoding:
        return f'Content-Encoding {headers.get("Content-Encoding")}'

    # The length of the coded body is not known in advance, so the router
    # sends it chunked.
    try:
        if headers.get('Transfer-Encoding') == 'chunked':
            body = dechunk(body)

        data = decode(body)
    except Exception as e:  # pylint: disable=broad-except
        return f'not valid {encoding}: {e!r}'

    foreign = {int(m) for m in TOKEN.findall(data)} - {rid}
    if foreign:
        return f'holds bytes of requests {sorted(foreign)[:4]}'

    if data != expected(rid):
        return f'{len(data)} bytes, expected {len(expected(rid))}'

    return None


def burst(encoding, decode, first_rid):
    """Sends CONCURRENCY requests at once; returns the bad responses."""

    results = {}
    barrier = threading.Barrier(CONCURRENCY)

    def worker(rid):
        try:
            barrier.wait(timeout=30)
            results[rid] = check(rid, encoding, decode)
        except BaseException as e:  # pylint: disable=broad-except
            results[rid] = repr(e)

    threads = [
        threading.Thread(target=worker, args=(rid,))
        for rid in range(first_rid, first_rid + CONCURRENCY)
    ]

    for t in threads:
        t.start()

    for t in threads:
        t.join(timeout=120)

    assert len(results) == CONCURRENCY, 'every request finished'

    return {rid: why for rid, why in sorted(results.items()) if why}


def describe(bad):
    return f'{len(bad)} of {CONCURRENCY} bad, first: {list(bad.items())[:3]}'


def start_streams(encoding, parts, delay):
    """Starts 4 compressed responses and waits for the header of each."""

    socks = []

    for rid in range(1001, 1005):
        sock = client.get(
            headers=request_headers(rid, encoding, parts, delay),
            no_recv=True,
        )
        sock.settimeout(30)
        socks.append(sock)

    # The router initialises the stream before it sends the header.  The
    # empty line at the end of the header goes out with the first chunk, so
    # do not wait for it.
    coded = b'\r\nContent-Encoding: ' + encoding.encode() + b'\r\n'

    for sock in socks:
        data = b''
        while coded not in data:
            part = sock.recv(4096)
            assert part, 'the response starts'
            data += part

    return socks


def burst_with(encoding, decode, action):
    """Runs action() while a burst of responses streams."""

    result = {}

    def run_burst():
        result['bad'] = burst(encoding, decode, 1)

    t = threading.Thread(target=run_burst)
    t.start()

    # Let the responses of the burst start to stream.
    time.sleep(0.05)

    action()

    t.join(timeout=120)

    assert 'bad' in result, 'the burst finished'

    return result['bad']


@pytest.mark.parametrize('encoding', ENCODINGS)
def test_python_compression_concurrent_streams(encoding):
    # The compressor state was one object per router thread.  A response that
    # is compressed in several event loop turns shared it with each other
    # response on that thread.  A client then got invalid data, the bytes of
    # another response, or a router crash.
    decode = decoder(encoding)
    setup_compression(encoding)

    bad = burst(encoding, decode, 1)

    assert not bad, describe(bad)


def stream_decoder(encoding):
    """Returns a function that decodes the next bytes of a body, or skips."""

    if encoding == 'gzip':
        return zlib.decompressobj(16 + zlib.MAX_WBITS).decompress

    if encoding == 'deflate':
        return zlib.decompressobj().decompress

    if encoding == 'zstd':
        return zstd_stream_decoder()

    return pytest.importorskip('brotli').Decompressor().process


@pytest.mark.parametrize('encoding', ENCODINGS)
def test_python_compression_part_flushed(encoding):
    # Each part of a streamed response must reach the client compressed and
    # complete before the next part is written (#788).  zstd held its output
    # until a full 128 KiB block or the end of the response.
    decode = stream_decoder(encoding)
    setup_compression(encoding)

    sock = client.get(
        headers=request_headers(2001, encoding, parts=2, delay=3),
        no_recv=True,
    )
    sock.settimeout(30)

    data = b''
    while b'\r\n\r\n' not in data:
        part = sock.recv(4096)
        assert part, 'the response starts'
        data += part

    head, _, data = data.partition(b'\r\n\r\n')
    assert b'Transfer-Encoding: chunked' in head, 'chunked'

    sock.settimeout(1)

    # The second part comes 3 s after the first.  Read for 2 s and decode
    # each complete chunk: the first part must be in what arrived.
    decoded = b''
    deadline = time.monotonic() + 2

    while time.monotonic() < deadline:
        while True:
            # The CRLF after the data of a chunk can come with the next one.
            if data.startswith(b'\r\n'):
                data = data[2:]

            end = data.find(b'\r\n')
            if end == -1:
                break

            size = int(data[:end], 16)
            if len(data) < end + 2 + size:
                break

            assert size != 0, 'the response is still streaming'

            decoded += decode(data[end + 2 : end + 2 + size])
            data = data[end + 2 + size :]

        if len(decoded) >= SIZE:
            break

        try:
            part = sock.recv(4096)
        except socket.timeout:
            continue

        assert part, 'the response is still streaming'
        data += part

    sock.close()

    assert decoded == expected(2001, parts=1), 'the first part, decoded'


@pytest.mark.parametrize('encoding', ENCODINGS)
def test_python_compression_client_close(encoding):
    # Clients that close the connection in the middle of a compressed body,
    # while other compressed responses stream.  The router must free the
    # stream of each closed response (an ASan build reports a leak if it does
    # not), and must not free or use the stream of another response.
    decode = decoder(encoding)
    setup_compression(encoding)

    socks = start_streams(encoding, 40, 0.02)

    def reset():
        for sock in socks:
            sock.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)
            )
            sock.close()

    bad = burst_with(encoding, decode, reset)

    assert not bad, describe(bad)

    # The application ends the closed responses after 40 parts.  Wait for
    # that, then check that the router still serves a correct response.
    time.sleep(1)

    assert check(2001, encoding, decode) is None, 'response after the close'


@pytest.mark.parametrize('encoding', ENCODINGS)
def test_python_compression_app_timeout(encoding):
    # The application stops for longer than "limits": {"timeout"} in the
    # middle of a compressed body.  The router then closes the connection,
    # and must free the stream of the response.
    decode = decoder(encoding)
    setup_compression(encoding, limits={"timeout": 1})

    socks = start_streams(encoding, 2, 2)

    bad = burst_with(encoding, decode, lambda: None)

    assert not bad, describe(bad)

    for sock in socks:
        data = b''
        while True:
            part = sock.recv(65536)
            if not part:
                break
            data += part

        sock.close()

        assert not data.endswith(b'0\r\n\r\n'), 'the response is cut'

    # The application ends the slow responses 2 seconds after it starts
    # them.  Wait for that, then check a response again.
    time.sleep(1)

    assert check(2001, encoding, decode) is None, 'response after the timeout'


@pytest.mark.parametrize('encoding', ENCODINGS)
def test_python_compression_large_header(encoding):
    # libunit sends a response buffer over 1024 bytes (the status, the field
    # descriptors and the field bytes) to the router through shared memory.
    # The WSGI module sends the headers with the first body write, so libunit
    # puts the first body part into the same message, behind the fields.  The
    # router must compress that part like the parts after it, and end the
    # stream after the last part.  Before the fix, the first part went out
    # uncompressed, and the stream had no end.  The large field must come
    # back, which shows that the headers took the shared memory path.
    decode = decoder(encoding)
    setup_compression(encoding)

    bad = check(
        3001, encoding, decode, {'X-Pad': '2048'}, {'X-Pad': 'p' * 2048}
    )

    assert bad is None, bad


# Large enough to reach the router through shared memory, so the compressor
# takes it (see test_php_compression.py and issue 162 for small bodies).
BODY = gzip.compress(b'A' * 100000, compresslevel=0)


def post(body, url='/', accept='gzip', timeout=30):
    # The "variables" application echoes the request body with the request's
    # Content-Type.  It needs a Custom-Header to build its response.  Raw
    # socket: the response body must not be decoded.
    sock = socket.create_connection(
        ('127.0.0.1', port_map.port(8080)), timeout
    )
    sock.settimeout(timeout)

    try:
        sock.sendall(
            f'POST {url} HTTP/1.1\r\n'
            f'Host: localhost\r\n'
            f'Content-Type: text/plain\r\n'
            f'Custom-Header: 557\r\n'
            f'Content-Length: {len(body)}\r\n'
            f'Accept-Encoding: {accept}\r\n'
            f'Connection: close\r\n\r\n'.encode()
            + body
        )

        chunks = []
        while True:
            data = sock.recv(256 * 1024)
            if not data:
                break
            chunks.append(data)

    finally:
        sock.close()

    head, _, data = b''.join(chunks).partition(b'\r\n\r\n')
    lines = head.split(b'\r\n')

    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(b':')
        headers[name.strip().decode()] = value.strip().decode()

    if headers.get('Transfer-Encoding') == 'chunked':
        data = dechunk(data)

    return int(lines[0].split()[1]), headers, data


def configure(response_headers, app='variables'):
    client.load(app)

    action = {"pass": f"applications/{app}"}

    if response_headers is not None:
        action["response_headers"] = response_headers

    assert 'success' in client.conf(
        {
            "http": {
                "compression": {
                    "compressors": [{"encoding": "gzip", "level": 1}]
                }
            }
        },
        'settings',
    ), 'compression'
    assert 'success' in client.conf([{"action": action}], 'routes'), 'routes'
    assert 'success' in client.conf(
        {"*:8080": {"pass": "routes"}}, 'listeners'
    ), 'listeners'


def test_python_compression_control():
    # Without a configured Content-Encoding, the echoed body is compressed.
    # So the test below passes because of the configured field.
    configure(None)

    status, headers, body = post(BODY)

    assert status == 200, 'status'
    assert headers.get('Content-Encoding') == 'gzip', 'compressed'
    assert gzip.decompress(body) == BODY, 'gzip of the echoed bytes'


def test_python_compression_response_headers_content_encoding():
    # https://github.com/freeunitorg/freeunit/issues/557, application path.
    # The application sends bytes that are already gzip, and the action
    # names their coding in "response_headers".  The compressor must not
    # code them again: that Content-Encoding replaces the compressor's.
    configure({"Content-Encoding": "gzip"})

    status, headers, body = post(BODY)

    assert status == 200, 'status'
    assert headers.get('Content-Encoding') == 'gzip', 'the configured coding'
    assert len(body) == len(BODY), 'the application length'
    assert body == BODY, 'the application bytes, coded once'


def test_python_compression_response_headers_content_encoding_template():
    # A template value that is not safe in a field is never sent.  So it
    # must not keep the compressor out.  Otherwise the body goes out with no
    # Content-Encoding to a client that refused identity.
    configure({"Content-Encoding": "$arg_c"})

    status, headers, body = post(BODY, url='/?c=gzip')

    assert status == 200, 'status'
    assert headers.get('Content-Encoding') == 'gzip', 'the configured coding'
    assert body == BODY, 'the application bytes, coded once'

    status, headers, body = post(
        BODY, url='/?c=gzip%0d%0a', accept='gzip, identity;q=0'
    )

    assert status == 200, 'unsafe value: status'
    assert headers.get('Content-Encoding') == 'gzip', 'the compressor coding'
    assert gzip.decompress(body) == BODY, 'gzip of the echoed bytes'


def test_python_compression_app_coding_removed_is_not_coded_again():
    # The application codes its own body and sends its own Content-Encoding.
    # A null Content-Encoding in "response_headers" then removes only the
    # field.  The removal rule is for a coding that Unit would apply, so it
    # does not apply here: the bytes are not coded again, and a client that
    # refused identity gets no 406.
    configure({"Content-Encoding": None}, app='content_encoding')

    for accept in ('gzip', 'gzip, identity;q=0'):
        status, headers, body = post(BODY, accept=accept)

        assert status == 200, f'{accept}: status'
        assert 'Content-Encoding' not in headers, f'{accept}: removed'
        assert body == BODY, f'{accept}: the application bytes'


def test_python_compression_empty_content_length():
    # An application response with "Content-Length: 0" carries the length in
    # the field, not in content_length_n.  Before the fix it was coded: the
    # field was dropped for an empty chunked body with Content-Encoding, and
    # "identity;q=0" was answered with 406.  An empty body has no bytes to
    # code or to refuse.
    configure(None)

    for accept in ('gzip', 'gzip, identity;q=0', 'identity;q=0'):
        status, headers, body = post(b'', accept=accept)

        assert status == 200, f'{accept}: status'
        assert 'Content-Encoding' not in headers, f'{accept}: not coded'
        assert headers.get('Content-Length') == '0', f'{accept}: length kept'
        assert body == b'', f'{accept}: empty body'


@pytest.mark.parametrize('body', ['hello', ''])
@pytest.mark.parametrize('encoding', ENCODINGS)
def test_python_compression_short_body(encoding, body):
    # The application declares "Content-Length: 10" and sends fewer bytes, or
    # none.  No data buffer completes the declared length, so the stream is
    # ended at the last buffer.  Before the fix the coded body ended without
    # the stream's trailer, and a client could not decode it.
    setup_compression(encoding)

    status, headers, coded = fetch(
        1,
        encoding,
        extra={
            'X-Parts': '1',
            'X-Delay': '0',
            'X-Body': body,
            'X-Content-Length': '10',
        },
    )

    assert status == 200, 'status'
    assert headers.get('Content-Encoding') == encoding, 'coded'

    coded = dechunk(coded)

    assert coded != b'', 'a coded body is never empty'
    assert decoder(encoding)(coded) == body.encode(), 'complete stream'
