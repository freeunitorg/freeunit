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


def _get_until_header(header, retries=50, delay=0.1, **kwargs):
    """Re-issue GET until `header` is present in the response, or retries run out.

    OTel (re)initialises asynchronously in the router *after* the control API
    has already accepted the telemetry config, so the very first request can
    race ahead of the tracer being ready: no span is created, hence no
    traceparent is injected. Poll briefly to absorb that init lag. A header that
    never appears still fails the caller's assertion, so a real regression is
    not masked. Extra kwargs are forwarded to `client.get` (e.g. headers).
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
