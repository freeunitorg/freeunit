# fake_otlp

A tiny **mock OTLP collector** for FreeUnit OpenTelemetry tests. FreeUnit
**exports** spans to it; the test drives FreeUnit and asserts what the
**collector** received (dumped request bytes). One build speaks both OTLP
transports; each instance serves one at a time (`--protocol`), so a test can pin
an exact export contract.

See [PLAN.md](PLAN.md) for the case backlog and the OTLP/gRPC re-introduction
plan.

## Build

```bash
cargo build --release --manifest-path test/fake_otlp/Cargo.toml
cp test/fake_otlp/target/release/fake_otlp /usr/local/bin/
```

CI builds it in the "Build fake_otlp" step; the pytest cases skip gracefully when
`/usr/local/bin/fake_otlp` is absent (`@_skipif_no_fake_otlp`).

Both transports are compiled in (HTTP handled std-only, gRPC via tonic/prost),
so the protocol under test is chosen by `--protocol`, never by how the mock was
built — mirroring FreeUnit's single `--otel` build. See PLAN.md.

## CLI

```
fake_otlp --port <N> [--protocol http|grpc] [--requests <N>] [--dump <FILE>]
```

| Flag | Meaning |
|------|---------|
| `--port N` | TCP port to listen on (`127.0.0.1`). Required. |
| `--protocol P` | transport (table below); default `http` |
| `--requests N` | exit after N export requests (default: run forever) |
| `--dump FILE` | append each received request (raw bytes) to FILE |

### Protocols

| Protocol | Behavior | Rust handler |
|----------|----------|--------------|
| `http` | accepts `POST /v1/traces`, reads `Content-Length` body, replies `200 OK` empty (a valid empty `ExportTraceServiceResponse`). std-only. | `serve_http` |
| `grpc` | accepts the unary `TraceService/Export` over HTTP/2, replies an empty `ExportTraceServiceResponse` (tonic/prost). | `serve_grpc` |

Both honour `--requests`/`--dump` identically and print
`span_received content_length=<N> content_type=<CT>` per export. A bare TCP
readiness probe (connect + immediate close) is **not** counted and **not**
dumped.

### Asserting on a span without a protobuf parser

The service name, resource attributes and span attributes are encoded in the
OTLP body as length-delimited UTF-8, and the trace id as 16 raw bytes. Tests
assert with a plain **byte-substring search** over the dump — no decoder needed:

- `b'FreeUnit' in body` → span carries `service.name=FreeUnit`
- `bytes.fromhex(TRACE_ID) in body` → inherited trace id was kept

For `grpc`, the dump is the decoded `ExportTraceServiceRequest` bytes, so the
same substring assertions hold across both transports.

## Test convention: one reserved port per test + name mirroring

Each instance serves one transport, so cases coexist on **distinct ports** and
FreeUnit's telemetry endpoint points at whichever a case configured. Two rules
keep the suite collision-free and greppable (same discipline as
`test/fake_upstream/`):

**1. One fixed, documented port per test.** A collector-using test does not grab
an ephemeral port — it owns a reserved number from the registry below and
launches its own instance on it (torn down in `finally`). Fixed numbers make a
run reproducible by hand (a real OTel SDK / curl hits the same port the test
used) and the registry is the single place that guarantees no two cases clash.

**2. Mirror the names.** A case carries one shared token across three layers —
pytest function, `--protocol`/scenario, and Rust handler — so a single `grep`
finds every side. E.g. token `grpc_span_exported`:
`test_otel_grpc_span_exported` ↔ `--protocol grpc` ↔ `serve_grpc`.

### Port registry

| Port | Token | Protocol (CLI) | Rust handler | pytest |
|------|-------|----------------|--------------|--------|
| 7970 | `span_exported` | `http` | `serve_http` | `test_otel_span_exported_with_service_name` |
| 7971 | `traceparent_response` | `http` | `serve_http` | `test_otel_traceparent_in_response` |
| 7972 | `traceparent_inherited` | `http` | `serve_http` | `test_otel_traceparent_inherited` |
| 7973 | `sampling_zero` | `http` | `serve_http` | `test_otel_sampling_zero_exports_nothing` |
| 7980 | `grpc_span_exported` | `grpc` | `serve_grpc` | `test_otel_span_exported_with_service_name[grpc]` ✅ |
| 7981 | `grpc_traceparent_inherited` | `grpc` | `serve_grpc` | `test_otel_traceparent_inherited[grpc]` ✅ |
| 7982 | `grpc_sampling_zero` | `grpc` | `serve_grpc` | `test_otel_sampling_zero_exports_nothing[grpc]` ✅ |

✅ = implemented · 🔲 = to add (tracked in PLAN.md). The gRPC cases are not
separate functions: they are the `[grpc]` parametrization of the HTTP tests
(same body, `protocol="grpc"`). HTTP ports use the `79xx` band like
`fake_upstream` (which owns `7990-7998`); gRPC cases use `798x`.

Config-validation tests (`test_otel_protocol_*`, `test_otel_batch_size_*`,
`test_otel_sampling_ratio_*`) need **no** collector and carry no port — they only
assert the control API accepts/rejects telemetry values.

A test pins its port as a module constant referencing this table:

```python
OTLP_PORT = 7970                           # reserved here for service-name export
proc = _run_fake_otlp(OTLP_PORT, requests=1, dump=dump)
try:
    client.conf(_config(_valid_telemetry(OTLP_PORT)))   # endpoint → this port
    assert client.get()['status'] == 200
    ...
finally:
    _kill(proc)
```

For manual reproduction, launch the same fixed ports by hand and let one
FreeUnit config point at one of them:

```bash
fake_otlp --port 7970 --protocol http  --requests 1 --dump /tmp/http.bin &
fake_otlp --port 7980 --protocol grpc  --requests 1 --dump /tmp/grpc.bin &
```

> Migration note: the current `test_otel.py` still uses `_get_free_port()`
> (ephemeral). Moving each collector-using test onto its reserved port above is
> tracked in PLAN.md (WS-B3).

## Adding a case

Give it a **new reserved port + shared token** in the registry, a Rust path that
reuses `serve_http` / `serve_grpc` (or a new `serve_<token>` if the collector
behaviour itself differs), and a matching `test_otel_<token>` pytest function, so
the three-way name mirroring stays complete.
