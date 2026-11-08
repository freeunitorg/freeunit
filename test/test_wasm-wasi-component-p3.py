"""WASI 0.3 (wasi:http@0.3.0) components.

The guest is built with rustc's wasm32-wasip2 target, not with
"cargo component", so this file does not need that feature.  The test
skips where the guest cannot be built (no wasm32-wasip2 target, or no
access to the crates it needs).
"""

import pytest
from unit.applications.lang.wasm_component import ApplicationWasmComponent

prerequisites = {'modules': {'wasm-wasi-component': 'any'}}

client = ApplicationWasmComponent()


@pytest.fixture(scope='module')
def p3_echo(tmp_path_factory):
    component = client.prepare_env_wasip2(
        'p3_echo', tmp_path_factory.mktemp('wasm_component')
    )

    if component is None:
        pytest.skip('cannot build the WASI 0.3 guest')

    return component


def test_wasm_component_p3_get(p3_echo):
    client.load('p3_echo', component=p3_echo)

    resp = client.get(
        url='/a/b?x=1',
        headers={'Host': 'localhost', 'X-Echo': 'hi', 'Connection': 'close'},
    )
    assert resp['status'] == 200, 'status'
    assert resp['headers']['x-wasi'] == '0.3', 'the 0.3 path ran'
    assert resp['headers']['x-method'] == 'GET', 'method'
    assert resp['headers']['x-path'] == '/a/b?x=1', 'path with query'
    assert resp['headers']['x-echo'] == 'hi', 'request header'
    assert resp['body'] == '', 'empty body'


def test_wasm_component_p3_post(p3_echo):
    client.load('p3_echo', component=p3_echo)

    body = 'hello p3 body ' * 1000

    resp = client.post(url='/post', body=body)
    assert resp['status'] == 200, 'status'
    assert resp['headers']['x-method'] == 'POST', 'method'
    assert resp['body'] == body, 'request body echoed'
