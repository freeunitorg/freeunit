from pathlib import Path

from unit.applications.lang.wasm import ApplicationWasm
from unit.check.check_prerequisites import check_prerequisites
from unit.option import option

prerequisites = {'modules': {'wasm': 'any'}, 'features': {'clang_wasm': True}}

check_prerequisites(prerequisites)

client = ApplicationWasm()


def test_wasm_hello():
    client.load('hello')

    resp = client.get()

    assert resp['status'] == 200
    assert resp['body'] == 'Hello from wasm\n'


def test_wasm_access_filesystem():
    data = Path(option.temp_dir) / 'wasm_data'
    data.mkdir()
    (data / 'hello.txt').write_text('from the preopen\n')

    client.load('hello', access_filesystem=[f'{data}/'])

    resp = client.get(url=f'{data}/hello.txt')

    assert resp['status'] == 200
    assert resp['body'] == 'from the preopen\n'


def test_wasm_access_filesystem_denied():
    data = Path(option.temp_dir) / 'wasm_data'
    data.mkdir()
    (data / 'inside.txt').write_text('inside the preopen\n')
    outside = Path(option.temp_dir) / 'outside.txt'
    outside.write_text('outside the preopen\n')

    # A symlink inside the preopen pointing outside it does not escape, with
    # or without the trailing slash RUSTSEC-2026-0269 abused.
    (data / 'link').symlink_to(outside)

    client.load('hello', access_filesystem=[f'{data}/'])

    # The guest answers 404 for any fopen() failure, so a broken preopen would
    # pass every assertion below.  Read a real file first.
    resp = client.get(url=f'{data}/inside.txt')

    assert resp['status'] == 200
    assert resp['body'] == 'inside the preopen\n'

    resp = client.get(url=str(outside))

    assert resp['status'] == 404

    for url in (f'{data}/link', f'{data}/link/'):
        resp = client.get(url=url)

        assert resp['status'] == 404, url
