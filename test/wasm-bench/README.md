# wasm-bench: how long a wasm application takes to get ready

FreeUnit compiles every wasm module or component with Cranelift each time
an application process starts (JIT): `wasmtime_module_new()` in
`src/wasm/nxt_rt_wasmtime.c` and `Component::from_file()` in
`src/wasm-wasi-component/src/lib.rs`.  The Wasmtime cache is built into the
C API but not configured, and nothing loads precompiled `.cwasm` files.

This directory compares three ways to get a module ready, to decide what a
development mode and a production mode should use:

| mode | how | what it costs |
|---|---|---|
| JIT | compile on every process start (today) | nothing to manage; slowest start |
| cache | Wasmtime cache: the first start compiles and stores, later starts load | a cache directory the app user can write; cold start unchanged |
| AOT | `.cwasm` precompiled ahead, loaded with deserialize | a build step; the artifact is native code tied to the Wasmtime version, engine config and CPU |

Nothing here is used by the test suite or by unitd.

## Contents

| path | what |
|---|---|
| [`c-echo/`](c-echo/) | C API guest: echoes the request (from libunit-wasm's example, Apache-2.0) |
| [`c-json/`](c-json/) | C API guest: JSON transform with cJSON (MIT, downloaded at build) |
| [`rust-hello/`](rust-hello/) | WASI 0.2 component: fixed hello answer |
| [`rust-json/`](rust-json/) | WASI 0.2 component: JSON transform and URL-shortener codes with serde_json |
| [`rust-image/`](rust-image/) | WASI 0.2 component: image thumbnails with the `image` crate |
| [`build.sh`](build.sh) | builds the guests (and the two CI test guests) into `build/` |
| [`harness/c/load-bench.c`](harness/c/load-bench.c) | replays `nxt_wasmtime_init()`: JIT, cache, precompile, AOT |
| [`harness/rust/`](harness/rust/) | replays `GlobalState::new()`: JIT, cache, precompile, AOT |
| [`harness/build.sh`](harness/build.sh) | builds the harnesses into `harness/bin/` |
| [`harness/bench.py`](harness/bench.py) | runs the matrix, prints a Markdown table |
| [`harness/unitd-smoke.sh`](harness/unitd-smoke.sh) | loads every guest into this tree's unitd and sends requests |
| [`harness/unitd-timing.sh`](harness/unitd-timing.sh) | cold/warm first request after an application restart in unitd |
| [`RESULTS.md`](RESULTS.md) | full numbers from one run |

The harnesses link the same Wasmtime as the modules: the C one against
`pkg/contrib/wasmtime/artifacts` (48.0.8, same cmake flags), the Rust one
with a copy of `src/wasm-wasi-component/Cargo.lock` and the same wasmtime
crate features.  The cache and parallel-compilation variants only add
features.

## Running

```sh
# Wasmtime C API, wasi-sysroot and libunit-wasm, as in CI
make -C pkg/contrib .wasmtime .wasi-sysroot .libunit-wasm
# guest toolchains: clang, lld, libclang-rt-<ver>-dev-wasm32; Rust targets
rustup target add wasm32-wasip2 wasm32-wasip1

test/wasm-bench/build.sh              # guests -> test/wasm-bench/build/
test/wasm-bench/harness/build.sh      # harnesses -> harness/bin/
test/wasm-bench/harness/bench.py --runs 5

# Against a real unitd built in this tree:
./configure --tests
./configure wasm --rpath --include-path=pkg/contrib/wasmtime/artifacts/include \
    --lib-path=pkg/contrib/wasmtime/artifacts/lib
./configure wasm-wasi-component
make && make wasm wasm-wasi-component
test/wasm-bench/harness/unitd-smoke.sh
test/wasm-bench/harness/unitd-timing.sh 5
```

The harnesses take one module per run, for example:

```sh
B=test/wasm-bench/build; H=test/wasm-bench/harness/bin
$H/load-bench jit $B/c-json.wasm
$H/load-bench precompile $B/c-json.wasm /tmp/c-json.cwasm
$H/load-bench aot $B/c-json.wasm /tmp/c-json.cwasm
printf '[cache]\ndirectory = "/tmp/wasm-cache"\n' > /tmp/cache.toml
$H/load-bench cache $B/c-json.wasm '' /tmp/cache.toml

$H/wasm-bench-load jit $B/rust-image.wasm [--epoch]
$H/wasm-bench-load precompile $B/rust-image.wasm --cwasm /tmp/rust-image.cwasm
$H/wasm-bench-load aot $B/rust-image.wasm --cwasm /tmp/rust-image.cwasm
$H/wasm-bench-load-cache cache $B/rust-image.wasm --cache-config /tmp/cache.toml
```

## Results in short

ms from process start to a ready module, mean of 5 runs, one 8-vCPU VM;
see [RESULTS.md](RESULTS.md) for spread, read times and the unitd check.
`ci-hello` and `ci-hello-world` are the guests of `test_wasm.py` and
`test_wasm-wasi-component.py`.

| guest | path | .wasm KiB | .cwasm KiB | JIT (today) | JIT, parallel compile | cache, 1st start | cache, next starts | AOT .cwasm |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| ci-hello | wasm (C API) | 248 | 106 | 15.28 | (already on) | 16.76 | 1.41 | 0.46 |
| c-echo | wasm (C API) | 155 | 87 | 15.82 | (already on) | 18.11 | 1.19 | 0.47 |
| c-json | wasm (C API) | 281 | 155 | 19.44 | (already on) | 20.69 | 1.89 | 0.57 |
| ci-hello-world | wasm-wasi-component | 96 | 272 | 84.00 | 22.44 | 81.79 | 1.64 | 0.62 |
| rust-hello | wasm-wasi-component | 84 | 246 | 82.25 | 20.50 | 85.79 | 1.46 | 0.64 |
| rust-json | wasm-wasi-component | 167 | 435 | 135.87 | 31.18 | 145.81 | 2.27 | 0.70 |
| rust-image | wasm-wasi-component | 938 | 2308 | 797.48 | 141.98 | 821.30 | 7.97 | 0.74 |

* JIT cost grows with code size: 15–20 ms for the C guests, 80–800 ms for
  the components.  A real unitd shows the same on the first request after a
  restart (22–24 ms C, 78–783 ms components).
* The Rust module compiles on one core: Wasmtime's `parallel-compilation`
  feature is off in `src/wasm-wasi-component`, and turning it on cuts JIT
  3.5–5.5x on this machine.  Epoch interruption (`execution_timeout`) adds
  20–35 % to JIT.
* A warm cache makes a start 10–100x faster (1–8 ms) but the first start
  per module stays at JIT cost (slightly more: it also writes the cache).
* AOT loads in under 1 ms for every guest, independent of size, because
  `deserialize_file` maps the code.  `.cwasm` files are 0.4–0.6x the
  `.wasm` for C (the `.wasm` carries DWARF) and 2.5–3x for components.

## Recommendation

* **Development: JIT**, as today, plus parallel compilation for the Rust
  module.  Edit, restart, and the new module is compiled; nothing to keep in
  sync.
* **Production: AOT `.cwasm`**, with the cache as the cheap middle step.
  The cache needs no build step and only a writable directory; AOT also
  removes the first-start compile and is the only mode with a flat start
  time.  A hybrid fits both: load a `.cwasm` when configured, fall back to
  JIT (with the cache if configured).

Caveats:

* A `.cwasm` is native code for one Wasmtime version, one engine
  configuration (epoch interruption, features, Cranelift flags) and one CPU
  architecture and feature set.  Wasmtime rejects a mismatch on load, so
  every Wasmtime bump and every change of those settings needs the artifacts
  rebuilt.
* Deserializing is `unsafe` in Wasmtime: a `.cwasm` is trusted like a
  shared library.  It must come from the operator, never from users, and
  live where only the operator can write.
* The cache directory must be writable by the application's user (the
  `user` an application runs as, not the user of the main process), on
  local storage, and is keyed by Wasmtime version and settings, so an
  upgrade starts it cold again.  Several processes starting at once may all
  compile before the first one stores the result.
