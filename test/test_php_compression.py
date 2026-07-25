import gzip

import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.option import option

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()


@pytest.fixture(autouse=True)
def requires_restart_mode():
    """
    Configuring compression is not reversible within one unitd: the module
    keeps its state in process globals pointing into the router configuration
    that was current when they were parsed, and a later configuration without
    a compression block neither reinitialises nor clears them, so the next
    request dereferences a freed pool and the router dies (#167).

    Without --restart the whole session shares one unitd, so these tests would
    crash whichever test runs next.  With it, each test gets its own unitd and
    the state cannot escape.  Drop this fixture once #167 is fixed.
    """
    if not option.restart:
        pytest.skip('needs --restart until #167 is fixed')

COMPRESSION_CONF = {
    "compression": {
        "types": ["text/html*"],
        "compressors": [{"encoding": "gzip", "level": 1}],
    }
}


def configure_compression():
    resp = client.conf({"http": COMPRESSION_CONF}, 'settings')

    if 'success' in resp:
        return

    # Compressors are build options; skip only for that, so a broken config
    # here fails loudly instead of quietly turning into a skipped test.
    if 'supported compressor' in resp.get('detail', ''):
        pytest.skip('unit built without gzip compression support')

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


def get_gzip():
    """
    The shared client decodes bodies as text and splits chunked bodies on
    CRLF, both of which destroy a compressed payload, so read the response
    raw.  latin1 maps every byte 1:1, so encoding back recovers it exactly.
    """
    resp = client.get(
        headers={
            'Host': 'localhost',
            'Accept-Encoding': 'gzip',
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
