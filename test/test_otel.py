import json
import os
import socket
import subprocess
import time

import pytest

from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# fake_otlp — Rust mock OTLP collector (test/fake_otlp/) speaking both OTLP/HTTP
# and OTLP/gRPC, so the transport under test is chosen by config (--protocol),
# never by how the mock was built. CI installs it via the "Build fake_otlp" step
# in ci.yml, mirroring fake_upstream. Skip gracefully when it is not built.
FAKE_OTLP_BIN = '/usr/local/bin/fake_otlp'

_skipif_no_fake_otlp = pytest.mark.skipif(
    not os.path.exists(FAKE_OTLP_BIN),
    reason=f'{FAKE_OTLP_BIN} not installed (build via test/fake_otlp)',
)

# The batch span processor flushes on its scheduled delay; give exports a
# generous window before declaring a span lost.
EXPORT_TIMEOUT = 20

# Window for the negative sampling test: long enough to exceed the flush
# interval, short enough to keep the suite snappy when nothing should arrive.
SAMPLING_CHECK_TIMEOUT = 10

# A 16-byte trace id / 8-byte span id used for the inheritance test.
TRACE_ID = '0af7651916cd43dd8448eb211c80319c'
PARENT_ID = 'b7ad6b7169203331'


def _get_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def _run_fake_otlp(port, requests=None, dump=None, protocol='http'):
    cmd = [FAKE_OTLP_BIN, '--port', str(port), '--protocol', protocol]
    if requests is not None:
        cmd += ['--requests', str(requests)]
    if dump is not None:
        cmd += ['--dump', dump]

    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    waitforsocket(port)
    return proc


def _kill(proc):
    if proc.poll() is None:
        proc.terminate()
    proc.wait()


def _config(telemetry):
    """A return-200 config skeleton wrapping the given telemetry block."""
    return {
        "settings": {"telemetry": telemetry},
        "listeners": {"*:8080": {"pass": "routes"}},
        "routes": [{"action": {"return": 200}}],
        "applications": {},
    }


def _valid_telemetry(
    collector_port, sampling_ratio=1.0, batch_size=1, protocol='http'
):
    # The OTLP SDK posts to the configured endpoint as-is (it does not append
    # the signal path), so an OTLP/HTTP endpoint must spell out /v1/traces to
    # reach a real collector — fake_otlp's hardened HTTP path rejects anything
    # else. OTLP/gRPC targets the host:port; the RPC method carries the path.
    if protocol == 'grpc':
        endpoint = f"http://127.0.0.1:{collector_port}"
    else:
        endpoint = f"http://127.0.0.1:{collector_port}/v1/traces"
    return {
        "endpoint": endpoint,
        "protocol": protocol,
        "sampling_ratio": sampling_ratio,
        "batch_size": batch_size,
    }


def _configure_or_skip(collector_port, sampling_ratio=1.0, protocol='http'):
    """Apply a valid OTel + return-200 config.

    A rejection here is disambiguated: if the error names the "telemetry"
    object, unit was built without --otel -> skip. Any *other* rejection of a
    known-good config is a real regression and is surfaced (fail), never masked
    by a skip. This is what lets the negative validation tests trust that a
    rejection came from the field validator under test.
    """
    conf = client.conf(
        _config(_valid_telemetry(collector_port, sampling_ratio, protocol=protocol))
    )
    if 'success' in conf:
        return
    if 'telemetry' in str(conf).lower():
        pytest.skip('unit built without --otel')
    pytest.fail(f'valid telemetry config rejected: {conf}')


def _require_otel():
    """Skip the test unless unit was built with --otel (known-good probe)."""
    _configure_or_skip(1)


def _response_headers_lower(resp):
    return {k.lower(): v for k, v in resp['headers'].items()}


def _get_until_header(header, retries=150, delay=0.1, **kwargs):
    """Re-issue GET until `header` is present in the response, or retries run out.

    OTel (re)initialises asynchronously in the router *after* the control API
    has already accepted the telemetry config, so the very first request can
    race ahead of the tracer being ready: no span is created, hence no
    traceparent is injected. Poll to absorb that init lag -- the gRPC exporter
    in particular can take several seconds to establish on a loaded CI runner,
    which the previous 5s budget occasionally missed. The loop returns the
    moment the header appears, so a larger cap costs nothing on the happy path;
    a header that never appears still fails the caller's assertion, so a real
    regression is not masked. Extra kwargs are forwarded to `client.get`.
    """
    resp = client.get(**kwargs)
    for _ in range(retries):
        if resp['status'] == 200 and header in _response_headers_lower(resp):
            return resp
        time.sleep(delay)
        resp = client.get(**kwargs)
    return resp


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_span_exported_with_service_name(tmp_path, protocol):
    """A traced request exports a span carrying service.name=FreeUnit."""
    port = _get_free_port()
    dump = str(tmp_path / 'otlp_dump.bin')
    proc = _run_fake_otlp(port, requests=1, dump=dump, protocol=protocol)
    try:
        _configure_or_skip(port, protocol=protocol)

        # OTel init in the router is async; poll until the tracer is ready
        # (traceparent present) so the captured span is fully attributed and
        # the first request doesn't race ahead of tracer setup.
        assert _get_until_header('traceparent')['status'] == 200

        try:
            proc.wait(timeout=EXPORT_TIMEOUT)
        except subprocess.TimeoutExpired:
            pytest.fail('fake_otlp did not receive an exported span')

        with open(dump, 'rb') as f:
            body = f.read()
        assert b'FreeUnit' in body, 'exported span must carry service.name=FreeUnit'
        # Semconv span attributes (1.35.6): recorded via nxt_otel_rs_add_attr,
        # not as the old free-form span events. The attribute *keys* travel as
        # literal strings in the OTLP protobuf payload.
        assert b'http.request.method' in body, 'span must carry semconv method attr'
        assert b'url.path' in body, 'span must carry semconv url.path attr'
        assert b'http.response.status_code' in body, 'span must carry status attr'
    finally:
        _kill(proc)


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_span_5xx_status(tmp_path, protocol):
    """A 5xx response is still traced and the span records the error status.

    The status_code attribute is emitted as a string (sprintf "%d"), so the
    literal `503` travels in the OTLP payload. Per the 1.35.6 fix, 503 >= 500
    also marks the span Status::Error (nxt_otel_rs_set_error); that enum is not
    asserted here (it is not a literal in the protobuf), but the status_code
    value is the concrete, reliable signal.
    """
    port = _get_free_port()
    dump = str(tmp_path / 'otlp_dump.bin')
    # Run forever and accumulate every export into the dump, so the readiness
    # phase (200 spans) does not consume a one-shot budget before the 503 span.
    proc = _run_fake_otlp(port, dump=dump, protocol=protocol)
    try:
        _configure_or_skip(port, protocol=protocol)

        # OTel inits asynchronously; wait until the tracer is ready on the
        # default 200 route (traceparent injected) before the error path.
        ready = _get_until_header('traceparent')
        assert (
            ready['status'] == 200
            and 'traceparent' in _response_headers_lower(ready)
        ), 'tracer did not become ready'

        assert 'success' in client.conf(
            '503', 'routes/0/action/return'
        ), 'switch route to return 503'

        # Fire 503s until the error span reaches the collector. status_code is
        # emitted as the string "503" (sprintf "%d"); per the 1.35.6 fix,
        # 503 >= 500 also marks the span Status::Error (not asserted here -- it
        # is a protobuf enum, not a literal). Only the 503 requests can add the
        # "503" bytes, so the readiness 200 spans don't false-positive.
        body = b''
        found = False
        for _ in range(int(EXPORT_TIMEOUT * 10)):
            resp = client.get()
            assert resp['status'] == 503, f'expected 503: {resp}'

            # The exporter may not have created the dump on the first pass.
            if os.path.exists(dump):
                with open(dump, 'rb') as f:
                    body = f.read()

                if b'503' in body:
                    found = True
                    break

            time.sleep(0.1)

        assert found, 'span with status_code=503 was not exported'
        assert b'http.response.status_code' in body, 'status_code attr missing'
    finally:
        _kill(proc)


@_skipif_no_fake_otlp
@pytest.mark.xfail(
    reason=(
        'Suspected bug: a malformed inbound traceparent returns HTTP 500 '
        'instead of being ignored. nxt_otel_parse_traceparent() returns '
        'NXT_ERROR on a length/format mismatch (src/nxt_otel.c ~445-476), and a '
        'header-parse callback returning NXT_ERROR fails the request. W3C '
        'Trace Context 3.2.2.3 requires an unparseable traceparent to be '
        'ignored and the trace restarted, not to reject the request -- so on '
        'an OTel-enabled listener any client can force a 500 with '
        '"traceparent: x". Deterministic, so strict=True: once #109 is fixed '
        'the test xpasses -> fails, forcing removal of this marker. '
        'See freeunitorg/freeunit#109.'
    ),
    strict=True,
)
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_traceparent_malformed(protocol):
    """A malformed inbound traceparent should be ignored (the trace is
    restarted), not turned into an error response."""
    port = _get_free_port()
    proc = _run_fake_otlp(port, protocol=protocol)  # run forever, absorb exports
    try:
        _configure_or_skip(port, protocol=protocol)

        resp = client.get(
            headers={
                'Host': 'localhost',
                'traceparent': 'this-is-not-a-valid-traceparent',
                'Connection': 'close',
            }
        )
        assert resp['status'] == 200, f'malformed traceparent must not error: {resp}'
    finally:
        _kill(proc)


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_traceparent_in_response(protocol):
    """FreeUnit injects a traceparent header into the response."""
    port = _get_free_port()
    proc = _run_fake_otlp(port, protocol=protocol)  # run forever, absorb exports
    try:
        _configure_or_skip(port, protocol=protocol)

        resp = _get_until_header('traceparent')
        assert resp['status'] == 200
        assert 'traceparent' in _response_headers_lower(resp), (
            'response must carry a traceparent header'
        )
    finally:
        _kill(proc)


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_traceparent_inherited(tmp_path, protocol):
    """An incoming traceparent is continued: the exported span keeps its trace id."""
    port = _get_free_port()
    dump = str(tmp_path / 'otlp_dump.bin')
    proc = _run_fake_otlp(port, requests=1, dump=dump, protocol=protocol)
    try:
        _configure_or_skip(port, protocol=protocol)

        resp = _get_until_header(
            'traceparent',
            headers={
                'Host': 'localhost',
                'traceparent': f'00-{TRACE_ID}-{PARENT_ID}-01',
                'Connection': 'close',
            },
        )
        assert resp['status'] == 200
        # FreeUnit echoes the inherited traceparent back in the response.
        assert TRACE_ID in _response_headers_lower(resp).get('traceparent', '')

        try:
            proc.wait(timeout=EXPORT_TIMEOUT)
        except subprocess.TimeoutExpired:
            pytest.fail('fake_otlp did not receive an exported span')

        with open(dump, 'rb') as f:
            body = f.read()
        # The trace id is encoded as 16 raw bytes in the OTLP protobuf payload.
        assert bytes.fromhex(TRACE_ID) in body, (
            'exported span must keep the inherited trace id'
        )
    finally:
        _kill(proc)


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_sampling_zero_exports_nothing(protocol):
    """sampling_ratio=0 → a new root trace is not sampled and never exported."""
    port = _get_free_port()
    proc = _run_fake_otlp(port, requests=1, protocol=protocol)
    try:
        _configure_or_skip(port, sampling_ratio=0.0, protocol=protocol)

        assert client.get()['status'] == 200

        try:
            # exceed the flush interval; nothing should ever arrive
            proc.wait(timeout=SAMPLING_CHECK_TIMEOUT)
            pytest.fail('fake_otlp received a span despite sampling_ratio=0')
        except subprocess.TimeoutExpired:
            pass  # expected — no export
    finally:
        _kill(proc)


# ---------------------------------------------------------------------------
# Config validation — guards the telemetry validators added in 1.35.6
# (src/nxt_conf_validation.c). These need no collector, so they carry no
# @_skipif_no_fake_otlp; they assert the control API accepts/rejects values
# and that a rejection comes from the validator under test (field name in the
# error), so a regression that loosens a bound can never pass silently.
# ---------------------------------------------------------------------------


def test_otel_protocol_grpc_accepted():
    """protocol "grpc" is a valid transport on any --otel build."""
    _require_otel()
    conf = client.conf(_config(_valid_telemetry(1, protocol='grpc')))
    assert 'success' in conf, f'protocol "grpc" must be accepted: {conf}'


def test_otel_protocol_invalid_rejected():
    """Only "http" and "grpc" are valid protocols."""
    _require_otel()
    tel = _valid_telemetry(1)
    tel["protocol"] = "https"
    conf = client.conf(_config(tel))
    assert 'error' in conf
    assert 'protocol' in str(conf).lower()


def test_otel_batch_size_zero_rejected():
    """batch_size must be greater than 0."""
    _require_otel()
    conf = client.conf(_config(_valid_telemetry(1, batch_size=0)))
    assert 'error' in conf
    assert 'batch_size' in str(conf).lower()


def test_otel_batch_size_too_large_rejected():
    """batch_size must not exceed 65536."""
    _require_otel()
    conf = client.conf(_config(_valid_telemetry(1, batch_size=65537)))
    assert 'error' in conf
    assert 'batch_size' in str(conf).lower()


def test_otel_batch_size_max_accepted():
    """batch_size at the 65536 upper bound is accepted."""
    _require_otel()
    conf = client.conf(_config(_valid_telemetry(1, batch_size=65536)))
    assert 'success' in conf, f'batch_size=65536 must be accepted: {conf}'


def test_otel_sampling_ratio_negative_rejected():
    """sampling_ratio below 0 must be rejected."""
    _require_otel()
    conf = client.conf(_config(_valid_telemetry(1, sampling_ratio=-0.1)))
    assert 'error' in conf
    assert 'sampling_ratio' in str(conf).lower()


def test_otel_sampling_ratio_above_one_rejected():
    """sampling_ratio above 1 must be rejected."""
    _require_otel()
    conf = client.conf(_config(_valid_telemetry(1, sampling_ratio=1.1)))
    assert 'error' in conf
    assert 'sampling_ratio' in str(conf).lower()


@pytest.mark.parametrize('ratio', [0.0, 1.0])
def test_otel_sampling_ratio_bounds_accepted(ratio):
    """sampling_ratio at the 0 and 1 boundaries is accepted."""
    _require_otel()
    conf = client.conf(_config(_valid_telemetry(1, sampling_ratio=ratio)))
    assert 'success' in conf, f'sampling_ratio={ratio} must be accepted: {conf}'


def test_otel_sampling_ratio_nonfinite_rejected():
    """A sampling_ratio that overflows to a non-finite double is rejected.

    Closest reachable proxy for the NaN-validator fix (33c7f0e0): strict JSON
    cannot express NaN, but a huge exponent (`1e400`) parses to +Inf, which
    must still land in the error path (> 1). Injected as a raw JSON number
    literal -- json.dumps() of a Python float would emit `Infinity`, testing
    the parser instead of the validator.
    """
    _require_otel()

    body = json.dumps(_config(_valid_telemetry(1)))
    body = body.replace('"sampling_ratio": 1.0', '"sampling_ratio": 1e400')
    assert '1e400' in body, 'sampling_ratio literal not injected'

    conf = client.conf(body)
    assert 'error' in conf, f'non-finite sampling_ratio must be rejected: {conf}'
