#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Build the load-time harnesses into test/wasm-bench/harness/bin/:
#
#   load-bench               C, against pkg/contrib/wasmtime/artifacts
#                            (make -C pkg/contrib .wasmtime), i.e. the same
#                            libwasmtime the "wasm" module links
#   wasm-bench-load          Rust, wasmtime crates and features as in
#                            src/wasm-wasi-component before #912 (lock
#                            copied from it)
#   wasm-bench-load-cache    + wasmtime "cache" feature
#   wasm-bench-load-par      + "cache" and "parallel-compilation"

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
W="$ROOT/pkg/contrib/wasmtime/artifacts"
BIN="$HERE/bin"

mkdir -p "$BIN"

cc -O2 -Wall -o "$BIN/load-bench" "$HERE/c/load-bench.c" \
    -I"$W/include" -L"$W/lib" -Wl,-rpath,"$W/lib" -lwasmtime

cd "$HERE/rust"
cargo build --release --locked -q
cp target/release/wasm-bench-load "$BIN/wasm-bench-load"
cargo build --release --locked -q --features cache
cp target/release/wasm-bench-load "$BIN/wasm-bench-load-cache"
cargo build --release --locked -q --features cache,parallel
cp target/release/wasm-bench-load "$BIN/wasm-bench-load-par"

ls -l "$BIN"
