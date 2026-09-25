import subprocess
import time

import pytest
from unit.applications.lang.wasm_component import ApplicationWasmComponent
from unit.log import Log

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


def test_wasm_component_execution_timeout_invalid():
    # "execution_timeout" is seconds and reaches the module as a 32-bit
    # millisecond value, so both ends are bounded at validation.
    def conf_timeout(value):
        return client.conf(
            {
                "app": {
                    "type": "wasm-wasi-component",
                    "processes": {"spare": 0},
                    "component": "/app.wasm",
                    "execution_timeout": value,
                }
            },
            'applications',
        )

    resp = conf_timeout(-1)
    assert 'must not be negative' in resp.get('detail', ''), 'negative'

    resp = conf_timeout(2147483 + 1)
    assert 'must not exceed' in resp.get('detail', ''), 'too large'

    assert 'success' in conf_timeout(2147483), 'largest'
    assert 'success' in conf_timeout(0), 'unbounded'


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


def test_wasm_component_unrepresentable_request():
    client.load('hello_world')

    # Unit forwards bytes that the Rust "http" crate will not put in a URI:
    # obs-text in Host, and "<" or a control byte in the target.  Building the
    # request then fails, and that error must fail this request alone.  The
    # worker used to abort on it (SIGABRT), which conftest's Log.check_alerts()
    # would also catch on teardown.
    for req in (
        b'GET / HTTP/1.1\r\nHost: caf\xff\r\nConnection: close\r\n\r\n',
        b'GET /a<b HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n',
    ):
        assert client.http(req, raw=True)['status'] == 400, req

    # The worker is still alive and serving after both.
    assert client.get()['status'] == 200


def test_wasm_component_request_body():
    client.load('hello_world')

    # Exercises the request_read() loop.  A body larger than one read keeps
    # the loop going, and the component must still answer rather than spin or
    # read past the buffer.
    for size in (1, 8192, 128 * 1024):
        resp = client.post(body='x' * size)
        assert resp['status'] == 200, size
        assert resp['body'] == 'Hello', size


def test_wasm_component_execution_timeout():
    # "spin_loop" never yields and never answers.  A guest only yields at an
    # async wasi import, so without a deadline it stays inside a single
    # call_handle() poll forever, holding its store and a runtime thread.
    # "execution_timeout" arms the wasmtime epoch deadline that stops it.
    client.load('spin_loop', execution_timeout=1)

    assert client.get()['status'] == 500, 'preempted'

    # The worker survived the trap and serves the next request, which the
    # deadline stops the same way.  A trap is an ordinary Err out of
    # call_handle(), not a panic -- under `panic = 'abort'` a panic would
    # take the worker with it.
    #
    # Time this one, not the first: "spare" is 0, so the first request pays
    # for starting the process and compiling the component, which would let
    # a deadline that fired at 100 ms still clear the lower bound.
    start = time.time()
    assert client.get()['status'] == 500, 'worker alive'
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

    assert Log.findall(r'signal 6') == [], 'no abort'
    assert Log.findall(r'panicked at') == [], 'no panic'


def test_wasm_component_execution_timeout_body(skip_fds_check):
    # A request with a body still gets its full deadline.  The deadline is
    # armed inside the guest task rather than beside Store::new(), so the
    # body read no longer comes out of the guest's budget.
    #
    # This pins the property, not the regression: the smallest configurable
    # timeout is one second and the router hands the module a body that is
    # already complete, so reading a few megabytes costs milliseconds and
    # this passes under the old arming too.  Catching that directly would
    # need a body large enough to take about a second to read.
    skip_fds_check(router=True)

    client.load('spin_loop', execution_timeout=1)

    start = time.time()
    assert client.post(body='x' * (4 * 1024 * 1024))['status'] == 500, 'preempted'
    elapsed = time.time() - start

    # The guest still gets its full second after the body has been read.
    assert elapsed >= 1, elapsed

    assert Log.findall(r'signal 6') == [], 'no abort'



def _reap_spinning(app):
    """Kill a wasm worker that will not stop on its own.

    Without a deadline the guest never yields, so nothing in Unit can reclaim
    the worker: the router answers the client and unlinks the rpc data, but
    the process keeps running.  Reap it here so it does not outlive the test
    -- the same shape as test_app_start_timeout.py's _reap_stuck().
    """

    out = subprocess.run(
        ['ps', '-eo', 'pid,args'], capture_output=True, text=True, check=False
    ).stdout

    for line in out.splitlines():
        if f'unit: "{app}" application' in line:
            subprocess.run(
                ['kill', '-9', line.split()[0]], check=False
            )


def test_wasm_component_execution_timeout_absent(skip_alert, skip_fds_check):
    # The control for test_wasm_component_execution_timeout: the same guest
    # with no deadline is never stopped.  "limits": {"timeout"} bounds what
    # the CLIENT waits for -- the router answers 503 and unlinks the request
    # -- but it does not reach the application, so the worker spins on and
    # this test reaps it.
    #
    # That difference is the point: 503-from-the-router is not
    # 500-from-the-deadline, and "execution_timeout" is not a request timeout.
    skip_alert(r'exited on signal')
    skip_fds_check(router=True)

    try:
        client.load('spin_loop', limits={"timeout": 3})

        start = time.time()
        assert client.get(read_timeout=30)['status'] == 503, 'router gave up'
        elapsed = time.time() - start

        assert elapsed >= 3, elapsed

        assert Log.findall(r'wasm trap: interrupt') == [], 'no deadline fired'

    finally:
        _reap_spinning('spin_loop')


def test_wasm_component_execution_timeout_unreached():
    # A deadline a normal guest never approaches stays out of the way.  The
    # extra tick in the tick computation is there so a request is never cut
    # short of its timeout; this is the other half of that -- arming the
    # deadline must not cost a request that answers immediately.
    client.load('hello_world', execution_timeout=1)

    resp = client.get()

    assert resp['status'] == 200, 'answered'
    assert resp['body'] == 'Hello', 'body'

    # And again on a warm worker, where the store is armed per request.
    assert client.get()['status'] == 200, 'warm'
