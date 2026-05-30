import os
import socket
import subprocess

import pytest

from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# fake_otlp — std-only Rust mock OTLP/HTTP collector (test/fake_otlp/).
# CI installs it via the "Build fake_otlp" step in ci.yml, mirroring
# fake_upstream. Skip gracefully when it is not built.
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


def _run_fake_otlp(port, requests=None, dump=None):
    cmd = [FAKE_OTLP_BIN, '--port', str(port)]
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


def _valid_telemetry(collector_port, sampling_ratio=1.0, batch_size=1):
    return {
        "endpoint": f"http://127.0.0.1:{collector_port}",
        "protocol": "http",
        "sampling_ratio": sampling_ratio,
        "batch_size": batch_size,
    }


def _configure_or_skip(collector_port, sampling_ratio=1.0):
    """Apply a valid OTel + return-200 config.

    A rejection here is disambiguated: if the error names the "telemetry"
    object, unit was built without --otel -> skip. Any *other* rejection of a
    known-good config is a real regression and is surfaced (fail), never masked
    by a skip. This is what lets the negative validation tests trust that a
    rejection came from the field validator under test.
    """
    conf = client.conf(_config(_valid_telemetry(collector_port, sampling_ratio)))
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


@_skipif_no_fake_otlp
def test_otel_span_exported_with_service_name(tmp_path):
    """A traced request exports a span carrying service.name=FreeUnit."""
    port = _get_free_port()
    dump = str(tmp_path / 'otlp_dump.bin')
    proc = _run_fake_otlp(port, requests=1, dump=dump)
    try:
        _configure_or_skip(port)

        assert client.get()['status'] == 200

        try:
            proc.wait(timeout=EXPORT_TIMEOUT)
        except subprocess.TimeoutExpired:
            pytest.fail('fake_otlp did not receive an exported span')

        with open(dump, 'rb') as f:
            body = f.read()
        assert b'FreeUnit' in body, 'exported span must carry service.name=FreeUnit'
    finally:
        _kill(proc)


@_skipif_no_fake_otlp
def test_otel_traceparent_in_response():
    """FreeUnit injects a traceparent header into the response."""
    port = _get_free_port()
    proc = _run_fake_otlp(port)  # run forever, just absorb exports
    try:
        _configure_or_skip(port)

        resp = client.get()
        assert resp['status'] == 200
        assert 'traceparent' in _response_headers_lower(resp), (
            'response must carry a traceparent header'
        )
    finally:
        _kill(proc)


@_skipif_no_fake_otlp
def test_otel_traceparent_inherited(tmp_path):
    """An incoming traceparent is continued: the exported span keeps its trace id."""
    port = _get_free_port()
    dump = str(tmp_path / 'otlp_dump.bin')
    proc = _run_fake_otlp(port, requests=1, dump=dump)
    try:
        _configure_or_skip(port)

        resp = client.get(
            headers={
                'Host': 'localhost',
                'traceparent': f'00-{TRACE_ID}-{PARENT_ID}-01',
                'Connection': 'close',
            }
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
def test_otel_sampling_zero_exports_nothing():
    """sampling_ratio=0 → a new root trace is not sampled and never exported."""
    port = _get_free_port()
    proc = _run_fake_otlp(port, requests=1)
    try:
        _configure_or_skip(port, sampling_ratio=0.0)

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


def test_otel_protocol_grpc_rejected():
    """OTLP/gRPC was dropped in 1.35.6 — protocol "grpc" must be rejected."""
    _require_otel()
    tel = _valid_telemetry(1)
    tel["protocol"] = "grpc"
    conf = client.conf(_config(tel))
    assert 'error' in conf, 'protocol "grpc" must be rejected'
    assert 'protocol' in str(conf).lower()


def test_otel_protocol_invalid_rejected():
    """Only "http" is a valid protocol."""
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
