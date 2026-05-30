# TODO

---

## Release roadmap

| Milestone | Due | Focus |
|-----------|-----|-------|
| **1.35.5** | current branch | Docker builder mode, parallel builds, Go 1.26, Node 24 |
| **1.35.6** | 2026-06-25 | OTEL 0.24→0.32 (#65), Rust DX (`rust1.x` variant, `libunit-rust`) |
| **1.35.7** | 2026-07-31 | Short-cycle release |
| **1.35.8** | 2026-08-28 | Short-cycle release + Docker Hub Official Images (`make library`) |

### Action plan — work streams & start dates

**Now (June):**

| Task | Milestone |
|------|-----------|
| Finish PR #66 (builder mode, parallel builds, README) | 1.35.5 |
| OTEL Phase 0: config audit, new fields, `fake_otlp`, `test_otel.py` | 1.35.6 |
| OTEL Phase 2: rewrite `nxt_otel_rs_runtime()` for 0.32 API (after Phase 0) | 1.35.6 |
| `rust1.x` Docker variant (WASM path, `Dockerfile.rust1.x`) | 1.35.6 |
| Docker: make debug build optional (`--debug` flag in `build-local.sh`, off by default; saves ~30-40 MB per image) | 1.35.6 |

**July (after 1.35.6):**

| Task | Milestone |
|------|-----------|
| `libunit-rust` SDK — bindgen + FFI + axum adapter (prototype) | 1.35.7 |
| Evaluate `libunit-rust` prototype → decide: WASM-first or native-first | 1.35.7 |
| `packages.freeunit.org` — GoAccess / JSON stats for download counter | 1.35.7 |
| `fake_upstream` prebuilt binary on `packages.freeunit.org` | 1.35.7 |

**August 1 — start 1.35.8 work:**

| Task | Milestone |
|------|-----------|
| Prepare `docker-library/official-images` PR: uncomment `make library` in Makefile, update `GitFetch`, test metadata generation | 1.35.8 |
| Docker Hub Official Images: go through review process (docker-library/official-images PR template, CI validation) | 1.35.8 |
| clang-ast plugin prebuilt binary on `packages.freeunit.org` | 1.35.8 |
| `libunit-rust` — crates.io publish (if 1.35.7 prototype passes review) | 1.35.8 |
| OTEL Phase 3: housekeeping (`"NGINX Unit"` → `"FreeUnit"`, `eprintln!` → log_callback) | 1.35.8 |

**Open questions (decide before August):**

- [ ] PHP TrueAsync — determine source of `nxt_php_extension.c` (fork EdmondDantes or write from scratch)
- [ ] PHP 8.5 `rootfs` SIGSEGV — needs diagnostics on real hardware
- [ ] OpenSSL 3.6 migration — verify clang-ast compatibility
- [ ] Proxy request buffering (#58) — define scope (per-action vs global)

---

## Release 1.35.6 — Rust developer experience

**Goal:** attract Rust developers — the fastest-growing language community with no
dedicated app server since Unit was archived. FreeUnit becomes the first app server
with a native Rust workflow.

### Background & analysis (2026-05-26)

- `rust:1-slim-trixie` confirmed on Docker Hub — ships Rust 1.95.0, `RUSTUP_HOME` /
  `CARGO_HOME` already set, same layout as our `freeunit-builder:trixie-rust1.94.1`.
  Image pulled and verified locally.
- All 21 trixie-based Dockerfiles already download Rust at build time (for wasmtime /
  wasm-wasi-component) and then discard it. The rust variant just keeps it in the final
  image — same pattern as `go1.x` variants.
- `libunit-go` is 676 lines of Go + 127 lines of C glue over `nxt_unit.c` (6800 lines).
  Path A for `libunit-rust` (bindgen) is estimated at ~500–1000 lines of Rust — a
  reasonable scope for a single contributor.
- Path B (pure Rust protocol reimplementation) rejected: too risky, too large, no benefit
  until `libunit-rust` has proven adoption.
- No existing Rust SDK for Unit found on GitHub (searched `unit-rs`, `libunit-rust`,
  `nginx-unit-rust`). First-mover advantage available.
- axum: 26k ⭐, actix-web: 24k ⭐ — Rust web ecosystem is mature and large.
- ngx-rust (https://github.com/nginx/ngx-rust) reviewed as reference. Longevity risk:
  F5-controlled, WIP, 60% commits from one engineer — use as pattern reference only.

### Step 1 — `rust1.x` Docker variant (WASM path, no C changes needed)

Base image `rust:1-slim-trixie` already ships Rust 1.95.0 with `RUSTUP_HOME` /
`CARGO_HOME` configured (same layout as our builder images). Only additions needed:
- `rustup target add wasm32-wasip1` inside the Dockerfile
- Build FreeUnit with wasm + wasm-wasi-component modules (same as `Dockerfile.wasm`)
- Keep Rust toolchain in the final image — identical pattern to `go1.x` variants

Result: `ghcr.io/freeunitorg/freeunit:latest-rust1.x` — write, compile, and serve
Rust WASM apps from a single container. No external toolchain, no separate build step.

- [ ] Create `pkg/docker/Dockerfile.rust1.x` based on `rust:1-slim-trixie`
- [ ] Add `rust1.x` to `docker.yml` CI matrix
- [ ] Add `rust1.x` to `ALL_VARIANTS` in `build-local.sh`
- [ ] Add `rust1.x` to variants table in `pkg/docker/README.md`
- [ ] Test: compile a minimal axum→WASM app and serve via FreeUnit wasm runtime

### Step 2 — `libunit-rust` SDK (native path, Path A: bindgen + FFI)

Mirrors `go/` library. Rust apps import `libunit-rust`, link `libnxt_unit.a`, and
speak the Unit app-worker protocol directly — no WASM compilation needed.

Architecture (same as Go):
```
nxt_unit.c (6800 lines)  ←  C protocol core, already battle-tested
     ↓  bindgen on nxt_unit.h
libunit-rust/src/ffi.rs  ←  generated FFI types (~auto)
libunit-rust/src/lib.rs  ←  safe wrappers (~500-1000 lines)
libunit-rust/src/axum.rs ←  axum/hyper adapter (drop-in replace for std listener)
```

Reference files: `go/unit.go`, `go/port.go`, `go/request.go`, `go/response.go`,
`go/nxt_cgo_lib.h`.

- [ ] Run `bindgen` on `src/nxt_unit.h` → `libunit-rust/src/ffi.rs`
- [ ] Implement safe wrappers: port management, request/response, handler registry
- [ ] Add `axum` adapter: `ListenAndServe(handler)` equivalent
- [ ] Publish to crates.io as `libunit-rust`
- [ ] Add example app to `tools/` or a separate `examples/rust/` directory

### Why this matters for growth

| | Today | After 1.35.6 |
|---|---|---|
| Rust WASM apps | manual setup | `docker pull freeunit:latest-rust1.x` |
| Rust native apps | not supported | `libunit-rust` on crates.io |
| Developer story | "compile to wasm32-wasi manually" | one-liner |

Rust is the #1 most admired language (Stack Overflow 2024). No app server currently
serves Rust developers. FreeUnit can own this space.

---

## OpenTelemetry crate upgrade 0.24 → 0.32 (issue #65, milestone 1.35.6)

| Crate | Current | Latest | Gap |
|-------|---------|--------|-----|
| `opentelemetry` | 0.24.0 | 0.32.0 | 8 minor |
| `opentelemetry-otlp` | 0.17.0 | 0.32.0 | 15 minor |
| `opentelemetry_sdk` | 0.24.1 | 0.32.0 | 8 minor |
| `opentelemetry-semantic-conventions` | 0.16.0 | 0.32.0 | 16 minor |

Original implementation by Ava Hahn — co-author of FreeUnit's OTel layer.
New account: [@ava-affine](https://github.com/ava-affine), `ava@sunnypup.io` (left F5, personal domain `sunnypup.io`).
The upstream `nginx/nginx-otel` is a separate C++ module, unrelated to our Rust crate.

### Known pitfalls (from code audit)

- `opentelemetry_otlp::new_pipeline()` removed in 0.27 — `nxt_otel_rs_runtime()` must be
  rewritten from scratch, not patched
- `BoxedSpan` is passed as a raw pointer across the C FFI boundary (`r->otel->trace`);
  verify its memory layout did not change between 0.24 and 0.32 before writing new code
- `opentelemetry_sdk::trace::Config`, `BatchConfigBuilder`, `runtime::Tokio` — all moved
  or removed; check new API in `opentelemetry_sdk` 0.32 docs
- `Protocol::HttpJson` arm in the runtime match is unreachable dead code — fix in same PR
- Service name `"NGINX Unit"` hardcoded in `lib.rs` lines 154 and 274 — rename to `"FreeUnit"`
- `eprintln!` used for errors in `nxt_otel_rs_runtime()` instead of the `log_callback` —
  fix while rewriting the function

### Phase 0: Documentation audit + new config fields + test infrastructure (prerequisite)

**Current config (4 fields):**
```json
{ "endpoint": "...", "protocol": "http", "batch_size": 128, "sampling_ratio": 1.0 }
```

**0a — Audit existing config:**
- [ ] Audit current OTEL JSON config schema against `docs/unit-openapi.yaml`
- [ ] Document all supported OTEL config fields and their defaults
- [ ] Check for gaps between OpenAPI spec and actual C/Rust implementation

**0b — New configuration fields (backward-compatible, current crate versions):**

Hardcoded values that must become configurable:

| Value | Current | Where | Proposed field |
|-------|---------|-------|----------------|
| Service name | `"NGINX Unit"` | `lib.rs:98,274` | `service_name` |
| Max queue size | `4096` | `lib.rs:103` | `max_queue_size` |
| Export timeout | `10s` | `lib.rs:27` | `export_timeout` |

Missing fields needed for production OTEL:

| Field | Type | Default | Why |
|-------|------|---------|-----|
| `service_name` | string | `"FreeUnit"` | Replaces hardcoded `"NGINX Unit"`; every service needs its own name |
| `headers` | object | `{}` | Auth to collector (`Authorization: Bearer ...`, `X-Api-Key`) |
| `root_certificate` | string | — | Custom CA for TLS collector connection |
| `resource_attributes` | object | `{}` | `service.version`, `deployment.environment`, custom labels |
| `max_queue_size` | integer | `4096` | Replaces hardcoded queue depth |
| `export_timeout` | integer (sec) | `10` | Replaces hardcoded timeout |

Bug fixes in existing fields:
- [ ] Fix `protocol`: add `enum: ["http", "grpc"]` to OpenAPI; sync required/optional between C validator and OpenAPI
- [ ] Fix `batch_size`: add upper bound validation (e.g. `<= 65536`)
- [ ] Fix span attributes to use OTel semconv: `"method"` → `http.request.method`, `"path"` → `url.path`, `"status"` → `http.response.status_code`
- [ ] Wrap sampler in `ParentBased(TraceIdRatioBased(...))` — respect upstream sampling decisions
- [ ] Pass `tracestate` through to Rust OTEL SDK (currently parsed in C, echoed, but dropped)

New fields implementation:
- [ ] Add all new fields to `docs/unit-openapi.yaml` (`configSettingsTelemetry` schema)
- [ ] Add validators in `src/nxt_conf_validation.c`
- [ ] Parse new fields in `src/nxt_router.c`
- [ ] Accept and use new parameters in `src/otel/src/lib.rs`

**Proposed full config after Phase 0:**
```json
{
  "settings": {
    "telemetry": {
      "endpoint": "http://collector:4318",
      "protocol": "http",
      "service_name": "my-app",
      "sampling_ratio": 1.0,
      "batch_size": 128,
      "max_queue_size": 4096,
      "export_timeout": 10,
      "headers": { "Authorization": "Bearer token" },
      "root_certificate": "/path/to/ca.pem",
      "resource_attributes": {
        "service.version": "1.0.0",
        "deployment.environment": "production"
      }
    }
  }
}
```

**0c — Test infrastructure:**
- [ ] Build `test/fake_otlp/` — std-only Rust mock OTLP collector

**fake_otlp design** — mirrors `test/fake_upstream/` exactly: single Rust binary,
no external deps, installed to `/usr/local/bin/fake_otlp` in CI (same step as
`fake_upstream`).

```
fake_otlp --port 19878 --requests 1
```

- Accepts `POST /v1/traces`, validates `Content-Type: application/x-protobuf` + non-empty body
- Responds `200 OK` with empty body (valid `ExportTraceServiceResponse`)
- Prints `span_received content_length=NNN` to stdout per request
- Exits after `--requests N`
- **HTTP only** — gRPC (HTTP/2) not supported; document as "use a real collector for gRPC"

- [ ] Write `test/test_otel.py` — gated on `FAKE_OTLP_BIN` + `--otel` build flag:

| Test | What it checks |
|------|----------------|
| `test_otel_span_exported` | span arrives at fake_otlp after one FreeUnit request |
| `test_otel_traceparent_propagated` | FreeUnit injects `traceparent` header into forwarded request |
| `test_otel_sampling_zero` | `sampling_ratio=0.0` → fake_otlp receives nothing (stays alive) |
| `test_otel_service_name` | exported span contains configured `service_name` |
| `test_otel_auth_header` | fake_otlp receives configured `headers` (Authorization) |
| `test_otel_resource_attributes` | span resource contains custom attributes |

- [ ] Verify all Phase 0 tests pass against the **current** 0.24 crates (establishes baseline)

### Phase 1: Pre-upgrade safety checks

- [ ] Verify `BoxedSpan` layout compatibility between 0.24 and 0.32 — `Arc<BoxedSpan>`
      crosses C FFI as raw pointer; layout change = UB. If trait bounds changed (e.g. added
      `Send + Sync`), introduce an intermediate opaque wrapper type
- [ ] Confirm `Protocol::HttpJson` is dead code — remove unreachable match arm

### Phase 2: Crate upgrade — full rewrite of `nxt_otel_rs_runtime()`

- [ ] Bump all `opentelemetry*` crates to 0.32.x in `src/otel/Cargo.toml`
- [ ] Rewrite `nxt_otel_rs_runtime()` — `new_pipeline()`, `new_exporter()`, `.tracing()`,
      `.with_trace_config()`, `.with_batch_config()`, `.install_batch()` all gone in 0.27+.
      Use `TracerProvider::builder()` + new `OtlpTracePipeline` API
- [ ] Replace `opentelemetry_sdk::trace::Config` → `TracerProviderBuilder`
- [ ] Replace `BatchConfigBuilder` → new location/API
- [ ] Replace `opentelemetry_sdk::runtime::Tokio` → new runtime model
- [ ] Adapt `src/otel/src/lib.rs` to all remaining API changes
- [ ] Update `src/nxt_otel.c` / `nxt_otel.h` if Rust ABI changed
- [ ] Run `test/test_otel.py` against upgraded code — must pass same baseline tests

### Phase 3: Housekeeping fixes (same PR)

- [ ] Rename `"NGINX Unit"` → `"FreeUnit"` in `lib.rs` (lines 154, 274)
- [ ] Replace `eprintln!` (lib.rs lines 188–189, 204) with `nxt_otel_log_callback`

### Phase 4: Final verification

- [ ] Build with `./configure --otel --openssl && make`; run clang-ast check
- [ ] Full `test/test_otel.py` pass

### Follow-up: span leak on aborted requests (pre-existing, found in #65 review)

`nxt_otel_rs_get_or_create_trace()` returns `Box::into_raw(...)` into
`r->otel->trace`; the span is only reclaimed/ended by `nxt_otel_rs_send_trace()`,
which is reached **only** via `NXT_OTEL_COLLECT_STATE` (`nxt_otel_span_collect`,
`src/nxt_otel.c`). All `ERROR_STATE` transitions occur while `trace == NULL`, so
the `nxt_otel_error()` "nothing to leak" comment holds for them. But if a request
is torn down between `INIT` and `COLLECT` (e.g. connection reset during body),
`r->otel->trace` is non-null and `send_trace` is never called → the boxed span
leaks. The state machine has no request-teardown hook.

Not a regression — same lifecycle existed before the 0.24→0.32 upgrade.

- [ ] Add a teardown path (request free / error finalize) that calls
      `nxt_otel_rs_send_trace()` (or a dedicated drop) when `r->otel->trace` is
      non-null and `COLLECT` was not reached, so the span is always reclaimed.

### Future: re-introduce OTLP/gRPC export (milestone TBD, post-1.35.6)

1.35.6 removed `protocol: "grpc"` and shipped HTTP-only export on a blocking
`reqwest` client + dedicated-thread `BatchSpanProcessor` (no tokio). Re-adding
gRPC is a deliberate future feature, gated on solving the runtime question below.

**Why gRPC was removed — it was an architecture decision, not a Rust bug or a
Phase-0 blocker.** The chain:

1. Upstream `opentelemetry-otlp` removed `new_pipeline()` in 0.27, so the exporter
   had to be *rewritten from scratch*, not patched (see "Known pitfalls" above).
2. The gRPC path in the crate is the `grpc-tonic` feature → pulls `tonic` → which
   mandates a **`tokio` async runtime**. The old 0.24 `Cargo.toml` carried exactly
   that: `opentelemetry-otlp` features `["http-proto", "tokio", "grpc-tonic",
   "tonic", "reqwest-rustls"]` + `tokio = { features = ["full"] }` + `rt-tokio`.
3. For an LTS fork that embeds this Rust staticlib inside the C app server, we
   wanted a predictable runtime with **no async executor we manage**. So the
   rewrite uses the blocking `reqwest` client driven by a dedicated-thread
   `BatchSpanProcessor` (new `Cargo.toml`: `default-features = false`, features
   `["trace", "http-proto", "reqwest-blocking-client"]`, no tokio, no tonic).
4. gRPC/tonic is inherently async and cannot run without tokio → dropping tokio
   **necessarily** drops gRPC. The removal is a *consequence* of the runtime
   simplification, not a goal in itself.
5. Bonus simplification: no TLS stack is linked either. reqwest 0.13 dropped the
   ring rustls provider; enabling rustls would pull `aws-lc-sys` (a cmake/C
   build), and native-tls would link a *second* OpenSSL into the staticlib. So
   the endpoint is plain `http://` (collector on localhost / internal network).

So Phase 0 had no gRPC problem; the upstream API churn forced a rewrite, and the
rewrite chose the tokio-free path on purpose. Bringing gRPC back means bringing a
tokio runtime back (Option A below) or finding a blocking gRPC transport (B).

**Ecosystem context (researched 2026-05-30):**

| Project | OTel layer | Export transport |
|---------|-----------|------------------|
| nginx (mainline) | `nginx/nginx-otel` (C++) | OTLP/**gRPC only**, port 4317 |
| Angie | `angie-module-otel` (= nginx-otel) | OTLP/**gRPC only** |
| freenginx | none native; mainline module via `--with-compat` | OTLP/**gRPC only** |
| Caddy / FrankenPHP | `tracing` directive (`opentelemetry-go`) | OTLP/**gRPC only**, port 4317 |
| opentelemetry-php (PHP app SDK) | OTel PHP extension/SDK | OTLP/**HTTP** (http/protobuf), port 4318 |
| **FreeUnit** | `src/otel/` (Rust `opentelemetry-otlp`) | OTLP/**HTTP only** (since 1.35.6) |

So the entire reverse-proxy / web-server family (nginx, Angie, Caddy/FrankenPHP)
is gRPC-first at the server layer. FreeUnit is the odd one out: a Rust crate
embedded in Unit, where HTTP export needs no async runtime but gRPC does. The
nginx-otel directive model (`otel_exporter { endpoint; interval; batch_size;
batch_count; }`, `otel_service_name`, `otel_trace on|off|$var`,
`otel_trace_context`) is a useful reference for our future config-field set (cf.
Phase 0b `service_name`, `resource_attributes`), but not for transport.

**Two findings that *validate* HTTP-only rather than argue against it:**

1. **HTTP-only aligns with the PHP app world.** `opentelemetry-php` (the PHP
   app-level SDK) exports over **OTLP/HTTP** (http/protobuf, port 4318) — exactly
   FreeUnit's transport. In FrankenPHP the server (Caddy, gRPC :4317) and the PHP
   app (HTTP :4318) actively *conflict* over the shared `OTEL_EXPORTER_OTLP_*`
   env vars (frankenphp#1715, caddy#5743 wants HTTP added). FreeUnit being an
   app server for PHP-first workloads, HTTP-only is the *consistent* choice, not
   a gap. If/when gRPC returns it should stay opt-in, never the default.

2. **Caddy config model — standard `OTEL_*` env vars, not its config file.**
   Caddy deliberately configures the exporter through the OTel env-var spec
   (`OTEL_EXPORTER_OTLP_ENDPOINT`, `OTEL_SERVICE_NAME`,
   `OTEL_TRACES_SAMPLER=parentbased_traceidratio`, `OTEL_TRACES_SAMPLER_ARG`),
   *not* the Caddyfile. Worth considering as a complement to our JSON config:
   - [ ] Evaluate honouring standard `OTEL_*` env vars as fallback/override for
         `settings.telemetry` (interop with existing OTel deployments; lets the
         same env that configures the PHP SDK also reach FreeUnit).

**Access-log trace id — direct reference for roadmap item #2.** Caddy adds
`traceID`/`spanID` to its access logs and exposes `{http.vars.trace_id}` /
`{http.vars.span_id}` placeholders to forward into the app. This is exactly the
`$otel_trace_id` in access logs feature (roadmap #2, "first community PR
candidate"). Use Caddy's placeholder + access-log-field approach as the design
reference when implementing it.

**The core problem — gRPC needs an async runtime:**
- `opentelemetry-otlp` gRPC = `grpc-tonic` feature → pulls `tonic` + `tokio`.
  That is exactly the tokio dependency 1.35.6 removed to get a predictable,
  single-dedicated-thread runtime for an LTS fork.
- Options to evaluate before committing to gRPC:
  - [ ] **A — opt-in tokio, HTTP stays default.** Re-add `tonic`+`tokio` behind
        a Cargo feature; spin a minimal current-thread tokio runtime on the
        existing dedicated BSP thread *only* when `protocol: "grpc"` is set.
        HTTP path keeps zero-async. Biggest dep surface.
  - [ ] **B — blocking gRPC transport.** Investigate a tokio-free gRPC client
        (e.g. driving `tonic` over a hand-rolled `block_on`, or a lighter
        HTTP/2 client). High risk; gRPC-without-async is not a solved problem
        in the Rust ecosystem.
  - [ ] **C — don't.** Stay HTTP-only; document that gRPC users put an OTel
        Collector (or `otelcol` sidecar) in front to translate OTLP/HTTP→gRPC.
        Lowest cost; matches "internal collector on localhost" deployment model
        we already document.

**When re-introducing (whichever option):**
- [ ] Restore `enum: ["http", "grpc"]` in `docs/unit-openapi.yaml` and the C
      validator (`nxt_conf_vldt_otel` in `src/nxt_conf_validation.c`); today both
      hard-reject `grpc`.
- [ ] Map gRPC default port 4317 vs HTTP 4318 in docs/examples (the sibling
      `docs/` + `freeunitorg.github.io` landing pages were corrected to HTTP/4318
      in 1.35.6 — revisit both if gRPC returns).
- [ ] Update `test/test_otel.py`: `test_otel_protocol_grpc_rejected` must flip to
      an acceptance/export test; `test/fake_otlp` is HTTP-only, so a gRPC path
      needs a real collector or an HTTP/2 mock.

**Recommendation:** default to Option C until there is concrete user demand;
prototype Option A behind a feature flag if demand appears. Do **not** re-add
tokio to the default build.

**Rust toolchain note (2026-05-30).** Rust 1.96 was just released (6-week
cadence; memory-safe, no GC, runtime limited to std-lib init — fits an embedded
staticlib well). Our builds pin **1.94.1** (`test/fake_otlp/rust-toolchain.toml`,
the `freeunit-builder:trixie-rust1.94.1` image, Docker variants), while the
floating `rust:1-slim-trixie` base for the planned `rust1.x` variant now tracks
1.95→1.96. Before any OTel/gRPC rework, and before shipping `rust1.x`:
- [ ] Bump the pinned toolchain (1.94.1 → current stable) and re-run the
      otel-enabled clang-ast build + `test_otel.py` to confirm the 0.32 crates
      and the staticlib still compile clean on newer Rust.
- [ ] Decide pin-vs-floating policy for the `rust1.x` image (floating base drifts
      to whatever `rust:1-slim-trixie` ships; pin for reproducible CI).

### Sources (researched 2026-05-30)

OTel transport across the reverse-proxy / app-server ecosystem:
- nginx-otel (C++, gRPC-only): https://github.com/nginx/nginx-otel
- NGINX OpenTelemetry admin guide: https://docs.nginx.com/nginx/admin-guide/dynamic-modules/opentelemetry/
- Angie OTel module (gRPC-only): https://en.angie.software/angie/docs/installation/external-modules/otel/
- Caddy `tracing` directive (opentelemetry-go, gRPC-only, OTEL_* env config): https://caddyserver.com/docs/caddyfile/directives/tracing
- Caddy issue #5743 — request to add OTLP/HTTP protocol: https://github.com/caddyserver/caddy/issues/5743
- FrankenPHP issue #1715 — Caddy(gRPC) vs PHP(HTTP) protocol conflict: https://github.com/php/frankenphp/issues/1715

Rust release (toolchain-pin context):
- Rust 1.96 release (ru): https://opennet.ru/65574/  (mirror: https://opennet.me/65574/)

---

## PHP TrueAsync mode (branch: php-graceful-shutdown)

Items below must be resolved before the branch can be merged and before
`test/test_php_trueasync.py` can run in CI.

### 1. `nxt_php_app_conf_t` is missing `async` and `entrypoint` fields

**Problem.**  `src/nxt_php_sapi.c` (commit 58ffa236) references `c->async`
and `c->entrypoint` where `c` is a pointer to `nxt_php_app_conf_t`
(defined in `src/nxt_application.h`).  The struct currently has only
`targets` and `options`, so the code **will not compile** as-is.

The original upstream commit (EdmondDantes/052afd8) likely also modified
`nxt_application.h` and the JSON config validator; those changes were not
included when the commit was cherry-picked into this branch.

**Risk.** Build failure on the first C compile step; CI is entirely broken.

**Fix.**
- Add the missing fields to `nxt_php_app_conf_t` in `src/nxt_application.h`:
  ```c
  typedef struct {
      nxt_conf_value_t  *targets;
      nxt_conf_value_t  *options;
      nxt_bool_t         async;      /* true when TrueAsync mode requested */
      nxt_str_t          entrypoint; /* path to the PHP entrypoint script   */
  } nxt_php_app_conf_t;
  ```
- Wire them up in the config parser (find where other PHP config fields are
  read from JSON — probably `nxt_router.c` or a PHP-specific conf handler).
- Add validation rules in `nxt_conf_validation.c` so the REST API rejects
  `entrypoint` without `async: true` and vice-versa.

---

### 2. `nxt_php_extension.c` / `nxt_php_extension_init()` are missing

**Problem.**  `PHP_MINIT_FUNCTION(nxt_php_ext)` calls
`nxt_php_extension_init()` (line 221 of `nxt_php_sapi.c`) and the async
request handler references `nxt_php_request_callback` (declared `extern`
at line 2187).  Both symbols are expected to come from `nxt_php_extension.c`,
which does not exist in the repository.

Without it the linker will fail and there is no PHP-land API for user
scripts to register a request handler.  The `\Unit\Server::setHandler()`
calls in `test/php/async_*/entrypoint.php` are placeholders until the
real extension is in place.

**Risk.** Linker failure; all TrueAsync tests will skip until resolved.

**Fix.**
- Locate the file in the EdmondDantes fork or write it from scratch.
  Minimum required:
  - `zval *nxt_php_request_callback` global (zeroed at startup).
  - `nxt_php_extension_init()` — registers `\Unit\Server` with a static
    `setHandler(callable $cb)` that stores `$cb` into the global above.
  - `\Unit\Request` class with methods `body()`, `query()`, `headers()`,
    `respond(int $status, array $headers, string $body)`.
- Add the new `.c` file to the PHP module build rules in `auto/modules/php`.
- Document the final PHP API surface so entrypoint authors have a stable
  contract.

---

### 3. No timeout / force-kill fallback in `nxt_php_quit_handler`

**Problem.**  `nxt_php_quit_handler()` calls `ZEND_ASYNC_SHUTDOWN()` and
returns immediately.  If user coroutines are stuck in a tight loop or
blocking I/O that the TrueAsync scheduler cannot interrupt, the worker
hangs forever.

**Risk.** Silent hang in production; container runtime must SIGKILL instead.

**Fix.**
- After `ZEND_ASYNC_SHUTDOWN()`, arm a timer for a configurable grace
  period (default 30 s).  On expiry call `exit(1)`.
- Expose as a PHP application config option:
  ```json
  { "type": "php", "async": true, "entrypoint": "server.php",
    "shutdown_timeout": 30 }
  ```
- Add a test that verifies the force-kill fires when a coroutine refuses
  to stop.

---

### 4. Behavior of in-flight requests on shutdown is undefined

**Problem.**  It is not documented whether `ZEND_ASYNC_SHUTDOWN()` lets
active request coroutines run to completion or cancels them immediately.

`test_php_trueasync_inflight_request_completes` assumes completion
semantics.  If cancellation is the designed behavior, the test assertion
must be changed to `pytest.xfail`.

**Action.**
- Read the TrueAsync scheduler shutdown semantics.
- Document the behavior.  Update the test accordingly.

---

### 5. Pending writes in `drain_queue` are silently abandoned on shutdown

**Problem.**  If `ZEND_ASYNC_SHUTDOWN()` fires while `drain_queue` is
non-empty, buffered response bytes are never sent and the client receives
a truncated body.

**Fix.**  Drain the queue synchronously in `nxt_php_quit_handler()` before
calling `ZEND_ASYNC_SHUTDOWN()`.

---

### 6. Test fixtures use a placeholder PHP API

The entrypoint scripts in `test/php/async_*/entrypoint.php` use
`\Unit\Server::setHandler()` and `\Unit\Request` — these are provisional
names.  Once item 2 is resolved:
- Update all three fixture files to match the real class/method names.
- Replace the `_check_trueasync_available()` runtime probe in
  `test_php_trueasync.py` with a proper `prerequisites` feature flag
  (requires updating `unit/check/discover_available.py`).

---

## OpenSSL 3.6 — test openssl-3.x branch

Before the OpenSSL 3.6 migration can be considered fully validated:

- [ ] Verify that the `openssl-3.x` branch (if it exists upstream or as a
      fork reference) still applies cleanly on top of `master` with the new
      `OBJ_sn2nid` / `OpenSSL_version_num` replacements.
- [ ] Run the full CI matrix (`ci.yml`) and confirm the new "Build OpenSSL 3.6"
      step succeeds on both `amd64` and `arm64` runners.
- [x] `clang-ast` workflow passes on `debian:testing` + system OpenSSL 1.1
      via `./test/run-local-full.sh` (verified on `pre-1.35.5` branch).
- [ ] Confirm `clang-ast` still passes when linked against OpenSSL 3.6
      (previously broken by `EVP_PKEY_asn1_find_str` / `SSLeay` deprecations
      — fixes need re-verification on the 3.6 build).
- [ ] Smoke-test TLS in a Docker image built from `Dockerfile.minimal`
      (now `debian:trixie-slim`) — load a certificate via the REST API and
      make an HTTPS request.
- [ ] Investigate `eclipse-temurin:11-jdk-noble` (Ubuntu 24.04, OpenSSL 3.3)
      as the one remaining image that does NOT reach OpenSSL 3.6; decide
      whether to build OpenSSL 3.6 from source in that Dockerfile or accept
      the gap until eclipse-temurin gains a Debian trixie variant.

---


## PHP 8.5 Compatibility

### `disable_classes` removed (PHP 8.5)

PHP 8.5 removed the `disable_classes` INI directive (deprecated since 8.4).
Unit passes it via `php_admin_value` in `nxt_php_sapi.c` — PHP 8.5 ignores it silently,
causing `test_php_application_disable_classes` and `test_php_application_disable_classes_user` to fail.

**Tests:** `test/test_php_application.py` — skipped for PHP >= 8.5 with explicit reason.

**Fix needed:**
- Remove or conditionalize `disable_classes` handling in `src/php/nxt_php_sapi.c`
- Consider returning an error from the config API if `disable_classes` is set with PHP 8.5+
- Or document the removal and drop the feature

---

### `rootfs` isolation SIGSEGV (PHP 8.5)

`test_php_isolation_rootfs` fails with signal 11 (SIGSEGV) when running PHP 8.5
inside a chroot/rootfs-isolated Unit application.

**Test:** `test/test_php_isolation.py` — skipped for PHP >= 8.5 with explicit reason.

**Likely causes:**
- PHP 8.5 introduced new shared library dependencies not present in the minimal rootfs fixture
- Or PHP 8.5 accesses paths at startup that are unavailable in the chroot

**Investigation steps:**
1. Run `ldd $(which php)` with PHP 8.5 and compare against the rootfs fixture contents
2. Check `unit.log` for the full path that caused the segfault (needs core dump or `strace`)
3. Check if `php 8.5 --define open_basedir=...` reproduces outside of Unit

---

## Test Infrastructure

### Download statistics for packages.freeunit.org

`packages.freeunit.org` serves tarballs (njs, wasmtime, wasi-sysroot, libunit-wasm)
but there is no download counter — no visibility into which packages are downloaded
and how often.

Server runs **Angie** (nginx-compatible fork, COMBINED log format).

**Options (ascending complexity):**
- [ ] **GoAccess** — install on server, parse Angie access log, publish HTML report to
      `packages.freeunit.org/stats/`
- [ ] **JSON counter via cron** — hourly `awk` over access.log → `stats.json`, enables
      badge endpoints or API consumers
- [ ] **Angie NJS counter** — shared-memory counter incremented per `.tar.gz` request,
      exposed as `/metrics` endpoint (no log parsing needed, real-time)

**Quick start (GoAccess):**
```bash
sudo apt-get install -y goaccess
goaccess /var/log/angie/packages.freeunit.org.access.log \
  --log-format=COMBINED -o /var/www/packages.freeunit.org/stats/index.html
```

---

### fake_otlp — harden request validation (deferred from #65)

`test/fake_otlp/` is built and used by `test/test_otel.py` in 1.35.6, but its
`handle()` (`src/main.rs`) currently counts **any** non-empty request as a
received span. It does not assert the request is a real OTLP export. Kept as-is
for now (the export tests pass); harden later so the mock can't be satisfied by
garbage:

- [ ] Validate request line is `POST /v1/traces`
- [ ] Validate `Content-Type: application/x-protobuf`
- [ ] Reject empty/zero-length protobuf body (currently only the readiness probe
      with a fully empty buffer is filtered)
- [ ] Optionally decode the protobuf far enough to confirm at least one span

The Phase 0 design (above) already specifies this behavior; the shipped binary
implements only the empty-probe guard. This item tracks closing that gap.

### Prebuild `fake_upstream` binary via packages.freeunit.org

`test/fake_upstream/` — Rust HTTP mock used by `test_proxy_chunked.py`.
Currently built from source in Docker (`cargo build --release`), adding ~0.5s per run.

**Improvement:**
- [ ] Build `fake_upstream` binary and publish to `packages.freeunit.org`
- [ ] Update `run-local.sh` to download prebuilt binary instead of `cargo build`
- [ ] Add SHA-512 checksum validation (like `pkg/contrib/Makefile` does for njs/wasmtime)
- [ ] Fallback to cargo build if download fails

**Benefits:**
- Faster test image builds
- Reproducible binaries across platforms (AMD64 + ARM64)
- No Rust toolchain required in Docker image

---

### clang-ast Docker build: debian:testing + `clang llvm-dev libclang-dev`

`test/run-local-full.sh` builds a Docker image for clang-ast analysis.
Fixed: use `clang llvm-dev libclang-dev` (not `clang-21 llvm-21-dev libclang-21-dev`).

**Current state:** Works on `debian:testing` (clang 21 + llvm 21).

**Future improvements:**
- [ ] Prebuild `freeunit-test-full:local` image and publish to GHCR
- [ ] Or add packages.freeunit.org binary for clang-ast plugin
- [ ] Cache Docker layers for apt install + clang-ast build

---

## avahahn/ngx-testing-fmk — evaluate as cross-platform test orchestration reference

https://github.com/avahahn/ngx-testing-fmk — personal shell-based framework by Ava Hahn
(ex-F5, co-author of FreeUnit OTel layer; new account: [@ava-affine](https://github.com/ava-affine), `ava@sunnypup.io`) for running nginx/nginx-otel tests across multiple
libvirt VMs in parallel.

**What it does:** boots libvirt VMs, rsyncs source + test dirs, builds and runs tests
on each VM in parallel, collects logs, shuts VMs down. `common.sh` provides a reusable
`parallel_invoke_and_wait` bash helper (fan-out with aggregated exit codes).

**Why it is not a direct fit for FreeUnit:**
- Hardwired to nginx + nginx-tests + nginx-otel — no Unit/FreeUnit hooks
- Requires a pre-configured libvirt infrastructure with shared credentials (`SECRET.sh`)
- FreeUnit already has pytest (`test/`) + GitHub Actions CI covering the same ground
- 0 stars, last commit Jan 2025, no active maintenance

**What is worth studying:**
- `parallel_invoke_and_wait` pattern in `common.sh` — clean bash fan-out with per-input
  log files and aggregated failure reporting; could inform a future `test/run-matrix.sh`
  if we ever need to test across distros locally without Docker
- Overall VM lifecycle approach (on → sync → build → test → off) as a template if we add
  libvirt/QEMU-based cross-distro testing outside of GitHub Actions

**Decision:** no code to borrow now. Revisit if we add a local multi-distro test matrix.

---

## ngx-rust — study Rust bindings and evaluate Rust runtime for FreeUnit

https://github.com/nginx/ngx-rust — Rust bindings for nginx dynamic modules by F5/NGINX.

**Longevity risk:** project is F5-controlled, WIP, and 60% of commits come from a single
engineer (`bavshin-f5`). F5 archived nginx/unit in Oct 2025 — same pattern applies here.
Use as a reference only; do not take a hard dependency.

**ngx-rust ≠ Rust runtime support.** ngx-rust is about writing nginx *modules* in Rust.
For FreeUnit there are two separate ideas worth separating:

### Idea 1 — Write FreeUnit C-layer extensions in Rust (like ngx-rust does for nginx)
Study `nginx-sys` FFI layer and `build.rs`/bindgen approach; apply safe/unsafe separation
patterns to `src/otel/`.

- [ ] Clone ngx-rust, study `nginx-sys` (FFI) and `build.rs` (bindgen)
- [ ] Apply safe wrapper patterns to `src/otel/`

### Idea 2 — Rust as a language runtime

**Current state:** no native Rust runtime exists.
- Go has `go/` library (`libunit-go`, 676 lines) — apps import it, it links `libnxt_unit.a`
  and speaks the Unit app-worker protocol via Unix sockets.
- Rust apps today: only path is compile to `wasm32-wasi` → run via FreeUnit wasm runtime.

**Path A — bindgen + FFI (recommended, ~2–4 weeks)**
- Run `bindgen` on `nxt_unit.h` → generate Rust FFI types
- Write safe wrappers (~500–1000 lines), mirroring `go/unit.go`, `go/port.go`,
  `go/request.go`, `go/response.go`
- Add `axum`/`hyper` adapter so users drop in `libunit-rust` like they do `libunit-go`
- Pro: reuses existing 6800-line `nxt_unit.c`, protocol already battle-tested
- Con: C linkage required (`libnxt_unit.a`), same as Go

**Path B — pure Rust protocol reimplementation (~2–3 months, high risk)**
- Reverse-engineer Unix socket framing from `nxt_unit.c` (6800 lines)
- Pro: no C dependency, fully async-native (tokio)
- Con: high risk of protocol bugs, large effort

**Recommendation:** Path A first. Path B only if `libunit-rust` gains traction and
users demand a zero-C dependency.

- [ ] Prototype `libunit-rust` via Path A (reference: `go/*.go`, `src/nxt_unit.h`)
- [ ] Decide: native `libunit-rust` vs WASM-first (WASM works today, lower barrier)

### Idea 3 — `rust` Docker variant (WASM path, no libunit-rust needed)

All trixie-based Dockerfiles already install Rust at build time (for wasmtime /
wasm-wasi-component) and then discard it. Go variants keep Go in the final image
(`FROM golang:1.24-trixie`). Same pattern applies for Rust:

Proposed `Dockerfile.rust1.x`:
- Base: `FROM rust:1-slim-trixie` (official Rust image, trixie variant)
- Add `wasm32-wasip1` target: `rustup target add wasm32-wasip1`
- Build FreeUnit with wasm + wasm-wasi-component modules (same as `Dockerfile.wasm`)
- Keep Rust toolchain in final image — users compile and serve Rust WASM in one container

Value: "compile and run Rust WASM apps with a single FreeUnit container" — no separate
build step, no external toolchain. Works today via existing wasm runtime.

- [ ] Check `rust:1-slim-trixie` exists on Docker Hub and is suitable as base
- [ ] Add `Dockerfile.rust1.x` to `pkg/docker/` and `docker.yml` matrix
- [ ] Add `rust1.x` to `build-local.sh` ALL_VARIANTS

---

## Chunked Encoding (RFC 9112) — Implemented in pre-1.35.5-i58

Branch `pre-1.35.5-i58` implements automatic chunked → Content-Length conversion
for proxy request forwarding. Key files:

- `src/nxt_h1proto.c` — buffer fix (L1149-1171) + CL injection (L2414-2475)
- `test/test_proxy_chunked.py` — 10 tests (all passing)
- `test/fake_upstream/` — Rust HTTP mock with strict CL validation

**Tests:** 10/10 passed ✅
**clang-ast:** PASSED ✅

**Pending upstream:**
- Consider making the conversion configurable (currently always-on when `r->chunked`)
- Add metrics/counter for chunked → CL conversions
- Consider adding `Transfer-Encoding` removal for HTTP/2 upstream (HTTP/2 doesn't use TE header)

---

## proxy: request buffering for chunked POST (issue #58)

Backend returns 411 when FreeUnit forwards a chunked POST with no `Content-Length`.
Workaround today: client-side buffering (`git config http.postBuffer`).

**Design questions to resolve before implementation:**

- Where does `request_buffering` live — on the `proxy` action object or `settings.http`?
  Per-action is more composable (can disable for upload routes); global is simpler but
  can't be selectively disabled.
- After buffering: does FreeUnit strip `Transfer-Encoding: chunked` and inject
  `Content-Length`, or re-encode? Must define behavior before writing the code.

**Implementation risks:**

- 🟡 Memory: `max_body_size` can be up to 17 GB. Need a per-request memory cap and
  a disk spill path — not just a flag that buffers everything in-process.

**Related upstream nginx/unit issues:** #445, #1088, #1278
