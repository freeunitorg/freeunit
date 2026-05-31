# fake_otlp — test plan & case backlog

Living checklist for OpenTelemetry export coverage driven by `fake_otlp`.
Captures the OTLP/HTTP path shipped in 1.35.6 **plus** the OTLP/gRPC
re-introduction (see `TODO.md` → "Future: re-introduce OTLP/gRPC export") so the
gRPC export path lands with real regression tests, not as dead code.

`fake_otlp` is a live mock OTLP collector. FreeUnit **exports** spans to it; the
test drives FreeUnit and asserts what the **collector** received (dumped bytes).
The mock speaks both transports in one build (HTTP std-only, gRPC via tonic),
mirroring FreeUnit itself — a single `--otel` build, protocol chosen by config.
Keep it deterministic. Naming/port convention (one
reserved port + shared token per test, Rust fn mirrors test prefix) is in
[README.md](README.md).

## Current protocols

| Protocol | Behavior | Rust handler | Build |
|----------|----------|--------------|-------|
| `http` | `POST /v1/traces`, read `Content-Length`, reply empty `200` | `serve_http` | ✅ std-only |
| `grpc` | unary `TraceService/Export` over HTTP/2, empty `ExportTraceServiceResponse` | `serve_grpc` | ✅ tonic/prost (always built) |

## Case matrix

Status: ✅ covered · 🔲 to add

### OTLP/HTTP — shipped in 1.35.6

| # | Case | Port | Telemetry | Assertion | Status |
|---|------|------|-----------|-----------|--------|
| 1 | Span exported with service name | 7970 | `protocol=http` | dump contains `b'FreeUnit'` | ✅ `test_otel_span_exported_with_service_name` |
| 2 | traceparent injected into response | 7971 | `protocol=http` | response carries `traceparent` header | ✅ `test_otel_traceparent_in_response` |
| 3 | Inherited traceparent continued | 7972 | `protocol=http` | dump contains the 16 raw trace-id bytes | ✅ `test_otel_traceparent_inherited` |
| 4 | sampling_ratio=0 exports nothing | 7973 | `protocol=http`, `sampling_ratio=0` | collector receives no span before timeout | ✅ `test_otel_sampling_zero_exports_nothing` |

### Config validation — no collector

| # | Case | Assertion | Status |
|---|------|-----------|--------|
| 5 | `protocol` enum | `"https"`/garbage rejected | ✅ `test_otel_protocol_invalid_rejected` |
| 6 | `batch_size` bounds | 0 and >65536 rejected; 65536 accepted | ✅ `test_otel_batch_size_*` |
| 7 | `sampling_ratio` bounds | <0 and >1 rejected; 0 and 1 accepted | ✅ `test_otel_sampling_ratio_*` |
| 8 | `protocol="grpc"` accepted | accepted on any `--otel` build | ✅ `test_otel_protocol_grpc_accepted` |

### OTLP/gRPC — re-introduced (config-selected `protocol`)

Cases 10–12 are the gRPC mirror of HTTP cases 1/3/4 — same assertions, different
transport. Implemented by **parametrizing** the shared HTTP test bodies over
`["http","grpc"]` (not separate functions). No transport skips: both are always
built, so a grpc case that can't reach the collector is a real failure.

| # | Case | Telemetry | Assertion | Status |
|---|------|-----------|-----------|--------|
| 9 | `protocol="grpc"` accepted | `protocol=grpc` | config succeeds | ✅ `test_otel_protocol_grpc_accepted` |
| 10 | gRPC span exported with service name | `protocol=grpc` | decoded `Export` contains `service.name=FreeUnit` | ✅ `test_otel_span_exported_with_service_name[grpc]` |
| 11 | gRPC inherited traceparent continued | `protocol=grpc` | decoded request keeps inherited trace id | ✅ `test_otel_traceparent_inherited[grpc]` |
| 12 | gRPC sampling_ratio=0 exports nothing | `protocol=grpc`, `sampling_ratio=0` | collector receives no `Export` before timeout | ✅ `test_otel_sampling_zero_exports_nothing[grpc]` |

---

## OTLP/gRPC re-introduction plan

Both transports are compiled into every `--otel` build; the config picks one at
runtime — matching upstream Unit (and what the public site already documents).
Per the 2026-05-31 audit in `TODO.md`, tokio is already linked transitively
(`reqwest-blocking → hyper/tower`) and `src/wasm-wasi-component/` already runs a
managed `tokio::runtime::Runtime` on its own thread — so the real delta is
`tonic` + tokio `rt` + *we* owning a small runtime, not adding tokio from zero.

### WS-A — FreeUnit emits gRPC

- [x] `src/otel/Cargo.toml`: `opentelemetry-otlp` features
      `["trace","http-proto","reqwest-blocking-client","grpc-tonic"]` + `tonic`
      + tokio `["rt-multi-thread"]`, all non-optional (no Cargo feature gate).
- [x] `src/otel/src/lib.rs` (`nxt_otel_rs_init`): branch on `proto` —
      `"http"` → blocking-reqwest path; `"grpc"` → `.with_tonic()` exporter on a
      small multi-thread tokio runtime owned by the export thread (stashed in a
      static, dropped on `nxt_otel_rs_shutdown_tracer`). Unknown protocol →
      rejected at runtime via `log_callback`.
- [x] `src/nxt_conf_validation.c` (`nxt_otel_validate_protocol`): accept both
      `"http"` and `"grpc"`; anything else → clear error.
- [x] `docs/unit-openapi.yaml`: `enum: ["http","grpc"]` on `protocol`.
- [x] Docs: gRPC default port **4317** vs HTTP **4318**; v1 plaintext only (no
      TLS) — captured in the openapi `protocol`/`endpoint` descriptions.
- [x] No new build flag: a single `--otel` builds both transports (the public
      site documents exactly this — `./configure --otel` + `protocol:"grpc"`).

### WS-B — `fake_otlp` speaks gRPC + tests

- [x] `test/fake_otlp/Cargo.toml`: tonic/prost/tokio/opentelemetry-proto as
      non-optional deps — one build serves both transports (no Cargo feature).
- [x] `src/grpc.rs`: a tonic `TraceServiceServer` started when `--protocol grpc`;
      `main.rs` dispatches `serve_http` vs `serve_grpc`. Mirrors the HTTP
      contract: `--port`, `--requests`, `--dump` (writes the re-encoded
      `ExportTraceServiceRequest` so the byte-substring assertions keep working),
      prints `span_received`.
- [x] Harden the **HTTP** path: assert request line `POST /v1/traces`,
      `Content-Type: application/x-protobuf`, reject empty bodies with 400.
- [x] `test/test_otel.py`:
  - [x] `_run_fake_otlp`: add `protocol="http"` → appends `--protocol`.
  - [x] `_valid_telemetry`: per-protocol `endpoint` (HTTP needs `/v1/traces`,
        grpc targets host:port) + `protocol` field.
  - [x] `test_otel_protocol_grpc_accepted` — grpc valid on any `--otel` build
        (no skip; both transports always present).
  - [x] **Parametrized** export cases over `["http","grpc"]` (cases 10–12) —
        no transport skip; an unreachable collector is a real failure.
  - [ ] **Port migration** — kept `_get_free_port()` deliberately: dynamic free
        ports avoid cross-file collisions and TOCTOU flakiness, and the mock
        binds the test-chosen port anyway (see Notes). The `797x`/`798x`
        registry stays informational for manual reproduction.

## Acceptance criteria

- [x] `protocol="grpc"` accepted on any `--otel` build (config-selected).
- [x] A `protocol="grpc"` traced request lands at `fake_otlp --protocol grpc`;
      decoded `Export` contains `service.name=FreeUnit`.
- [x] Inherited-traceparent and sampling-zero pass for **both** transports.
- [x] HTTP cases 1–8 stay green alongside the gRPC cases in one build.

## Risks / open questions

- [ ] **TLS for gRPC** — v1 plaintext-only; revisit aws-lc-sys / second-OpenSSL
      cost (`TODO.md`) only if TLS-to-collector demand appears.
- [ ] **Runtime shape** — small multi-thread tokio owned by the export thread;
      verify clean `shutdown()`/flush on reconfigure so the span-leak follow-up
      in `TODO.md` is not reopened.

## Notes

- gRPC default port is **4317**, HTTP **4318** (OTLP spec). The registry keeps
  gRPC cases on `798x` and HTTP on `797x` regardless, since the mock binds the
  test-chosen port — the 4317/4318 split only matters for docs/examples.
- Keep any new CLI flag backward-compatible with the existing
  `--port/--requests/--dump` so the shipped HTTP tests do not change invocation.

## References

- `TODO.md` → "Future: re-introduce OTLP/gRPC export" (Options A/B/C, ecosystem
  table, 2026-05-31 tokio re-audit).
- `src/otel/src/lib.rs` — `nxt_otel_rs_init` (protocol branch).
- `src/nxt_conf_validation.c` — `nxt_conf_vldt_otel` (protocol enum).
- OTLP spec (gRPC 4317 / HTTP 4318): https://opentelemetry.io/docs/specs/otlp/
