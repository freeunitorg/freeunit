# TODO

---

## Release roadmap

| Milestone | Due | Focus |
|-----------|-----|-------|
| **1.35.5** | current branch | Docker builder mode, parallel builds, Go 1.26, Node 24 |
| **1.35.6** | 2026-06-25 | OTEL 0.24→0.32 (#65), OTLP/gRPC re-intro (config-selected `protocol`), Rust DX (`rust1.x` variant, `libunit-rust`) |
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
| otel local image smoke-test: close doc/tag gaps (hardcoded 1.35.5, php-base snippet without `--otel`) | 1.35.6 |

**July (after 1.35.6):**

| Task | Milestone |
|------|-----------|
| `libunit-rust` SDK — bindgen + FFI + axum adapter (prototype) | 1.35.7 |
| Evaluate `libunit-rust` prototype → decide: WASM-first or native-first | 1.35.7 |
| `packages.freeunit.org` — GoAccess / JSON stats for download counter | 1.35.7 |
| `fake_upstream` prebuilt binary on `packages.freeunit.org` | 1.35.7 |
| OTEL new config fields (`service_name`, `headers`, `root_certificate`, `resource_attributes`, `max_queue_size`, `export_timeout`) | 1.35.7 |
| Rust toolchain bump: 1.94.1 → current stable (1.96); re-run otel clang-ast build + `test_otel.py`; decide pin-vs-floating for `rust1.x` image | 1.35.7 |

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
- [ ] Add `rust1.x` to `release-docker.yml` CI matrix
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

## OpenTelemetry — issue #65

Crate upgrade 0.24 → 0.32 **completed**. Current crates: all 0.32 (see
`src/otel/Cargo.toml`). What remains: configurable fields, semconv attributes,
application-aware spans, test expansion.

Original implementation by Ava Hahn ([@ava-affine](https://github.com/ava-affine)).
The upstream `nginx/nginx-otel` is a separate C++ module, unrelated to our Rust crate.

### Done in 1.35.6 ✅

- [x] Bump all `opentelemetry*` crates to 0.32
- [x] Rewrite `nxt_otel_rs_init()` — old `new_pipeline()` API gone in 0.27
- [x] Dedicated-thread `BatchSpanProcessor` + blocking reqwest (no tokio runtime)
- [x] `ParentBased(TraceIdRatioBased(...))` sampler — respects upstream sampling
- [x] Rename `"NGINX Unit"` → `"FreeUnit"` in service name
- [x] Replace `eprintln!` → `nxt_otel_log_cb` FFI callback
- [x] Remove dead `Protocol::HttpJson` arm (never reached)
- [x] `batch_size` validation bounds (1–65536) in `nxt_conf_validation.c`
- [x] `sampling_ratio` validation bounds (0–1)
- [x] `protocol` enum — `"http"` + `"grpc"`, both valid on any `--otel` build;
      transport chosen at runtime by config (upstream behaviour)
- [x] OTLP/gRPC export re-introduced — both transports compiled into every
      `--otel` build (no separate flag); `settings/telemetry/protocol` selects
      `http` (blocking reqwest) or `grpc` (tonic over a small owned tokio
      runtime); v1 plaintext h2c only
- [x] `fake_otlp` — Rust mock OTLP collector (`test/fake_otlp/`) speaking both
      HTTP and gRPC in one build; HTTP path hardened (`POST /v1/traces` +
      `application/x-protobuf` + non-empty body, else 400)
- [x] `test_otel.py` — span export, traceparent, sampling, config validation;
      export cases parametrized over `["http","grpc"]`

### Competitive gap analysis (2026-05-31)

**Sources:** `nginx/nginx-otel` (C++, gRPC-only, 10 tests), Caddy `tracing` module
(Go, gRPC, 9 unit tests + metrics). Angie = nginx-otel fork (not on GitHub, no
unique tests). FreeUnit has the only std-only mock collector + sampling-zero test.

#### Span attributes comparison

| Attribute | nginx-otel | Caddy | FreeUnit (current) |
|-----------|-----------|-------|-------------------|
| HTTP method | ✅ `http.method` | ✅ auto (otelhttp) | ❌ `"method"` (not semconv) |
| URL/path | ✅ `http.target` | ✅ auto | ❌ `"path"` (not semconv) |
| Status code | ✅ `http.status_code` | ✅ auto | ❌ `"status"` (not semconv) |
| HTTP scheme | ✅ `http.scheme` | ✅ auto | — |
| HTTP flavor | ✅ `http.flavor` | ✅ auto | — |
| User agent | ✅ `http.user_agent` | ✅ auto | — |
| Request body size | ✅ `http.request_content_length` | — | ❌ `"body size"` |
| Response body size | ✅ `http.response_content_length` | — | — |
| Server name | ✅ `net.host.name` | — | — |
| Client address | ✅ `net.sock.peer.addr` | — | — |
| Error on 5xx | ✅ `span.setError()` | — | — |
| **Application name** | — | — | — |
| **Application type** | — | — | — |

**FreeUnit differentiator — application-aware spans.** Unlike nginx/Caddy (reverse
proxies), FreeUnit is an **app server**: it knows which application handled the
request. When multiple PHP versions or Go+Python apps run side-by-side, OTel spans
must identify the target:

| Attribute | Source | Example |
|-----------|--------|---------|
| `unit.application.name` | `r->app_name` from router | `"wordpress"`, `"api-v2"` |
| `unit.application.type` | language module (php/python/go/...) | `"php"`, `"python"`, `"go"` |
| `unit.application.processes` | app conf | `"5"` |

These are **FreeUnit-specific** — no competitor has them. Every `add_event` call
in `nxt_otel.c` should include these alongside the standard semconv attributes.

#### Test coverage comparison

| Test case | nginx-otel | Caddy | FreeUnit |
|-----------|-----------|-------|---------|
| Span exported | ✅ | ✅ | ✅ |
| traceparent inject | ✅ (4 modes) | ✅ | ✅ |
| traceparent inherit | ✅ | ✅ | ✅ |
| Sampling zero | — | — | ✅ **unique** |
| $otel_trace_id variable | ✅ | ✅ | — |
| Custom span attributes | ✅ | ✅ | — |
| Custom resource attributes | ✅ | ✅ | — |
| Exporter headers (auth) | ✅ | ✅ | — |
| TLS export | ✅ | ✅ | — |
| Trace off | ✅ | — | — |
| Batching | ✅ | — | — |
| HTTP/2.0, 3.0 | ✅ | — | — |

#### Config features comparison

| Feature | nginx-otel | Caddy | FreeUnit (current) |
|---------|-----------|-------|-------------------|
| Transport | gRPC only | gRPC (autoexport) | HTTP only |
| Trace context modes | ignore/extract/inject/propagate | auto (autoprop) | inject only |
| $otel_trace_id etc. | ✅ 4 variables | ✅ 2 placeholders | — |
| Custom span name | ✅ `otel_span_name` | ✅ `span` directive | — |
| Custom span attrs | ✅ `otel_span_attr` | ✅ `span_attributes` | — |
| Resource attributes | ✅ `otel_resource_attr` | ✅ (env + semconv) | `"FreeUnit"` hardcoded |
| Exporter headers | ✅ `header` directive | ✅ `OTEL_EXPORTER_OTLP_HEADERS` | — |
| TLS to collector | ✅ `trusted_certificate` | ✅ (env) | — |
| OTEL_* env vars | — | ✅ | — |
| Metrics | ❌ | ✅ (separate subsystem) | ❌ |

### Remaining work — per milestone

**1.35.6 (current, close before release):**
- [ ] Fix span attributes to use OTel semconv: `"method"` → `http.request.method`,
      `"path"` → `url.path`, `"status"` → `http.response.status_code`
- [ ] Add application-aware attributes: `unit.application.name`, `unit.application.type`
      from `r->app_name` / language module in `nxt_otel.c`
- [ ] Add missing standard attributes: `http.scheme`, `http.flavor`, `http.user_agent`,
      `server.address`, `client.address`
- [ ] Pass `tracestate` through to Rust OTEL SDK (parsed in C at `nxt_otel.c:376`,
      stored in `r->otel->trace_state`, but never forwarded to Rust)
- [ ] Add `setError()` equivalent on HTTP 5xx responses
- [x] Harden `fake_otlp` request validation (PLAN.md WS-B): validate `POST /v1/traces`,
      `Content-Type: application/x-protobuf`, reject empty bodies

**1.35.7 (July):**
- [ ] New config fields: `service_name`, `headers`, `root_certificate`,
      `resource_attributes`, `max_queue_size`, `export_timeout`
- [ ] `$otel_trace_id`, `$otel_span_id`, `$otel_parent_id` variables (access log +
      response headers — nginx-otel has 4 variables, Caddy has 2 placeholders)
- [ ] Custom span attributes directive (per-route, like nginx-otel `otel_span_attr`)
- [ ] Evaluate `OTEL_*` env vars as fallback/override for `settings.telemetry`
      (Caddy pattern — interop with existing OTel deployments)
- [ ] Expand `test_otel.py`: custom span attrs, resource attrs, auth headers,
      trace context modes

**1.35.8 (August):**
- [x] gRPC transport re-introduction — landed early in 1.35.6, config-selected
      `protocol` in every `--otel` build (see "Done in 1.35.6")
- [ ] TLS to collector (`root_certificate` + reqwest rustls/native-tls; gRPC v1
      is plaintext h2c only)
- [ ] Metrics exploration (Caddy has full HTTP metrics via OTLP; nginx-otel has none)

### Span leak on aborted requests (pre-existing)

`nxt_otel_rs_get_or_create_trace()` returns `Box::into_raw(...)` into
`r->otel->trace`. The span is reclaimed only via `NXT_OTEL_COLLECT_STATE`
(`nxt_otel_span_collect`). `nxt_otel_request_error_path()` correctly sends trace
through COLLECT for error cases, but if the request is torn down between INIT and
COLLECT without hitting the error path, `r->otel->trace` leaks.

- [ ] Add a teardown path that calls `nxt_otel_rs_send_trace()` when
      `r->otel->trace` is non-null and COLLECT was not reached.

### gRPC re-introduction notes — DONE (config-selected, no build flag)

Shipped in 1.35.6. Both transports compiled into every `--otel` build;
`settings/telemetry/protocol` picks `http`/`grpc` at runtime — same UX as
upstream Unit (which the public site already documents). Key decisions:
- tokio already linked transitively (reqwest → hyper → tokio 1.52.3), so the
  delta is tonic + `grpc-tonic` + a small runtime this crate owns
- Precedent: `wasm-wasi-component` runs managed tokio runtime on dedicated thread
- Considered an opt-in `--otel-grpc` Cargo feature but rejected it: it changed
  the historical config-only UX and split the build matrix. Always-on keeps one
  binary, one config contract.
- v1 is plaintext h2c only — no TLS to collector (tracked under 1.35.8 TLS item)
- See `test/fake_otlp/PLAN.md` for the gRPC test infrastructure (all boxes ticked)

### Competitive references

- **nginx/nginx-otel** (C++, gRPC-only, port 4317, TLS since 0.1.2):
  https://github.com/nginx/nginx-otel — docs: https://nginx.org/en/docs/ngx_otel_module.html
- **NGINX OTel admin guide**: https://docs.nginx.com/nginx/admin-guide/dynamic-modules/opentelemetry/
- **Angie OTel module** (gRPC-only, nginx-otel fork): https://en.angie.software/angie/docs/installation/external-modules/otel/
- **Caddy `tracing`** (opentelemetry-go, gRPC for traces, OTEL_* env config):
  https://caddyserver.com/docs/caddyfile/directives/tracing
- **Caddy #5743** — request to add OTLP/HTTP for traces: https://github.com/caddyserver/caddy/issues/5743
- **FrankenPHP #1715** — Caddy(gRPC) vs PHP(HTTP) env var conflict: https://github.com/php/frankenphp/issues/1715
- **OTLP spec** (4317 gRPC / 4318 HTTP; default SHOULD be http/protobuf): https://opentelemetry.io/docs/specs/otlp/
- **OTLP exporter config** (per-SDK defaults; Go=grpc, Node=http/protobuf): https://opentelemetry.io/docs/specs/otel/protocol/exporter/
- **opentelemetry-php** (HTTP/protobuf, port 4318): https://github.com/open-telemetry/opentelemetry-php

In-repo audit (2026-05-31):
- tokio in otel staticlib via reqwest-blocking → hyper/tower (`src/otel/Cargo.lock`)
- managed tokio runtime precedent: `src/wasm-wasi-component/src/lib.rs`
- gRPC origin: upstream commit `8b697101` "otel: add opentelemetry rust crate code" (http+grpc)
- Rust toolchain: pinned 1.94.1, Rust 1.96 just released (6-week cadence; memory-safe,
  no GC, runtime limited to std-lib init — fits embedded staticlib). The floating
  `rust:1-slim-trixie` base for `rust1.x` variant now tracks 1.95→1.96. Bump
  pinned toolchain → current stable, re-run otel clang-ast + `test_otel.py` to
  confirm 0.32 crates compile clean on newer Rust. Decide pin-vs-floating policy
  for `rust1.x` image (floating drifts; pin for reproducible CI).

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
- [ ] Run the full CI matrix (`build-test.yml`) and confirm the new "Build OpenSSL 3.6"
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

### otel coverage in local image smoke-tests — already exists; close the doc/tag gaps (milestone 1.35.6)

**Scheduled for the 1.35.6 release** — close the doc/tag gaps below as part of
the same milestone as the OTel 0.24→0.32 upgrade (#65).

otel runtime-build coverage is already in place in two paths; the gaps are
documentation and a hardcoded version, not missing infrastructure:

- `test/run-local.sh` test image (`FROM python:3.14-slim-trixie` + rustup-pinned
  toolchain) configures `--otel` and builds `fake_otlp`, so the pytest path
  exercises the otel staticlib + `test_otel.py`.
- `pkg/docker/local/Dockerfile.{minimal,php8.5,wasm}` (run via
  `pkg/docker/build-local.sh -b minimal`) build
  **`FROM ghcr.io/freeunitorg/freeunit-builder:trixie-rust1.94.1`** — a pre-built
  builder image with Rust already baked in — and **already configure `--otel`**
  (`--njs --otel --zlib --zstd --brotli`). **NOTE: `pkg/docker/local/` is
  experimental — a local build-speedup only.** It depends on the builder image
  being built first (no apt/rustup at build time), is not wired into CI, and is
  not the canonical/release image set (that is `pkg/docker/Dockerfile.*`). Treat
  it as a fast dev loop, not the documented smoke-test of record.

Remaining gaps:
- [ ] `pkg/docker/local/Dockerfile.*` hardcode `git clone -b 1.35.5` and
      `LABEL ... version="1.35.5"` — bump on each release (for local branch
      testing, override the clone ref per the CLAUDE.md note).
- [ ] Decide the fate of the experimental `pkg/docker/local/` set: promote it to
      a documented/CI fast-path, or keep it as a personal dev shortcut. Until
      then, do not point users' "rebuild Docker images locally" flow at it as the
      official smoke-test.
- [ ] The CLAUDE.md "rebuild Docker images locally" snippet documents a
      `php:8.5-cli-trixie` build **without `--otel`** — at minimum mention the
      `--otel` flag there so the documented smoke-test can cover otel (it needs a
      Rust toolchain in that image, which the php base lacks).
- [ ] Once the `rust1.x` variant lands its `rust:1-slim-trixie` base, fold the
      builder-image and `rust:1-slim-trixie` approaches into one documented story
      (both already ship Rust; no rustup step needed).

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
- [ ] Add `Dockerfile.rust1.x` to `pkg/docker/` and `release-docker.yml` matrix
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
