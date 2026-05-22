# TODO

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
