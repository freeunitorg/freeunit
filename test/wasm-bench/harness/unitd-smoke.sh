#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Start the unitd from this tree's build/, load every wasm-bench guest as an
# application, send one request to each and print status and body.  Then
# print the gap between "application started" style log lines for each app
# process, to compare with the harness numbers.
#
# Needs ./configure --tests; ./configure wasm ...; ./configure
# wasm-wasi-component; make; make wasm wasm-wasi-component (see README.md),
# and test/wasm-bench/build.sh.  Runs as the current user; nothing is
# installed.  Port 8400-8404 on 127.0.0.1.

set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B="$HERE/build"
T=$(mktemp -d /tmp/wasm-bench-unitd.XXXXXX)
SOCK="$T/control.sock"

cleanup() {
    [ -f "$T/unit.pid" ] && kill "$(cat "$T/unit.pid")" 2>/dev/null || true
    sleep 0.5
    [ -n "${KEEP:-}" ] || rm -rf "$T"
}
trap cleanup EXIT

# configure wasm --rpath records pkg/contrib/wasmtime/artifacts/lib as a
# relative RUNPATH; make it independent of the working directory.
LD_LIBRARY_PATH="$ROOT/pkg/contrib/wasmtime/artifacts/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH

"$ROOT/build/sbin/unitd" --control "unix:$SOCK" --pid "$T/unit.pid" \
    --log "$T/unit.log" --modulesdir "$ROOT/build/lib/unit/modules" \
    --statedir "$T" --tmpdir "$T"

for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.1; done

luw='"request_handler": "luw_request_handler", "malloc_handler": "luw_malloc_handler", "free_handler": "luw_free_handler", "module_init_handler": "luw_module_init_handler", "module_end_handler": "luw_module_end_handler"'

curl -sS --unix-socket "$SOCK" -X PUT -d @- http://localhost/config <<JSON
{
  "listeners": {
    "127.0.0.1:8400": { "pass": "applications/c-echo" },
    "127.0.0.1:8401": { "pass": "applications/c-json" },
    "127.0.0.1:8402": { "pass": "applications/rust-hello" },
    "127.0.0.1:8403": { "pass": "applications/rust-json" },
    "127.0.0.1:8404": { "pass": "applications/rust-image" }
  },
  "applications": {
    "c-echo": { "type": "wasm", "module": "$B/c-echo.wasm", $luw },
    "c-json": { "type": "wasm", "module": "$B/c-json.wasm", $luw },
    "rust-hello": { "type": "wasm-wasi-component", "component": "$B/rust-hello.wasm" },
    "rust-json": { "type": "wasm-wasi-component", "component": "$B/rust-json.wasm" },
    "rust-image": { "type": "wasm-wasi-component", "component": "$B/rust-image.wasm" }
  }
}
JSON
echo

req() {
    name=$1; shift
    printf '\n== %s: ' "$name"
    curl -sS -o "$T/body" -w '%{http_code} %{size_download}B %{time_total}s\n' "$@"
    case $(file -b --mime-type "$T/body") in
        image/*) file -b "$T/body" ;;
        *) head -c 600 "$T/body"; echo ;;
    esac
}

req c-echo -d 'hello=wasm' http://127.0.0.1:8400/echo?x=1
req c-json http://127.0.0.1:8401/
req c-json-post -d '{"a":[1,2,3],"b":{"c":"d"}}' http://127.0.0.1:8401/
req rust-hello http://127.0.0.1:8402/
req rust-json-get http://127.0.0.1:8403/
req rust-json-transform -d '{"z":1,"a":[true,null,"x",2.5]}' http://127.0.0.1:8403/transform
req rust-json-shorten -d '{"url":"https://freeunit.org/"}' http://127.0.0.1:8403/shorten
req rust-image 'http://127.0.0.1:8404/?w=64&h=48'

echo
echo "== log ($T/unit.log)"
grep -E 'wasm|component|started|exited|error|alert' "$T/unit.log" | tail -40
