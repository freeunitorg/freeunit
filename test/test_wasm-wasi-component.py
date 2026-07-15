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
