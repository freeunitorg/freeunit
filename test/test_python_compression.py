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


@pytest.mark.parametrize('encoding', ENCODINGS)
def test_python_compression_unknown_length(encoding):
    # A streamed response with no Content-Length (#856).  The router used to
    # skip compression for an unknown length, so such a response, and any
    # PHP script that does not send Content-Length, was never compressed.
    # It is coded, sent chunked, and the last buffer ends the stream.
    setup_compression(encoding)

    status, headers, coded = fetch(
        3001, encoding, extra={'X-Content-Length': 'none'}
    )

    assert status == 200, 'status'
    assert headers.get('Content-Encoding') == encoding, 'coded'
    assert headers.get('Transfer-Encoding') == 'chunked', 'chunked'
    assert 'Content-Length' not in headers, 'no length'
    assert headers.get('Vary') == 'Accept-Encoding', 'vary'
    assert decoder(encoding)(dechunk(coded)) == expected(3001), 'decodes'


@pytest.mark.parametrize('encoding', ENCODINGS)
def test_python_compression_unknown_length_part_flushed(encoding):
    # With no Content-Length, each part must still reach the client while
    # the response streams: the stream is flushed after each part and ended
    # only by the last buffer.
    decode = stream_decoder(encoding)
    setup_compression(encoding)

    sock = client.get(
        headers={
            **request_headers(3002, encoding, parts=2, delay=3),
            'X-Content-Length': 'none',
        },
        no_recv=True,
    )
    sock.settimeout(30)

    data = b''
    while b'\r\n\r\n' not in data:
        part = sock.recv(4096)
        assert part, 'the response starts'
        data += part

    head, _, data = data.partition(b'\r\n\r\n')
    assert b'Content-Encoding: ' + encoding.encode() in head, 'coded'

    sock.settimeout(1)

    decoded = b''
    deadline = time.monotonic() + 2

    while time.monotonic() < deadline and len(decoded) < SIZE:
        while True:
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

    assert decoded == expected(3002, parts=1), 'the first part, decoded'


def test_python_compression_unknown_length_identity_refused():
    # With an unknown length, "identity;q=0" used to get the identity body
    # it refused, because negotiation was skipped.  It is negotiated now:
    # an acceptable coding is used, and none at all is a 406.
    setup_compression('gzip')

    status, headers, coded = fetch(
        3003, 'gzip, identity;q=0', extra={'X-Content-Length': 'none'}
    )

    assert status == 200, 'status'
    assert headers.get('Content-Encoding') == 'gzip', 'coded'
    assert gzip.decompress(dechunk(coded)) == expected(3003), 'decodes'

    status, _, _ = fetch(
        3004, 'identity;q=0', extra={'X-Content-Length': 'none'}
    )

    assert status == 406, 'nothing acceptable'


# "hold_timeout": a body of unknown length is held until it reaches the
# "min_length" of the selected coding, ends, or the timeout fires.  The
# header goes out only then, so "min_length" can apply to such a body.
HOLD_MIN = 1024


def setup_hold(hold_timeout, encoding='gzip', types=None, min_length=HOLD_MIN):
    client.load('compression_stream', threads=32)

    compression = {
        "types": types or ["text/plain"],
        "compressors": [{"encoding": encoding, "min_length": min_length}],
    }

    if hold_timeout is not None:
        compression["hold_timeout"] = hold_timeout

    resp = client.conf({"http": {"compression": compression}}, 'settings')

    if 'supported compressor' in resp.get('detail', ''):
        pytest.skip(f'unit built without {encoding} compression support')

    assert 'success' in resp, f'hold configured: {resp}'


def parts_body(rid, size, parts):
    token = b'<%06d>' % rid

    return (token * (size // len(token) + 1))[:size] * parts


def fetch_parts(rid, parts, size, delay, accept='gzip', extra=None):
    return fetch(
        rid,
        accept,
        extra={
            'X-Parts': str(parts),
            'X-Size': str(size),
            'X-Delay': str(delay),
            'X-Content-Length': 'none',
            **(extra or {}),
        },
    )


def read_head(sock):
    data = b''
    while b'\r\n\r\n' not in data:
        part = sock.recv(4096)
        assert part, 'the response starts'
        data += part

    return data.partition(b'\r\n\r\n')


def test_python_compression_hold_timeout_validation():
    client.load('compression_stream')

    for value in (-1, 2**31, 1.5, "100", True):
        assert 'error' in client.conf(
            {
                "http": {
                    "compression": {
                        "compressors": [{"encoding": "gzip"}],
                        "hold_timeout": value,
                    }
                }
            },
            'settings',
        ), f'{value!r} refused'

    setup_hold(0)


def test_python_compression_hold_small_unknown_length():
    # Five writes of 40 bytes and no Content-Length.  Below "min_length",
    # so the hold sees the end of the body and sends it as identity with
    # its length, instead of coding 200 bytes, chunked.
    setup_hold(1000)

    status, headers, body = fetch_parts(4001, 5, 40, 0.01)

    assert status == 200, 'status'
    assert 'Content-Encoding' not in headers, 'not coded'
    assert headers.get('Content-Length') == '200', 'the length, now known'
    assert 'Transfer-Encoding' not in headers, 'not chunked'
    assert headers.get('Vary') == 'Accept-Encoding', 'negotiated: vary'
    assert body == parts_body(4001, 40, 5), 'the body'


def test_python_compression_hold_large_unknown_length():
    # Reaches "min_length" in the first message: coded, as without a hold.
    setup_hold(1000)

    status, headers, coded = fetch_parts(4002, PARTS, SIZE, 0.01)

    assert status == 200, 'status'
    assert headers.get('Content-Encoding') == 'gzip', 'coded'
    assert headers.get('Transfer-Encoding') == 'chunked', 'chunked'
    assert 'Content-Length' not in headers, 'no length'
    assert gzip.decompress(dechunk(coded)) == expected(4002), 'decodes'


def test_python_compression_hold_reaches_min_length():
    # Five writes of 400 bytes: the hold ends with the third, at 1200 bytes,
    # and the rest streams after the held bytes.
    setup_hold(5000)

    status, headers, coded = fetch_parts(4003, 5, 400, 0.05)

    assert status == 200, 'status'
    assert headers.get('Content-Encoding') == 'gzip', 'coded'
    assert headers.get('Transfer-Encoding') == 'chunked', 'chunked'
    assert gzip.decompress(dechunk(coded)) == parts_body(4003, 400, 5), 'decodes'


def test_python_compression_hold_timer():
    # A slow stream below "min_length": the hold timer ends the hold, and
    # the first part reaches the client coded, long before the second part
    # is written.
    setup_hold(50)

    # The first request starts the application.
    fetch_parts(4004, 1, 40, 0)

    decode = stream_decoder('gzip')

    start = time.monotonic()

    sock = client.get(
        headers={
            **request_headers(4005, 'gzip', parts=3, delay=0.6),
            'X-Size': '40',
            'X-Content-Length': 'none',
        },
        no_recv=True,
    )
    sock.settimeout(5)

    head, _, data = read_head(sock)
    elapsed = time.monotonic() - start

    assert b'Content-Encoding: gzip' in head, 'coded'
    assert b'Transfer-Encoding: chunked' in head, 'chunked'
    assert elapsed < 0.4, f'held for {elapsed:.3f} s, before the second part'

    while True:
        end = data.find(b'\r\n')
        if end != -1:
            size = int(data[:end], 16)
            if len(data) >= end + 2 + size:
                break

        part = sock.recv(4096)
        assert part, 'the response is still streaming'
        data += part

    assert size != 0, 'the response is still streaming'
    assert (
        decode(data[end + 2 : end + 2 + size]) == parts_body(4005, 40, 1)
    ), 'the first part, decoded'
    assert time.monotonic() - start < 0.5, 'the first part before the second'

    sock.settimeout(10)

    while True:
        part = sock.recv(4096)
        if not part:
            break
        data += part

    sock.close()

    assert gzip.decompress(dechunk(data)) == parts_body(4005, 40, 3), 'decodes'


def head_time(sock):
    start = time.monotonic()
    head, _, _ = read_head(sock)
    return head, time.monotonic() - start


def test_python_compression_hold_exempt_event_stream():
    # An event stream is never held, even below "min_length".
    setup_hold(5000, types=["text/*"])

    fetch_parts(4006, 1, 40, 0)

    sock = client.get(
        headers={
            **request_headers(4007, 'gzip', parts=2, delay=2),
            'X-Size': '40',
            'X-Content-Length': 'none',
            'X-Content-Type': 'text/event-stream; charset=utf-8',
        },
        no_recv=True,
    )
    sock.settimeout(10)

    head, elapsed = head_time(sock)
    sock.close()

    assert b'Content-Encoding: gzip' in head, 'coded at once, as before'
    assert elapsed < 1.5, f'not held: {elapsed:.3f} s'

    # The same response as text/plain is held until it ends.
    sock = client.get(
        headers={
            **request_headers(4008, 'gzip', parts=2, delay=2),
            'X-Size': '40',
            'X-Content-Length': 'none',
        },
        no_recv=True,
    )
    sock.settimeout(10)

    head, elapsed = head_time(sock)
    sock.close()

    assert elapsed >= 1.5, f'held: {elapsed:.3f} s'
    assert b'Content-Length: 80' in head, 'identity, with its length'


def test_python_compression_hold_exempt_head():
    # A HEAD response has no body to wait for.
    setup_hold(5000)

    fetch_parts(4009, 1, 40, 0)

    sock = client.head(
        headers={
            **request_headers(4010, 'gzip', parts=2, delay=2),
            'X-Size': '40',
            'X-Content-Length': 'none',
        },
        no_recv=True,
    )
    sock.settimeout(10)

    head, elapsed = head_time(sock)
    sock.close()

    assert head.startswith(b'HTTP/1.1 200'), 'status'
    assert elapsed < 1.5, f'not held: {elapsed:.3f} s'


def test_python_compression_hold_identity_refused():
    # A held body whose end arrives below "min_length" is negotiated with
    # its length, as a response that declares it: a client that refuses
    # identity gets the 406 that a known length gets.  A body that reaches
    # the minimum is coded.
    setup_hold(1000)

    status, headers, _ = fetch_parts(4011, 5, 40, 0.01, 'gzip, identity;q=0')

    assert status == 406, 'gzip is below its minimum, identity refused'
    assert headers.get('Vary') == 'Accept-Encoding', 'the 406 varies'

    status, headers, coded = fetch_parts(
        4012, PARTS, SIZE, 0.01, 'gzip, identity;q=0'
    )

    assert status == 200, 'status'
    assert headers.get('Content-Encoding') == 'gzip', 'coded'
    assert gzip.decompress(dechunk(coded)) == expected(4012), 'decodes'

    status, _, _ = fetch_parts(4013, 5, 40, 0.01, 'identity;q=0')

    assert status == 406, 'nothing acceptable'


def test_python_compression_hold_absent():
    # Without "hold_timeout", "min_length" does not apply to an unknown
    # length, as before: the short body is coded.
    setup_hold(None)

    status, headers, coded = fetch_parts(4014, 5, 40, 0.01)

    assert status == 200, 'status'
    assert headers.get('Content-Encoding') == 'gzip', 'coded'
    assert gzip.decompress(dechunk(coded)) == parts_body(4014, 40, 5), 'decodes'


def test_python_compression_hold_many():
    # Many held responses at once, some committed by their end, some by
    # their size.  A write over 1024 bytes cannot fall back to a plain port
    # message, so it takes shared memory, one 16 KiB chunk here.  Each
    # application buffer is given back as soon as it is copied: these
    # responses take 7000 chunks, more than the 100 MB an application may
    # hold, so a leak would leave the last check unanswered.
    setup_hold(2000, min_length=65536)

    results = []

    def run(rid):
        parts, size = (8, 2000) if rid % 2 else (6, 16000)
        status, headers, body = fetch_parts(rid, parts, size, 0.01)

        if headers.get('Transfer-Encoding') == 'chunked':
            body = dechunk(body)

        if headers.get('Content-Encoding') == 'gzip':
            body = gzip.decompress(body)

        results.append(status == 200 and body == parts_body(rid, size, parts))

    for base in range(0, 1000, 40):
        threads = [
            threading.Thread(target=run, args=(5000 + base + i,))
            for i in range(40)
        ]

        for t in threads:
            t.start()

        for t in threads:
            t.join(timeout=60)

    assert len(results) == 1000 and all(results), 'every held response'

    status, headers, body = fetch_parts(4015, 8, 2000, 0.01)

    assert status == 200, 'still served'
    assert 'Content-Encoding' not in headers, 'below the minimum: identity'
    assert body == parts_body(4015, 2000, 8), 'the body'


def test_python_compression_hold_client_close():
    # Clients that close while their responses are held.  Nothing is sent
    # for them; the hold, its timer and the held bytes go with the request.
    setup_hold(1000)

    fetch_parts(4016, 1, 40, 0)

    socks = []
    for i in range(20):
        sock = client.get(
            headers={
                **request_headers(4100 + i, 'gzip', parts=4, delay=0.2),
                'X-Size': '40',
                'X-Content-Length': 'none',
            },
            no_recv=True,
        )
        socks.append(sock)

    time.sleep(0.1)

    for sock in socks:
        sock.setsockopt(
            socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)
        )
        sock.close()

    # Past the end of the applications' responses and the hold timeout.
    time.sleep(1.5)

    status, headers, body = fetch_parts(4017, 5, 40, 0.01)

    assert status == 200 and body == parts_body(4017, 40, 5), 'still served'


def test_python_compression_hold_app_timeout():
    # The application deadline expires while the response is held: the
    # header has not been sent, so the client gets a 503, and the hold timer
    # that fires later sends nothing.
    client.load('compression_stream', threads=32, limits={"timeout": 1})

    assert 'success' in client.conf(
        {
            "http": {
                "compression": {
                    "types": ["text/plain"],
                    "compressors": [
                        {"encoding": "gzip", "min_length": HOLD_MIN}
                    ],
                    "hold_timeout": 2000,
                }
            }
        },
        'settings',
    )

    status, _, _ = fetch_parts(4018, 2, 40, 1.5)

    assert status == 503, 'the deadline, while held'

    # The hold timer fires, then the application ends that response.
    time.sleep(2)

    status, headers, body = fetch_parts(4019, 5, 40, 0.01)

    assert status == 200 and body == parts_body(4019, 40, 5), 'still served'
