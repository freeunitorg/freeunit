import gzip
import hashlib
import socket

import pytest

from unit.applications.lang.php import ApplicationPHP

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()


def configure_compression(encoding='gzip'):
    conf = {
        "compression": {
            "types": ["text/html*"],
            "compressors": [{"encoding": encoding}],
        }
    }

    if encoding == 'gzip':
        conf["compression"]["compressors"][0]["level"] = 1

    resp = client.conf({"http": conf}, 'settings')

    if 'success' in resp:
        return

    # Compressors are build options; skip only for that, so a broken config
    # here fails loudly instead of quietly turning into a skipped test.
    if 'supported compressor' in resp.get('detail', ''):
        pytest.skip(f'unit built without {encoding} compression support')

    pytest.fail(f'could not configure compression: {resp}')


def dechunk(data):
    body = b''

    while True:
        end = data.find(b'\r\n')
        if end == -1:
            pytest.fail('truncated chunk header')

        try:
            size = int(data[:end], 16)
        except ValueError:
            pytest.fail(f'invalid chunk size {data[:end]!r}')

        if size == 0:
            return body

        body += data[end + 2 : end + 2 + size]
        data = data[end + 2 + size + 2 :]


def get_gzip(encoding='gzip'):
    """
    The shared client decodes bodies as text and splits chunked bodies on
    CRLF, both of which destroy a compressed payload, so read the response
    raw.  latin1 maps every byte 1:1, so encoding back recovers it exactly.
    """
    resp = client.get(
        headers={
            'Host': 'localhost',
            'Accept-Encoding': encoding,
            'Connection': 'close',
        },
        raw_resp=True,
        encoding='latin1',
        read_buffer_size=1024 * 1024,
    )

    head, _, body = resp.partition('\r\n\r\n')
    lines = head.split('\r\n')

    status = int(lines[0].split()[1])

    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(':')
        headers[name.strip()] = value.strip()

    data = body.encode('latin1')

    if headers.get('Transfer-Encoding') == 'chunked':
        data = dechunk(data)

    return status, headers, data


def test_php_compression_large_body():
    # The working reference case: a body large enough to travel through shared
    # memory reaches the compressor as a port-mmap buffer and is compressed.
    client.load('comp_large_body')
    configure_compression()

    status, headers, body = get_gzip()

    assert status == 200, 'status'
    assert headers['Content-Encoding'] == 'gzip', 'encoding advertised'
    assert body[:2] == b'\x1f\x8b', 'gzip magic'
    assert gzip.decompress(body) == b'A' * 100000, 'round-trips'


@pytest.mark.skip(
    reason='#162: small app responses are advertised gzip but sent '
    'uncompressed'
)
def test_php_compression_small_body():
    # Staged for issue #162.  nxt_http_comp_check_compression() commits to
    # compression and emits Content-Encoding before any body buffer exists,
    # but nxt_http_comp_compress_app_response() then declines the buffer:
    #
    #     if (!nxt_buf_is_port_mmap(*b)) { return NXT_OK; }
    #
    # A body small enough to arrive in an ordinary port message is plain
    # memory, so it goes out verbatim under the Content-Encoding already sent
    # and no client can decode it.
    #
    # Un-skip once #162 is fixed.  The assertion holds whichever way it is
    # fixed: either the body really is gzip, or the encoding was never
    # advertised.
    client.load('comp_small_body')
    configure_compression()

    status, headers, body = get_gzip()

    assert status == 200, 'status'

    if headers.get('Content-Encoding') == 'gzip':
        assert body[:2] == b'\x1f\x8b', 'advertised gzip must really be gzip'
        assert gzip.decompress(body) == b'A' * 64, 'round-trips'

    else:
        # Declining to compress is an equally valid fix, as long as the
        # encoding is not advertised.
        assert body == b'A' * 64, 'uncompressed body delivered verbatim'


RANDOM_BODY_SIZE = 9 * 1024 * 1024
LEAK_REQUESTS = 15  # > 100 MB in total: more shared memory than an app may hold


def raw_get(path, encoding, timeout=30):
    """
    A plain socket request.  The shared client cannot express "fail rather
    than block forever", which is exactly the failure these tests look for.
    """
    sock = socket.create_connection(('127.0.0.1', 8080), timeout)
    sock.settimeout(timeout)

    try:
        sock.sendall(
            f'GET {path} HTTP/1.1\r\n'
            f'Host: localhost\r\n'
            f'Accept-Encoding: {encoding}\r\n'
            f'Connection: close\r\n\r\n'.encode()
        )

        chunks = []
        while True:
            data = sock.recv(256 * 1024)
            if not data:
                break
            chunks.append(data)

    finally:
        sock.close()

    resp = b''.join(chunks)
    head, _, body = resp.partition(b'\r\n\r\n')
    lines = head.split(b'\r\n')

    status = int(lines[0].split()[1])

    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(b':')
        headers[name.strip().decode()] = value.strip().decode()

    if headers.get('Transfer-Encoding') == 'chunked':
        body = dechunk(body)

    return status, headers, body


def decompress(encoding, data):
    if encoding == 'gzip':
        return gzip.decompress(data)

    if encoding == 'br':
        brotli = pytest.importorskip(
            'brotli', reason='python brotli bindings not installed'
        )
        return brotli.decompress(data)

    if encoding == 'zstd':
        try:
            from compression import zstd  # Python >= 3.14

            return zstd.decompress(data)
        except ImportError:
            zstandard = pytest.importorskip(
                'zstandard', reason='python zstd bindings not installed'
            )
            return zstandard.ZstdDecompressor().decompressobj().decompress(data)

    pytest.fail(f'no decompressor for {encoding}')


@pytest.mark.parametrize('encoding', ['gzip', 'br', 'zstd'])
def test_php_compression_large_random_body(encoding):
    """
    Coverage for the chain walk in nxt_http_comp_compress_app_response():
    a multi-megabyte incompressible body arrives as a long sequence of
    port-mmap buffers, so every compressor is driven across many calls with
    the finishing one arriving last.  Check the body still round-trips.
    """
    client.load('comp_random_body')
    configure_compression(encoding)

    status, headers, body = raw_get(
        f'/?n={RANDOM_BODY_SIZE}', encoding
    )

    assert status == 200, 'status'
    assert headers['Content-Encoding'] == encoding, 'encoding advertised'

    # The compressed body is framed either by Content-Length or by chunked
    # transfer encoding -- never by both, and never by neither, or the client
    # cannot tell where it ends.
    has_clen = 'Content-Length' in headers
    is_chunked = headers.get('Transfer-Encoding') == 'chunked'

    assert has_clen != is_chunked, f'exactly one framing: {headers}'

    if has_clen:
        assert len(body) == int(headers['Content-Length']), 'framing consistent'

    plain = decompress(encoding, body)

    assert len(plain) == RANDOM_BODY_SIZE, 'full body length'
    assert (
        hashlib.sha256(plain).hexdigest() == headers['X-Body-Sha256']
    ), 'round-trips byte for byte'


def test_php_compression_shm_not_leaked():
    """
    Regression test for #177.

    nxt_http_comp_compress_app_response() released the application's buffer
    with nxt_buf_free(), a plain pool free that never runs the buffer's
    completion handler.  For a port-mmap buffer that handler,
    nxt_port_mmap_buf_completion(), is what marks the shared memory chunks
    free again, so every compressed response leaked its chunks.

    An application may hold only so much shared memory (shm_limit, 100 MB by
    default), so the leak is fatal rather than merely wasteful: after enough
    compressed bytes the worker can no longer allocate a buffer and the next
    request never gets a response.  Before the fix this wedges partway
    through the loop and the request below times out.
    """
    client.load('comp_random_body')
    configure_compression()

    for i in range(LEAK_REQUESTS):
        status, headers, body = raw_get(
            f'/?n={RANDOM_BODY_SIZE}', 'gzip'
        )

        assert status == 200, f'status of request {i}'
        assert headers['Content-Encoding'] == 'gzip', f'encoding, request {i}'
        assert (
            hashlib.sha256(gzip.decompress(body)).hexdigest()
            == headers['X-Body-Sha256']
        ), f'body of request {i}'


def test_php_compression_identity_refused():
    # An application response is negotiated like a static one.  The client
    # refuses identity and names no coding Unit can apply, so no acceptable
    # representation is left and the answer is 406.
    #
    # nxt_router_response_ready_handler() used to read every non-NXT_OK
    # result from nxt_http_comp_check_acceptable() as a server error and take
    # the generic "fail:" path, which answers 503 -- a negotiation failure
    # reported as the server being unable to serve anybody.
    client.load('comp_large_body')
    configure_compression()

    status, headers, _ = raw_get('/', 'identity;q=0')

    assert status == 406, 'no representation left for the application body'
    assert 'Content-Encoding' not in headers


def test_php_compression_wildcard_offers_a_coding():
    # The wildcard reaches the application path too.  Sect. 12.5.3: "*"
    # matches any available content coding not explicitly listed, so this
    # client refuses the response's own bytes and accepts everything else --
    # gzip included.  Reading "*" as identity alone left nothing acceptable
    # and the router answered 406 to a request it could serve.
    client.load('comp_large_body')
    configure_compression()

    status, headers, body = raw_get('/', 'identity;q=0, *;q=1')

    assert status == 200, 'the wildcard offers gzip'
    assert headers.get('Content-Encoding') == 'gzip', 'encoding advertised'
    assert gzip.decompress(body) == b'A' * 100000, 'round-trips'

    # A coding the field names keeps its own weight and is not matched by
    # the wildcard, so gzip is worth 0.1 here and is still what is sent.
    status, headers, body = raw_get('/', 'identity;q=0, *;q=0.5, gzip;q=0.1')

    assert status == 200, 'an explicitly named gzip is still acceptable'
    assert headers.get('Content-Encoding') == 'gzip', 'at its own weight'
    assert gzip.decompress(body) == b'A' * 100000


def test_php_compression_identity_refused_below_min_length():
    # gzip is acceptable but never applied: "min_length" is above this body,
    # so the response would fall back to the identity the client refused.
    # 406, and still not 503.
    client.load('comp_small_body')

    # client.load() installs a configuration without a "settings" object, so
    # a PUT to settings/http/compression is rejected.  The whole "settings"
    # object is put, and the result is checked: with the PUT silently failing
    # this test passed through the no-compressor path instead.
    assert 'success' in client.conf(
        {
            "http": {
                "compression": {
                    "types": ["text/html*"],
                    "compressors": [
                        {"encoding": "gzip", "min_length": 1000}
                    ],
                }
            }
        },
        'settings',
    ), 'gzip with min_length 1000'

    status, headers, _ = raw_get('/', 'gzip, identity;q=0')

    assert status == 406, 'a below-minimum gzip is not available'
    assert 'Content-Encoding' not in headers

    # The same response to a client that takes identity is unchanged.
    status, headers, body = raw_get('/', 'gzip')

    assert status == 200, 'identity is acceptable here'
    assert 'Content-Encoding' not in headers
    assert body == b'A' * 64, 'the body, uncompressed'
