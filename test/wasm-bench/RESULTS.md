# wasm-bench results

Measured on 2026-11-06 on an 8-vCPU KVM guest ("Intel(R) Xeon(R)
Processor", 1 thread per core, 15 GiB RAM, Linux 6.12), Wasmtime 48.0.8:
the C API from `pkg/contrib` (`make -C pkg/contrib .wasmtime`) and the Rust
crates from the `src/wasm-wasi-component` lock.  Guests built with Debian
clang 19.1.7 / wasi-sysroot 27 and rustc 1.99.1.  These are numbers from one
virtual machine: compare modes with each other, not with other hosts.

## Harness, per process (`harness/bench.py --runs 5`)

All times in ms, mean ± stdev (min–max) over 5 fresh processes.  "read" is
the `fopen`/`fread` (C) or `fs::read` (Rust) of the input file;
"compile/load" is `wasmtime_module_new` / `Component::from_file` (JIT,
cache) or `wasmtime_module_deserialize(_file)` /
`Component::deserialize_file` (AOT); "ready" runs from `main()` to an
instantiated module (C: `wasmtime_linker_module`) or a `ProxyPre` (Rust),
without exec and dynamic linking of the harness itself.

Modes: `jit` is what unitd does today.  `cache-cold` is the first start with
an empty Wasmtime cache directory, `cache-warm` a later start.  `aot` loads
a `.cwasm` made by `wasmtime_module_serialize` /
`Engine::precompile_component` with the same engine config.  `aot-mem` (C)
reads the `.cwasm` into memory first.  `jit-par` (Rust) turns on Wasmtime's
`parallel-compilation` feature, which `src/wasm-wasi-component` does not
enable today (the C API build has it on).  `jit-epoch` (Rust) is JIT with
epoch interruption, as with `execution_timeout`.

| path | guest | .wasm KiB | .cwasm KiB | mode | read ms | compile/load ms | ready (process) ms |
|---|---|---:|---:|---|---:|---:|---:|
| C API (wasm) | ci-hello | 248 | 106 | jit | 0.17 ± 0.02 (0.15–0.19) | 14.70 ± 1.36 (13.54–16.90) | 15.28 ± 1.36 (14.13–17.50) |
| C API (wasm) | ci-hello | 248 | 106 | cache-cold | 0.17 ± 0.01 (0.16–0.19) | 16.04 ± 0.77 (15.18–16.95) | 16.76 ± 0.79 (15.88–17.69) |
| C API (wasm) | ci-hello | 248 | 106 | cache-warm | 0.16 ± 0.02 (0.14–0.17) | 0.74 ± 0.08 (0.65–0.84) | 1.41 ± 0.08 (1.30–1.51) |
| C API (wasm) | ci-hello | 248 | 106 | aot | 0.00 ± 0.00 (0.00–0.00) | 0.11 ± 0.01 (0.10–0.13) | 0.46 ± 0.03 (0.41–0.50) |
| C API (wasm) | ci-hello | 248 | 106 | aot-mem | 0.08 ± 0.01 (0.07–0.10) | 0.15 ± 0.01 (0.14–0.17) | 0.62 ± 0.08 (0.55–0.74) |
| C API (wasm) | c-echo | 155 | 87 | jit | 0.12 ± 0.02 (0.09–0.13) | 15.30 ± 0.83 (14.08–16.20) | 15.82 ± 0.84 (14.63–16.77) |
| C API (wasm) | c-echo | 155 | 87 | cache-cold | 0.13 ± 0.03 (0.10–0.16) | 17.44 ± 1.06 (16.70–19.32) | 18.11 ± 1.06 (17.39–19.97) |
| C API (wasm) | c-echo | 155 | 87 | cache-warm | 0.09 ± 0.01 (0.08–0.11) | 0.64 ± 0.13 (0.49–0.78) | 1.19 ± 0.14 (1.02–1.36) |
| C API (wasm) | c-echo | 155 | 87 | aot | 0.00 ± 0.00 (0.00–0.00) | 0.11 ± 0.01 (0.09–0.12) | 0.47 ± 0.05 (0.42–0.54) |
| C API (wasm) | c-echo | 155 | 87 | aot-mem | 0.06 ± 0.01 (0.05–0.07) | 0.12 ± 0.01 (0.11–0.14) | 0.53 ± 0.04 (0.46–0.56) |
| C API (wasm) | c-json | 281 | 155 | jit | 0.19 ± 0.02 (0.17–0.23) | 18.81 ± 4.02 (16.03–25.90) | 19.44 ± 4.01 (16.60–26.52) |
| C API (wasm) | c-json | 281 | 155 | cache-cold | 0.18 ± 0.02 (0.15–0.21) | 19.96 ± 1.05 (18.72–20.98) | 20.69 ± 1.08 (19.42–21.75) |
| C API (wasm) | c-json | 281 | 155 | cache-warm | 0.19 ± 0.08 (0.14–0.33) | 1.18 ± 0.27 (0.93–1.56) | 1.89 ± 0.38 (1.58–2.47) |
| C API (wasm) | c-json | 281 | 155 | aot | 0.00 ± 0.00 (0.00–0.00) | 0.15 ± 0.02 (0.12–0.17) | 0.57 ± 0.08 (0.49–0.68) |
| C API (wasm) | c-json | 281 | 155 | aot-mem | 0.10 ± 0.02 (0.09–0.14) | 0.21 ± 0.04 (0.19–0.28) | 0.73 ± 0.15 (0.65–0.99) |
| Rust (wasm-wasi-component) | ci-hello-world | 96 | 272 | jit | 0.11 ± 0.02 (0.08–0.12) | 83.64 ± 7.00 (76.48–92.21) | 84.00 ± 7.02 (76.83–92.57) |
| Rust (wasm-wasi-component) | ci-hello-world | 96 | 272 | jit-epoch | 0.08 ± 0.01 (0.07–0.09) | 101.98 ± 18.23 (84.71–130.99) | 102.31 ± 18.28 (85.02–131.43) |
| Rust (wasm-wasi-component) | ci-hello-world | 96 | 272 | jit-par | 0.10 ± 0.03 (0.08–0.14) | 21.93 ± 1.56 (20.33–23.82) | 22.44 ± 1.77 (20.64–24.82) |
| Rust (wasm-wasi-component) | ci-hello-world | 96 | 272 | cache-cold | 0.08 ± 0.02 (0.07–0.11) | 81.16 ± 3.52 (78.50–85.41) | 81.79 ± 3.72 (79.02–86.53) |
| Rust (wasm-wasi-component) | ci-hello-world | 96 | 272 | cache-warm | 0.07 ± 0.01 (0.06–0.08) | 1.26 ± 0.19 (1.08–1.47) | 1.64 ± 0.22 (1.44–1.88) |
| Rust (wasm-wasi-component) | ci-hello-world | 96 | 272 | aot | 0.06 ± 0.00 (0.06–0.07) | 0.27 ± 0.06 (0.21–0.34) | 0.62 ± 0.11 (0.48–0.75) |
| Rust (wasm-wasi-component) | rust-hello | 84 | 246 | jit | 0.07 ± 0.00 (0.06–0.07) | 81.92 ± 2.96 (78.82–85.44) | 82.25 ± 2.97 (79.13–85.78) |
| Rust (wasm-wasi-component) | rust-hello | 84 | 246 | jit-epoch | 0.08 ± 0.01 (0.07–0.09) | 111.71 ± 11.96 (95.36–122.29) | 112.13 ± 12.00 (95.73–122.74) |
| Rust (wasm-wasi-component) | rust-hello | 84 | 246 | jit-par | 0.07 ± 0.01 (0.06–0.08) | 20.16 ± 1.45 (18.31–21.89) | 20.50 ± 1.46 (18.64–22.27) |
| Rust (wasm-wasi-component) | rust-hello | 84 | 246 | cache-cold | 0.07 ± 0.00 (0.07–0.07) | 85.26 ± 1.42 (83.77–87.19) | 85.79 ± 1.44 (84.25–87.77) |
| Rust (wasm-wasi-component) | rust-hello | 84 | 246 | cache-warm | 0.06 ± 0.00 (0.05–0.06) | 1.10 ± 0.05 (1.04–1.17) | 1.46 ± 0.06 (1.38–1.52) |
| Rust (wasm-wasi-component) | rust-hello | 84 | 246 | aot | 0.06 ± 0.00 (0.05–0.06) | 0.27 ± 0.08 (0.21–0.40) | 0.64 ± 0.16 (0.51–0.89) |
| Rust (wasm-wasi-component) | rust-json | 167 | 435 | jit | 0.11 ± 0.00 (0.11–0.12) | 135.52 ± 3.83 (131.30–141.15) | 135.87 ± 3.83 (131.65–141.49) |
| Rust (wasm-wasi-component) | rust-json | 167 | 435 | jit-epoch | 0.11 ± 0.00 (0.11–0.12) | 163.52 ± 2.01 (161.59–166.80) | 163.87 ± 2.02 (161.93–167.15) |
| Rust (wasm-wasi-component) | rust-json | 167 | 435 | jit-par | 0.12 ± 0.01 (0.11–0.13) | 30.81 ± 1.39 (28.88–32.60) | 31.18 ± 1.37 (29.32–32.97) |
| Rust (wasm-wasi-component) | rust-json | 167 | 435 | cache-cold | 0.11 ± 0.01 (0.10–0.12) | 145.32 ± 11.22 (136.34–163.66) | 145.81 ± 11.22 (136.82–164.15) |
| Rust (wasm-wasi-component) | rust-json | 167 | 435 | cache-warm | 0.10 ± 0.02 (0.08–0.14) | 1.86 ± 0.20 (1.64–2.17) | 2.27 ± 0.26 (1.97–2.68) |
| Rust (wasm-wasi-component) | rust-json | 167 | 435 | aot | 0.09 ± 0.00 (0.08–0.09) | 0.31 ± 0.05 (0.25–0.39) | 0.70 ± 0.13 (0.56–0.87) |
| Rust (wasm-wasi-component) | rust-image | 938 | 2308 | jit | 0.60 ± 0.02 (0.57–0.62) | 797.06 ± 20.18 (767.55–823.23) | 797.48 ± 20.21 (767.95–823.69) |
| Rust (wasm-wasi-component) | rust-image | 938 | 2308 | jit-epoch | 0.58 ± 0.06 (0.49–0.66) | 949.86 ± 7.61 (942.86–959.36) | 950.24 ± 7.62 (943.23–959.72) |
| Rust (wasm-wasi-component) | rust-image | 938 | 2308 | jit-par | 0.60 ± 0.04 (0.56–0.67) | 141.54 ± 6.81 (131.02–150.05) | 141.98 ± 6.80 (131.50–150.52) |
| Rust (wasm-wasi-component) | rust-image | 938 | 2308 | cache-cold | 0.65 ± 0.09 (0.49–0.73) | 820.67 ± 21.20 (790.59–849.91) | 821.30 ± 21.25 (791.17–850.64) |
| Rust (wasm-wasi-component) | rust-image | 938 | 2308 | cache-warm | 0.48 ± 0.13 (0.31–0.59) | 7.55 ± 1.26 (6.42–9.50) | 7.97 ± 1.29 (6.87–10.00) |
| Rust (wasm-wasi-component) | rust-image | 938 | 2308 | aot | 0.35 ± 0.07 (0.29–0.48) | 0.39 ± 0.09 (0.31–0.51) | 0.74 ± 0.13 (0.60–0.90) |

## Real unitd (`harness/unitd-timing.sh 5`)

Each guest as the only application, `"processes": {"max": 1, "spare": 0}`.
After `/control/applications/app/restart` no application process exists,
so the first request pays for fork, engine, compile and instantiation; the
second is a plain request.  curl `time_total`, ms, 5 restarts.

| guest | type | first request after restart ms (cold) | second request ms (warm) |
|---|---|---|---|
| ci-hello | wasm | 22.7 ± 1.8 (20.5–25.4) | 0.67 ± 0.27 (0.25–0.87) |
| c-echo | wasm | 21.7 ± 0.5 (20.9–22.1) | 0.46 ± 0.09 (0.33–0.55) |
| c-json | wasm | 23.7 ± 2.6 (19.8–26.3) | 0.62 ± 0.19 (0.35–0.82) |
| ci-hello-world | wasm-wasi-component | 78.3 ± 8.2 (72.8–92.8) | 1.11 ± 0.18 (0.94–1.35) |
| rust-hello | wasm-wasi-component | 80.9 ± 4.6 (76.6–86.4) | 0.83 ± 0.22 (0.63–1.19) |
| rust-json | wasm-wasi-component | 143.6 ± 5.7 (136.0–149.6) | 1.19 ± 0.32 (0.84–1.59) |
| rust-image | wasm-wasi-component | 782.9 ± 30.6 (750.3–831.4) | 7.39 ± 0.25 (7.11–7.68) |

The cold first request matches the harness "jit" column plus a few ms of
process start, so the harness measures what an application process pays.
