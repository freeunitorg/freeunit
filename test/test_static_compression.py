from pathlib import Path

import pytest

from unit.applications.proto import ApplicationProto
from unit.option import option

client = ApplicationProto()


@pytest.fixture(autouse=True)
def requires_restart_mode():
    """
    Configuring compression is not reversible within one unitd (#167), so
    these tests need their own unitd or they crash whichever test runs next.
    Same reasoning as test_php_compression.py; drop both once #167 is fixed.
    """
    if not option.restart:
        pytest.skip('needs --restart until #167 is fixed')


@pytest.fixture(autouse=True)
def setup_method_fixture(temp_dir):
    assets_dir = f'{temp_dir}/assets'
    Path(assets_dir).mkdir(parents=True, exist_ok=True)
    Path(f'{assets_dir}/big.css').write_text(
        'body{color:red}' * 500, encoding='utf-8'
    )

    assert 'success' in client.conf(
        {
            "settings": {
                "http": {
                    "compression": {
                        "types": ["text/css"],
                        "compressors": [
                            {"encoding": "gzip", "level": 5, "min_length": 10}
                        ],
                    }
                }
            },
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"share": f'{assets_dir}$uri'}}],
        }
    ), 'compression configure'


def test_static_compression_baseline():
    resp = client.get(
        url='/big.css',
        headers={
            'Host': 'localhost',
            'Accept-Encoding': 'gzip',
            'Connection': 'close',
        },
    )
    assert resp['status'] == 200, 'compressed 200'
    assert resp['headers']['Content-Encoding'] == 'gzip', 'gzip applied'


def test_static_compression_precondition_does_not_mask_406():
    # RFC 9110 Sect. 13.2.1: an ordinary failure outranks a precondition.  A
    # client that refuses every encoding cannot be served at all, so the
    # answer is 406 -- a validator must not turn that into 304 or 412.
    etag = client.get(url='/big.css')['headers']['ETag']

    def get(**headers):
        return client.get(
            url='/big.css',
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                'Accept-Encoding': 'identity;q=0, *;q=0',
                **headers,
            },
        )

    assert get()['status'] == 406, 'unacceptable without a validator'
    assert get(**{'If-None-Match': '*'})['status'] == 406, '406 outranks 304'
    assert get(**{'If-None-Match': etag})['status'] == 406, '406 over 304'
    assert get(**{'If-Match': '"nope"'})['status'] == 406, '406 outranks 412'


def test_static_compression_304_carries_no_encoding():
    # A 304 sends no body, so it must not claim one is encoded, and the
    # compressor must not have been initialised for it either.
    etag = client.get(url='/big.css')['headers']['ETag']

    resp = client.get(
        url='/big.css',
        headers={
            'Host': 'localhost',
            'Connection': 'close',
            'Accept-Encoding': 'gzip',
            'If-None-Match': etag,
        },
    )

    assert resp['status'] == 304, 'not modified'
    assert resp['body'] == '', 'no body'
    assert 'Content-Encoding' not in resp['headers'], 'no Content-Encoding'
    assert 'Content-Length' not in resp['headers'], 'no Content-Length'
