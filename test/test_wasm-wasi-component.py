import pytest
from unit.applications.lang.wasm_component import ApplicationWasmComponent

prerequisites = {
    'modules': {'wasm-wasi-component': 'any'},
    'features': {'cargo_component': True},
}

client = ApplicationWasmComponent()


def test_wasm_component():
    client.load('hello_world')

    req = client.get()

    assert client.get()['status'] == 200
    assert req['body'] == 'Hello'


def test_wasm_component_cstring_nul():
    # "component" is consumed as a NUL-terminated C-string path; an embedded
    # NUL (survives JSON parsing) or an empty value must be rejected at
    # validation.  "spare": 0 exercises validation without spawning.
    def conf_component(value):
        return client.conf(
            {
                "app": {
                    "type": "wasm-wasi-component",
                    "processes": {"spare": 0},
                    "component": value,
                }
            },
            'applications',
        )

    resp = conf_component("/x\0y.wasm")
    assert 'null character' in resp.get('detail', ''), 'nul'

    resp = conf_component("")
    assert 'must not be empty' in resp.get('detail', ''), 'empty'


def test_wasm_component_obs_text():
    client.load('hello_world')

    # Non-UTF-8 / obs-text bytes in target must be lossily replaced with U+FFFD
    # rather than panicking/aborting the wasm worker.
    resp = client.http(
        b'GET /caf\xff\xfe HTTP/1.1\r\n'
        b'Host: localhost\r\n'
        b'Connection: close\r\n\r\n',
        raw=True,
    )
    assert resp['status'] == 200
    assert resp['body'] == 'Hello'

    # Non-UTF-8 / obs-text bytes in headers must also not crash the wasm worker.
    resp = client.http(
        b'GET / HTTP/1.1\r\n'
        b'Host: localhost\r\n'
        b'Custom: caf\xff\xfe\r\n'
        b'Connection: close\r\n\r\n',
        raw=True,
    )
    assert resp['status'] == 200
    assert resp['body'] == 'Hello'
