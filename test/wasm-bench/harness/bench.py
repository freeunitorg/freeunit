#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the wasm-bench load-time matrix and print a Markdown table.

Every cell is a fresh process (the harness), so the numbers include what a
FreeUnit application process pays when it starts: engine creation, reading
the file, compiling or loading the code, and linking/instantiation.

Modes, for each guest:
  jit          what unitd did when these numbers were taken (before #912
               for the component module; the C API always had parallel
               compilation on).  Used as the baseline mode here.
  cache-cold   Wasmtime cache on, cache directory empty (first start)
  cache-warm   Wasmtime cache on, directory filled by the previous start
  aot          precompiled .cwasm, deserialize_file
  aot-mem      (C only) fopen/fread the .cwasm, wasmtime_module_deserialize
  jit-par      (Rust only) jit with wasmtime's parallel-compilation feature,
               which src/wasm-wasi-component did not enable when the numbers
               were measured; #912 turned it on later
  jit-epoch    (Rust only) jit with epoch interruption, as with
               "execution_timeout"

Usage: bench.py [--runs N] [--out FILE.md]
Expects test/wasm-bench/build/*.wasm (../build.sh) and harness/bin/
(harness/build.sh).
"""

import argparse
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / 'build'

BIN = HERE / 'bin'
C_BIN = BIN / 'load-bench'
RUST_BIN = {
    'default': BIN / 'wasm-bench-load',
    'cache': BIN / 'wasm-bench-load-cache',
    'par': BIN / 'wasm-bench-load-par',
}

C_GUESTS = ['ci-hello', 'c-echo', 'c-json']
RUST_GUESTS = ['ci-hello-world', 'rust-hello', 'rust-json', 'rust-image']


def parse(line):
    return {k: (float(v) if k != 'mode' else v)
            for k, v in (kv.split('=') for kv in line.split())}


def run(cmd):
    out = subprocess.run(cmd, check=True, capture_output=True, text=True)
    return parse(out.stdout.strip().splitlines()[-1])


def stats(xs):
    m = statistics.mean(xs)
    sd = statistics.stdev(xs) if len(xs) > 1 else 0.0
    return f'{m:.2f} ± {sd:.2f} ({min(xs):.2f}–{max(xs):.2f})'


def bench_c(name, runs, tmp):
    wasm = BUILD / f'{name}.wasm'
    cwasm = tmp / f'{name}.c.cwasm'
    toml = tmp / 'cache.toml'
    run([C_BIN, 'precompile', wasm, cwasm])
    rows = {}
    for mode in ['jit', 'cache-cold', 'cache-warm', 'aot', 'aot-mem']:
        res = []
        for _ in range(runs):
            if mode.startswith('cache'):
                cdir = tmp / 'cache'
                if mode == 'cache-cold' or not cdir.exists():
                    shutil.rmtree(cdir, ignore_errors=True)
                    cdir.mkdir()
                    toml.write_text(f'[cache]\ndirectory = "{cdir}"\n')
                    if mode == 'cache-warm':
                        run([C_BIN, 'cache', wasm, '', toml])
                res.append(run([C_BIN, 'cache', wasm, '', toml]))
            else:
                res.append(run([C_BIN, mode, wasm, cwasm]))
        rows[mode] = res
    return rows, cwasm.stat().st_size


def bench_rust(name, runs, tmp):
    wasm = BUILD / f'{name}.wasm'
    cwasm = tmp / f'{name}.r.cwasm'
    toml = tmp / 'cache.toml'
    d, c, p = RUST_BIN['default'], RUST_BIN['cache'], RUST_BIN['par']
    run([d, 'precompile', wasm, '--cwasm', cwasm])
    rows = {}
    for mode in ['jit', 'jit-epoch', 'jit-par', 'cache-cold', 'cache-warm',
                 'aot']:
        res = []
        for _ in range(runs):
            if mode.startswith('cache'):
                cdir = tmp / 'cache'
                if mode == 'cache-cold' or not cdir.exists():
                    shutil.rmtree(cdir, ignore_errors=True)
                    cdir.mkdir()
                    toml.write_text(f'[cache]\ndirectory = "{cdir}"\n')
                    if mode == 'cache-warm':
                        run([c, 'cache', wasm, '--cache-config', toml])
                res.append(run([c, 'cache', wasm, '--cache-config', toml]))
            elif mode == 'jit-epoch':
                res.append(run([d, 'jit', wasm, '--epoch']))
            elif mode == 'jit-par':
                res.append(run([p, 'jit', wasm]))
            else:
                res.append(run([d, mode, wasm, '--cwasm', cwasm]))
        rows[mode] = res
    return rows, cwasm.stat().st_size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--out')
    args = ap.parse_args()

    lines = [
        '| path | guest | .wasm KiB | .cwasm KiB | mode | read ms | '
        'compile/load ms | ready (process) ms |',
        '|---|---|---:|---:|---|---:|---:|---:|',
    ]

    with tempfile.TemporaryDirectory(prefix='wasm-bench-') as t:
        tmp = Path(t)
        for path, guests, fn in [('C API (wasm)', C_GUESTS, bench_c),
                                 ('Rust (wasm-wasi-component)', RUST_GUESTS,
                                  bench_rust)]:
            for g in guests:
                if not (BUILD / f'{g}.wasm').exists():
                    print(f'skip {g}: no build/{g}.wasm', file=sys.stderr)
                    continue
                rows, csize = fn(g, args.runs, tmp)
                size = (BUILD / f'{g}.wasm').stat().st_size
                for mode, res in rows.items():
                    lines.append(
                        f'| {path} | {g} | {size / 1024:.0f} | '
                        f'{csize / 1024:.0f} | {mode} | '
                        f'{stats([r["read"] for r in res])} | '
                        f'{stats([r["load"] for r in res])} | '
                        f'{stats([r["main_total"] for r in res])} |')
                print(f'done {g}', file=sys.stderr)

    table = '\n'.join(lines) + '\n'
    print(table)
    if args.out:
        Path(args.out).write_text(table)


if __name__ == '__main__':
    main()
