import json
import os
import re
import socket
import subprocess
import time

import pytest

from conftest import run_process
from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# fake_otlp — Rust mock OTLP collector (test/fake_otlp/) speaking both OTLP/HTTP
# and OTLP/gRPC, so the transport under test is chosen by config (--protocol),
# never by how the mock was built. CI installs it via the "Build fake_otlp" step
# in ci.yml, mirroring fake_upstream. Skip gracefully when it is not built.
# Overridable so the suite can be run against a locally built fake_otlp
# without root (installing into /usr/local/bin needs it).
FAKE_OTLP_BIN = os.environ.get('FAKE_OTLP_BIN', '/usr/local/bin/fake_otlp')

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


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_sampling_zero_still_propagates(protocol):
    """sampling_ratio=0 → nothing is exported, but context still propagates.

    The request path skips the attribute work for a span the sampler dropped
    (nxt_otel_span_add_headers, src/nxt_otel.c). Propagation sits outside that
    gate on purpose: W3C Trace Context requires the traceparent to reach the
    peer and the application whatever the sampling decision was, so a
    downstream service can join -- or knowingly decline to join -- the trace.
    """
    port = _get_free_port()
    proc = _run_fake_otlp(port, protocol=protocol)  # run forever, absorb exports
    try:
        _configure_or_skip(port, sampling_ratio=0.0, protocol=protocol)

        resp = _get_until_header('traceparent')
        assert resp['status'] == 200
        assert 'traceparent' in _response_headers_lower(resp), (
            'an unsampled request must still carry a traceparent header'
        )
    finally:
        _kill(proc)


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_traceparent_malformed(protocol):
    """A malformed inbound traceparent is ignored and the trace restarted.

    W3C Trace Context §3.2.2.3 requires an unparseable traceparent to be
    ignored and the trace restarted, never to reject the request. The header is
    client-supplied, so failing the request turns a bad header into a trivial
    availability attack (every request 500s). Regression guard for the fix in
    nxt_otel_parse_traceparent (src/nxt_otel.c): the request must be served
    (200) AND still traced with a fresh root span (a new traceparent is injected
    into the response), not served with telemetry silently disabled.
    """
    port = _get_free_port()
    proc = _run_fake_otlp(port, protocol=protocol)  # run forever, absorb exports
    try:
        _configure_or_skip(port, protocol=protocol)

        # Ensure the tracer is actually live before probing, so the malformed
        # header exercises the parse callback rather than being skipped while
        # OTel is still initialising.
        assert _get_until_header('traceparent')['status'] == 200

        # Wrong length, wrong shape, and content-invalid values (correct
        # 55-char shape but non-hex/uppercase segments, all-zero ids, the
        # forbidden "ff" version, or a misplaced hyphen) all take the
        # error_state path in the parser; each must be served 200 and get a
        # fresh, non-inherited traceparent in the response. The probe retries
        # via _get_until_header: OTel re-inits asynchronously on config change,
        # so a single-shot probe can race the tracer swap and go through
        # untraced. A regression is not masked — telemetry silently disabled
        # never yields a traceparent, so the poll drains and `injected` stays
        # empty.
        for bad in [
            'x',
            'this-is-not-a-valid-traceparent',
            '00-tooshort-01',
            f'zz-{TRACE_ID}-{PARENT_ID}-01',  # non-hex version
            f'00-{TRACE_ID.upper()}-{PARENT_ID}-01',  # uppercase: not HEXDIGLC
            f'00-{"0" * 32}-{PARENT_ID}-01',  # all-zero trace id
            f'00-{TRACE_ID}-{"0" * 16}-01',  # all-zero parent id
            f'ff-{TRACE_ID}-{PARENT_ID}-01',  # forbidden version
            f'00-{TRACE_ID[:-1]}-{PARENT_ID}-0-1',  # shifted hyphen, len 55
        ]:
            resp = _get_until_header(
                'traceparent',
                headers={
                    'Host': 'localhost',
                    'traceparent': bad,
                    'Connection': 'close',
                },
            )
            assert resp['status'] == 200, (
                f'malformed traceparent {bad!r} must be served, not 500'
            )
            injected = _response_headers_lower(resp).get('traceparent', '')
            assert injected, (
                f'malformed traceparent {bad!r} must restart the trace '
                f'(fresh traceparent injected), not disable telemetry'
            )
            assert not injected.startswith('00-00000000000000000000000000000000'), (
                f'restarted traceparent must carry a real trace id: {injected!r}'
            )
            assert TRACE_ID not in injected, (
                f'content-invalid traceparent {bad!r} must not be inherited'
            )

        # A well-formed traceparent is still inherited (control case): the
        # response echoes the incoming trace id. Retried like the probes above
        # (the async tracer swap can land at any point inside the test); an
        # inheritance regression still fails — a restarted trace echoes a
        # different id than TRACE_ID.
        resp = _get_until_header(
            'traceparent',
            headers={
                'Host': 'localhost',
                'traceparent': f'00-{TRACE_ID}-{PARENT_ID}-01',
                'Connection': 'close',
            },
        )
        assert resp['status'] == 200, 'valid traceparent must be served'
        assert TRACE_ID in _response_headers_lower(resp).get('traceparent', ''), (
            'valid traceparent must be inherited, not restarted'
        )
    finally:
        _kill(proc)


def _run_capture_server(server_port, capture_file):
    """A raw upstream that appends each request it receives to capture_file."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', server_port))
    sock.listen(5)

    resp = b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n'

    while True:
        conn, _ = sock.accept()

        # Read until end-of-headers: a short recv() is NOT end-of-message, and
        # the traceparent unit injects is appended last in the field list, so
        # a capture truncated at a segment boundary loses exactly that header.
        # All requests here are body-less GETs, so end-of-headers ends the
        # message.
        data = b''
        while b'\r\n\r\n' not in data:
            part = conn.recv(4096)
            if not part:
                break
            data += part

        with open(capture_file, 'ab') as f:
            f.write(data + b'\n===END===\n')

        conn.sendall(resp)
        conn.close()


def _configure_proxy_or_skip(collector_port, upstream_port, protocol='http'):
    """Apply a telemetry + proxy config, skipping cleanly on a non-otel build."""
    conf = client.conf(
        {
            "settings": {
                "telemetry": _valid_telemetry(collector_port, protocol=protocol)
            },
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {"action": {"proxy": f"http://127.0.0.1:{upstream_port}"}}
            ],
            "applications": {},
        }
    )
    if 'success' in conf:
        return
    if 'telemetry' in str(conf).lower():
        pytest.skip('unit built without --otel')
    pytest.fail(f'proxy + telemetry config rejected: {conf}')


def _forwarded_headers(capture_file, marker, name):
    """The values of header `name` in the captured proxied request with marker.

    The client.get that sends the probe only returns after the upstream has
    answered, so the forwarded request is already on disk; a short sleep guards
    the file append that races the socket write.
    """
    time.sleep(0.5)
    blocks = capture_file.read_bytes().decode(errors='replace').split('===END===')
    probe = [b for b in blocks if marker in b.lower()]
    assert probe, 'proxied request carrying the probe marker was not captured'
    return re.findall(rf'(?im)^{name}:[ \t]*(.+?)[ \t\r]*$', probe[-1])


def _forwarded_traceparents(capture_file, marker):
    return _forwarded_headers(capture_file, marker, 'traceparent')


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_traceparent_malformed_not_forwarded(tmp_path, protocol):
    """The bad traceparent is not forwarded alongside the restarted one.

    Restarting the trace makes nxt_otel_propagate_header() append a fresh
    traceparent to the proxied/app request. The original malformed header must
    be dropped (field->skip), otherwise a proxied peer or the application
    receives two traceparent headers — the bad one and the new one — and a
    downstream reader keying on the first is misled. Regression guard for the
    field->skip part of the fix in nxt_otel_parse_traceparent.
    """
    upstream_port = _get_free_port()
    capture = tmp_path / 'forwarded.txt'
    run_process(_run_capture_server, upstream_port, str(capture))
    waitforsocket(upstream_port)

    collector_port = _get_free_port()
    proc = _run_fake_otlp(collector_port, protocol=protocol)
    try:
        _configure_proxy_or_skip(collector_port, upstream_port, protocol)

        # Warm the tracer so the probe request actually restarts a trace.
        assert _get_until_header('traceparent')['status'] == 200

        # The probe goes through _get_until_header too: OTel re-inits
        # asynchronously on every config change, so a single-shot probe can
        # race the tracer swap and go through untraced (no injection at all).
        # A traced probe always carries a response traceparent, so polling for
        # it retries only raced attempts and cannot mask a regression.
        marker = 'otel-malformed-probe'
        resp = _get_until_header(
            'traceparent',
            headers={
                'Host': 'localhost',
                'traceparent': 'x',
                'X-Probe': marker,
                'Connection': 'close',
            },
        )
        assert resp['status'] == 200

        forwarded = _forwarded_traceparents(capture, marker)
        assert len(forwarded) == 1, (
            f'exactly one traceparent must be forwarded, got {forwarded}'
        )
        assert forwarded[0] != 'x', (
            f'the malformed traceparent must not be forwarded: {forwarded}'
        )
    finally:
        _kill(proc)


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
@pytest.mark.parametrize('order', ['valid_first', 'malformed_first'])
def test_otel_traceparent_duplicate_valid_preserved(tmp_path, protocol, order):
    """A valid traceparent is kept when a duplicate malformed one is present.

    Header fields are parsed in wire order, so a malformed duplicate must not
    discard context already accepted from a valid traceparent (in either
    order). The valid trace id is inherited (echoed in the response) and exactly
    that one traceparent is forwarded downstream — never the malformed
    duplicate, never a second restarted header.
    """
    valid = f'00-{TRACE_ID}-{PARENT_ID}-01'
    traceparent = [valid, 'x'] if order == 'valid_first' else ['x', valid]

    upstream_port = _get_free_port()
    capture = tmp_path / 'forwarded.txt'
    run_process(_run_capture_server, upstream_port, str(capture))
    waitforsocket(upstream_port)

    collector_port = _get_free_port()
    proc = _run_fake_otlp(collector_port, protocol=protocol)
    try:
        _configure_proxy_or_skip(collector_port, upstream_port, protocol)

        assert _get_until_header('traceparent')['status'] == 200

        # Retry the probe across the async tracer re-init (see the malformed
        # test above); an inheritance regression still fails: the traced
        # response would carry a restarted (wrong) trace id, not TRACE_ID.
        marker = 'otel-dup-probe'
        resp = _get_until_header(
            'traceparent',
            headers={
                'Host': 'localhost',
                'traceparent': traceparent,
                'X-Probe': marker,
                'Connection': 'close',
            },
        )
        assert resp['status'] == 200
        assert TRACE_ID in _response_headers_lower(resp).get('traceparent', ''), (
            'the valid duplicate must be inherited, not restarted'
        )

        forwarded = _forwarded_traceparents(capture, marker)
        assert len(forwarded) == 1, (
            f'exactly one traceparent must be forwarded, got {forwarded}'
        )
        assert TRACE_ID in forwarded[0], (
            f'the valid trace id must be the one forwarded: {forwarded}'
        )
    finally:
        _kill(proc)


TRACESTATE = 'congo=t61rcWkgMzE'


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_tracestate_dropped_on_restart(tmp_path, protocol):
    """Inbound tracestate is dropped when the trace is restarted.

    W3C Trace Context ties tracestate to the context in traceparent. With no
    valid traceparent accepted (malformed or absent), the trace restarts and
    the stale vendor state must not seed the new root span, be forwarded
    downstream, or be echoed back — only the fresh traceparent survives.
    """
    upstream_port = _get_free_port()
    capture = tmp_path / 'forwarded.txt'
    run_process(_run_capture_server, upstream_port, str(capture))
    waitforsocket(upstream_port)

    collector_port = _get_free_port()
    proc = _run_fake_otlp(collector_port, protocol=protocol)
    try:
        _configure_proxy_or_skip(collector_port, upstream_port, protocol)

        assert _get_until_header('traceparent')['status'] == 200

        scenarios = {
            'malformed': {'traceparent': 'x', 'tracestate': TRACESTATE},
            'absent': {'tracestate': TRACESTATE},
        }
        for scenario, headers in scenarios.items():
            marker = f'otel-ts-drop-{scenario}'
            resp = _get_until_header(
                'traceparent',
                headers={
                    'Host': 'localhost',
                    'X-Probe': marker,
                    'Connection': 'close',
                    **headers,
                },
            )
            assert resp['status'] == 200
            assert 'tracestate' not in _response_headers_lower(resp), (
                f'[{scenario}] rejected tracestate must not be echoed'
            )

            assert _forwarded_headers(capture, marker, 'tracestate') == [], (
                f'[{scenario}] rejected tracestate must not be forwarded'
            )
            forwarded = _forwarded_traceparents(capture, marker)
            assert len(forwarded) == 1 and forwarded[0] != 'x', (
                f'[{scenario}] one fresh traceparent expected: {forwarded}'
            )
    finally:
        _kill(proc)


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_tracestate_preserved_on_inherit(tmp_path, protocol):
    """Inbound tracestate rides along when the traceparent is inherited."""
    upstream_port = _get_free_port()
    capture = tmp_path / 'forwarded.txt'
    run_process(_run_capture_server, upstream_port, str(capture))
    waitforsocket(upstream_port)

    collector_port = _get_free_port()
    proc = _run_fake_otlp(collector_port, protocol=protocol)
    try:
        _configure_proxy_or_skip(collector_port, upstream_port, protocol)

        assert _get_until_header('traceparent')['status'] == 200

        marker = 'otel-ts-keep'
        resp = _get_until_header(
            'traceparent',
            headers={
                'Host': 'localhost',
                'traceparent': f'00-{TRACE_ID}-{PARENT_ID}-01',
                'tracestate': TRACESTATE,
                'X-Probe': marker,
                'Connection': 'close',
            },
        )
        assert resp['status'] == 200
        assert TRACE_ID in _response_headers_lower(resp).get('traceparent', '')
        assert TRACESTATE in _response_headers_lower(resp).get(
            'tracestate', ''
        ), 'tracestate must be echoed with an inherited traceparent'

        assert _forwarded_headers(capture, marker, 'tracestate') == [
            TRACESTATE
        ], 'tracestate must be forwarded with an inherited traceparent'
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


def _status_telemetry():
    """The /status "telemetry" object, or None when it is absent."""
    return client.conf_get('/status').get('telemetry')


def _wait_for_spans(field, timeout=EXPORT_TIMEOUT, delay=0.1):
    """Poll /status until telemetry/spans/<field> is non-zero; return the object."""
    deadline = time.time() + timeout
    telemetry = _status_telemetry()
    while time.time() < deadline:
        if telemetry is not None and telemetry['spans'][field] > 0:
            return telemetry
        time.sleep(delay)
        telemetry = _status_telemetry()
    return telemetry


def test_otel_status_absent_without_telemetry():
    """/status carries no "telemetry" object when telemetry is not configured.

    True of a build without --otel as well, which is why this one does not
    probe for OTel support first: the absence is the contract in both cases.
    """
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    )

    assert _status_telemetry() is None


@_skipif_no_fake_otlp
@pytest.mark.parametrize('protocol', ['http', 'grpc'])
def test_otel_status_counts_exported_spans(protocol):
    """A span that reaches the collector is counted in /status (issue #219)."""
    port = _get_free_port()
    proc = _run_fake_otlp(port, protocol=protocol)
    try:
        _configure_or_skip(port, protocol=protocol)

        assert _get_until_header('traceparent')['status'] == 200

        telemetry = _wait_for_spans('exported')
        assert telemetry is not None, '/status must report telemetry when configured'
        assert telemetry['spans']['exported'] > 0, 'exported spans must be counted'
        assert telemetry['spans']['failed'] == 0, 'a live collector must not fail'
    finally:
        _kill(proc)


def test_otel_status_counts_failed_spans():
    """An export the collector refuses is counted as failed, while the process
    is still running -- the whole point of #219: before this the only signal
    was a log line at exit.
    """
    # Nothing is listening here: the export is refused, not merely slow.
    port = _get_free_port()
    _configure_or_skip(port)

    assert _get_until_header('traceparent')['status'] == 200

    telemetry = _wait_for_spans('failed')
    assert telemetry is not None, '/status must report telemetry when configured'
    assert telemetry['spans']['failed'] > 0, 'a refused export must be counted'
    assert telemetry['spans']['exported'] == 0, 'nothing can have been exported'
