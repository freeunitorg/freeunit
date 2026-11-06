#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Build the wasm-bench guests into test/wasm-bench/build/.
#
#   C modules (type "wasm"): clang --target=wasm32-wasi with the wasi-sysroot
#   and libunit-wasm from pkg/contrib, the same flags as
#   test/unit/applications/lang/wasm.py.  Run first:
#       make -C pkg/contrib .wasi-sysroot .libunit-wasm
#   and install clang, lld and libclang-rt-<ver>-dev-wasm32.
#
#   Rust components (type "wasm-wasi-component"): cargo build --release
#   --target wasm32-wasip2, which links a component directly (no
#   cargo-component).  Run first:  rustup target add wasm32-wasip2
#
# Usage: test/wasm-bench/build.sh [c|rust|ci|all]

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$HERE/build"
WHAT=${1:-all}

SYSROOT="$ROOT/pkg/contrib/wasi-sysroot"
LUW="$ROOT/pkg/contrib/libunit-wasm/src/c"

CJSON_VERSION=1.7.18
CJSON_SHA256=3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5
CJSON_URL="https://github.com/DaveGamble/cJSON/archive/refs/tags/v$CJSON_VERSION.tar.gz"

mkdir -p "$OUT"

cc_wasm() {
    out=$1; shift
    clang -I"$LUW/include" --sysroot="$SYSROOT" --target=wasm32-wasi \
        -O2 -g -std=gnu11 -fno-common \
        -Wl,--no-entry,--export=__heap_base,--export=__data_end,--export=malloc,--export=free,--stack-first,-z,stack-size=8388608 \
        -mexec-model=reactor --rtlib=compiler-rt \
        "$@" "$LUW/libunit-wasm.c" -o "$out"
}

fetch_cjson() {
    dir="$OUT/cJSON-$CJSON_VERSION"
    [ -f "$dir/cJSON.c" ] && return 0
    tgz="$OUT/cJSON-$CJSON_VERSION.tar.gz"
    curl -fsSL -o "$tgz" "$CJSON_URL"
    echo "$CJSON_SHA256  $tgz" | sha256sum -c - >/dev/null
    tar -xzf "$tgz" -C "$OUT"
}

build_c() {
    [ -d "$SYSROOT" ] && [ -f "$LUW/libunit-wasm.c" ] || {
        echo "missing pkg/contrib wasi-sysroot/libunit-wasm" >&2; exit 1; }

    cc_wasm "$OUT/c-echo.wasm" "$HERE/c-echo/c-echo.c"

    fetch_cjson
    cc_wasm "$OUT/c-json.wasm" -I"$OUT/cJSON-$CJSON_VERSION" \
        "$HERE/c-json/c-json.c" "$OUT/cJSON-$CJSON_VERSION/cJSON.c"
}

# The two guests the CI suites load, for comparison: test/wasm/hello
# (test_wasm.py) and test/wasm_component/hello_world
# (test_wasm-wasi-component.py, needs cargo-component).
build_ci() {
    cc_wasm "$OUT/ci-hello.wasm" "$ROOT/test/wasm/hello/hello.c"

    if cargo component --help >/dev/null 2>&1; then
        tmp=$(mktemp -d)
        cp -r "$ROOT/test/wasm_component/hello_world" "$tmp/"
        (cd "$tmp/hello_world" && cargo component build --release -q)
        cp "$tmp/hello_world/target/wasm32-wasip1/release/test_wasi_component.wasm" \
            "$OUT/ci-hello-world.wasm"
        rm -rf "$tmp"
    else
        echo "cargo-component not found: skipping ci-hello-world" >&2
    fi
}

build_rust() {
    for n in rust-hello rust-json rust-image; do
        (cd "$HERE/$n" && cargo build --release --locked --target wasm32-wasip2 -q)
        cp "$HERE/$n/target/wasm32-wasip2/release/wasm_bench_$(echo "$n" | tr - _).wasm" \
            "$OUT/$n.wasm"
    done
}

case $WHAT in
    c) build_c ;;
    rust) build_rust ;;
    ci) build_ci ;;
    all) build_c; build_rust; build_ci ;;
    *) echo "usage: $0 [c|rust|ci|all]" >&2; exit 2 ;;
esac

ls -l "$OUT"/*.wasm
