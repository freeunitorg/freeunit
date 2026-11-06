"""Static files over TLS with kernel TLS transmit.

With "Options": "KTLS" and kTLS TX active, the static handler sends the
body with SSL_sendfile().  Without it, the body is read into memory and
sent with SSL_write().  The responses must be identical in both cases.

The tests need the kernel "tls" module (/proc/net/tls_stat) and an OpenSSL
that enables kTLS.  The path checks also need a --debug build, because
SSL_sendfile() is logged at debug level.
"""

import gzip
import hashlib
import http.client
import os
import re
import ssl
from pathlib import Path

import pytest

from unit.applications.tls import ApplicationTLS
from unit.log import Log
from unit import port as port_map

prerequisites = {'modules': {'openssl': 'any'}}

client = ApplicationTLS()

KTLS_PORT = port_map.port(8080)
TLS_PORT = port_map.port(8081)

# The write limit of one nxt_conn_io_write() pass.
LIMIT = 10 * 1024 * 1024

FILES = {
    '1m': 1024 * 1024,
    '10m': LIMIT,
    '10m1': LIMIT + 1,
}

TLS_STAT = '/proc/net/tls_stat'


def tls_tx_sw():
    try:
        with open(TLS_STAT, encoding='utf-8') as f:
            for line in f:
                name, value = line.split()

                if name == 'TlsTxSw':
                    return int(value)

    except FileNotFoundError:
        return None

    return None


def age_file(path, seconds=2):
    # A file from the current second gets a weak ETag, and a later request
    # can get a strong one.  See age_file() in test_static.py.
    when = os.stat(path).st_mtime - seconds
    os.utime(path, (when, when))


def connect(port):
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE

    return http.client.HTTPSConnection(
        '127.0.0.1', port, context=context, timeout=60
    )


def request(conn, path, headers=None, method='GET'):
    conn.request(method, path, headers=headers or {})
    resp = conn.getresponse()
    body = resp.read()

    fields = {k.lower(): v for k, v in resp.getheaders()}

    return resp.status, fields, body


def log_from(start):
    with open(Log.get_path(), 'rb') as f:
        f.seek(start)
        return f.read().decode('utf-8', 'ignore')


def log_size():
    return os.path.getsize(Log.get_path())


@pytest.fixture(autouse=True)
def setup_method_fixture(temp_dir):
    if tls_tx_sw() is None:
        pytest.skip(f'kernel TLS is not available: no {TLS_STAT}')

    assets = f'{temp_dir}/assets'
    Path(assets).mkdir()

    for name, size in FILES.items():
        Path(f'{assets}/{name}').write_bytes(os.urandom(size))

    Path(f'{assets}/probe').write_bytes(b'x')

    for f in Path(assets).iterdir():
        age_file(f)

    client.certificate()

    conf = client.conf(
        {
            "listeners": {
                "*:8080": {
                    "pass": "routes",
                    "tls": {
                        "certificate": "default",
                        "conf_commands": {"Options": "KTLS"},
                    },
                },
                "*:8081": {
                    "pass": "routes",
                    "tls": {"certificate": "default"},
                },
            },
            "routes": [{"action": {"share": f'{assets}$uri'}}],
            "applications": {},
        }
    )

    if 'success' not in conf:
        pytest.skip(f'OpenSSL does not take "Options": "KTLS": {conf}')

    before = tls_tx_sw()

    conn = connect(KTLS_PORT)
    status, _, body = request(conn, '/probe')
    conn.close()

    assert status == 200 and body == b'x', 'probe'

    if tls_tx_sw() <= before:
        pytest.skip('kTLS TX did not activate: TlsTxSw did not grow')

    return assets


def test_tls_ktls_static_identical(setup_method_fixture):
    assets = setup_method_fixture

    for name, size in FILES.items():
        data = Path(f'{assets}/{name}').read_bytes()

        ktls = connect(KTLS_PORT)
        tls = connect(TLS_PORT)

        # Each list entry runs on the same two connections (keep-alive).

        cases = [
            ('GET', {}, 200, data),
            ('HEAD', {}, 200, b''),
            ('GET', {'Range': 'bytes=100-199'}, 206, data[100:200]),
            ('GET', {'Range': 'bytes=-1'}, 206, data[-1:]),
            # For "10m1" this body is exactly LIMIT bytes.
            ('GET', {'Range': 'bytes=1-'}, 206, data[1:]),
            # Unit ignores a multi-range request and sends the whole file.
            ('GET', {'Range': 'bytes=0-0,5-5'}, 200, data),
        ]

        for method, headers, want_status, want_body in cases:
            k_status, k_fields, k_body = request(
                ktls, f'/{name}', headers, method
            )
            t_status, t_fields, t_body = request(
                tls, f'/{name}', headers, method
            )

            what = f'{name} {method} {headers}'

            assert k_status == want_status, f'{what}: ktls status'
            assert t_status == want_status, f'{what}: tls status'

            assert (
                hashlib.sha256(k_body).hexdigest()
                == hashlib.sha256(want_body).hexdigest()
            ), f'{what}: ktls body'
            assert k_body == t_body, f'{what}: same body'

            want_length = len(want_body) if method == 'GET' else size

            assert k_fields['content-length'] == str(
                want_length
            ), f'{what}: Content-Length'

            for field in (
                'content-length',
                'etag',
                'last-modified',
                'content-range',
                'accept-ranges',
                'content-type',
            ):
                assert k_fields.get(field) == t_fields.get(
                    field
                ), f'{what}: same {field}'

        ktls.close()
        tls.close()


def test_tls_ktls_static_compressed(setup_method_fixture):
    # The compressed body is a temp file that replaces the shared one.
    assets = setup_method_fixture

    data = b'0123456789abcdef' * (LIMIT // 16 + 1)
    Path(f'{assets}/text.txt').write_bytes(data)
    age_file(f'{assets}/text.txt')

    resp = client.conf(
        {
            "http": {
                "compression": {
                    "types": ["text/plain"],
                    "compressors": [{"encoding": "gzip"}],
                }
            }
        },
        'settings',
    )

    if 'not a supported compressor' in str(resp):
        pytest.skip('unit built without gzip support')

    assert 'success' in resp, 'compression configure'

    debug = bool(Log.findall(r'\[debug\]'))
    start = log_size()

    headers = {'Accept-Encoding': 'gzip'}

    ktls = connect(KTLS_PORT)
    k_status, k_fields, k_body = request(ktls, '/text.txt', headers)
    ktls.close()

    log = log_from(start)

    tls = connect(TLS_PORT)
    t_status, t_fields, t_body = request(tls, '/text.txt', headers)
    tls.close()

    assert k_status == 200 and t_status == 200, 'status'
    assert k_fields.get('content-encoding') == 'gzip', 'Content-Encoding'
    assert gzip.decompress(k_body) == data, 'ktls body'
    assert k_body == t_body, 'same body'

    for field in ('content-length', 'content-encoding', 'etag', 'vary'):
        assert k_fields.get(field) == t_fields.get(field), f'same {field}'

    if debug:
        sent = sum(
            int(n)
            for n in re.findall(r'SSL_sendfile\([^)]*\): (\d+)', log)
        )

        assert sent == len(k_body), 'SSL_sendfile() sent the coded body'


def test_tls_ktls_static_sendfile(setup_method_fixture):
    assets = setup_method_fixture

    if not Log.findall(r'\[debug\]'):
        pytest.skip('needs a build configured with --debug')

    start = log_size()
    before = tls_tx_sw()

    conn = connect(KTLS_PORT)

    for name, size in FILES.items():
        status, _, body = request(conn, f'/{name}')

        assert status == 200, f'{name}: status'
        assert body == Path(f'{assets}/{name}').read_bytes(), f'{name}: body'

    conn.close()

    assert tls_tx_sw() > before, 'TlsTxSw grows: kTLS TX is active'

    log = log_from(start)

    calls = re.findall(
        r'SSL_sendfile\(\d+, \d+, @\d+, (\d+)\): (-?\d+)', log
    )

    sent = sum(int(n) for _, n in calls if int(n) > 0)

    assert sent == sum(FILES.values()), 'SSL_sendfile() sent every body byte'

    assert all(
        int(size) <= LIMIT for size, _ in calls
    ), 'SSL_sendfile() is bounded by the write limit'

    assert not re.search(r'pread\(', log), 'no pread() of the body'


def test_tls_ktls_static_tls_uses_read(setup_method_fixture):
    assets = setup_method_fixture

    if not Log.findall(r'\[debug\]'):
        pytest.skip('needs a build configured with --debug')

    start = log_size()

    conn = connect(TLS_PORT)
    status, _, body = request(conn, '/10m1')
    conn.close()

    assert status == 200, 'status'
    assert body == Path(f'{assets}/10m1').read_bytes(), 'body'

    log = log_from(start)

    assert not re.search(r'SSL_sendfile\(', log), 'no SSL_sendfile()'
    assert re.search(r'pread\(', log), 'pread() of the body'
    assert re.search(r'SSL_write\(', log), 'SSL_write() of the body'
