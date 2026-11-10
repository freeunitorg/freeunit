# wasm-bench harnesses

Standalone programs that replay how the two FreeUnit wasm modules get a
module ready, against the same Wasmtime 48.0.8, and time each step.

* `c/load-bench.c` — `nxt_wasmtime_init()`: `wasm_engine_new()`, store,
  linker with WASI, `fopen`/`fread`, `wasmtime_module_new()`,
  `wasmtime_linker_module()`.  Modes: `jit`, `cache`, `precompile`, `aot`,
  `aot-mem`.
* `rust/` — `GlobalState::new()`: `Config` with the component model (and
  epoch interruption with `--epoch`), `Engine::new()`,
  `Component::from_file()`, linker with WASI p2 and wasi:http,
  `instantiate_pre()`, `ProxyPre::new()`.  Modes: `jit`, `cache`
  (`--features cache`), `precompile`, `aot`.
* `bench.py` — runs every mode for every guest in `../build/` N times.
* `unitd-smoke.sh`, `unitd-timing.sh` — the same guests in a real unitd.

`build.sh` builds the harness binaries into `bin/`.  See
[../README.md](../README.md) for the full procedure and the results.
