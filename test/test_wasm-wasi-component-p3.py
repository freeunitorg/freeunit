"""WASI 0.3 (wasi:http@0.3.0) components.

The guest is built with rustc's wasm32-wasip2 target, not with
"cargo component", so this file does not need that feature.  The test
skips where the guest cannot be built (no wasm32-wasip2 target, or no
access to the crates it needs).
"""

import time

import pytest
from unit.applications.lang.wasm_component import ApplicationWasmComponent
from unit.log import Log

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


@pytest.fixture(scope='module')
def p3_spin_loop(tmp_path_factory):
    component = client.prepare_env_wasip2(
        'p3_spin_loop', tmp_path_factory.mktemp('wasm_component')
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


def test_wasm_component_p3_execution_timeout(p3_spin_loop):
    # The WASI 0.3 handler runs inside Store::run_concurrent().  A guest that
    # never yields and never answers must be stopped by the same wasmtime
    # epoch deadline "execution_timeout" arms on the WASI 0.2 path, or the
    # store keeps the task running and the request never completes.
    client.load('p3_spin_loop', execution_timeout=1, component=p3_spin_loop)

    start = time.time()
    assert client.get()['status'] == 500, 'preempted'
    elapsed = time.time() - start

    # Not cut short of the configured timeout, and not far past it.  The
    # deadline is 1 s plus at most two 100 ms ticks; the rest of the margin
    # is the request itself.
    assert elapsed >= 1, elapsed
    assert elapsed < 5, elapsed

    # Assert the mechanism, not just the status: a guest that trapped for its
    # own reasons would answer 500 through the same path.  The epoch deadline
    # renders as an "interrupt" trap.
    assert Log.findall(r'wasm trap: interrupt') != [], 'stopped by the deadline'

    # The trap is an ordinary Err out of the store, not a panic or an abort:
    # under `panic = 'abort'` either would take the worker with it.
    assert Log.findall(r'signal 6') == [], 'no abort'
    assert Log.findall(r'panicked at') == [], 'no panic'

    # The worker survived the trap and serves the next request, which the
    # deadline stops the same way.
    assert client.get()['status'] == 500, 'worker alive'
