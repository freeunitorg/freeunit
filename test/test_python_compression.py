import gzip
import re
import socket
import struct
import threading
import time
import zlib

import pytest

from unit.applications.lang.python import ApplicationPython

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
        try:
            from compression import zstd  # Python 3.14 and later.

            return zstd.decompress
        except ImportError:
            pass

        zstandard = pytest.importorskip('zstandard')

        def decode(data):
            d = zstandard.ZstdDecompressor().decompressobj()
            out = d.decompress(data)

            if not d.eof:
                raise ValueError('zstd frame is not complete')

            return out

        return decode

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


def fetch(rid, encoding):
    # latin1 maps each byte to one character, so encoding back gives the
    # compressed bytes exactly.
    resp = client.get(
        headers=request_headers(rid, encoding, PARTS),
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


def check(rid, encoding, decode):
    """Returns None for a correct response, else what is wrong with it."""

    status, headers, body = fetch(rid, encoding)

    if status != 200:
        return f'status {status}'

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
    # empty line at the end of the header goes out with the first chunk,
    # which for zstd can be the last one, so do not wait for it.
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
    # and must free the stream of the response.  A client close does not do
    # this for zstd: the router writes no zstd data until the last part, so
    # it does not see the close before the stream ends.
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
