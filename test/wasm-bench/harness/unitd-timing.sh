#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Cross-check the harness numbers against a real unitd: every guest runs as
# an application with "processes": {"spare": 0}, so after
# /control/applications/NAME/restart no application process exists and the
# next request has to start one (fork from the prototype, engine, compile,
# instantiate) before it is answered.  Prints the first-request latency
# ("cold", includes the compile) and the second ("warm") for RUNS restarts.
#
# Same prerequisites as unitd-smoke.sh.  Usage: unitd-timing.sh [RUNS]

set -eu

RUNS=${1:-5}
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B="$HERE/build"
T=$(mktemp -d /tmp/wasm-bench-unitd.XXXXXX)
SOCK="$T/control.sock"

cleanup() {
    [ -f "$T/unit.pid" ] && kill "$(cat "$T/unit.pid")" 2>/dev/null || true
    sleep 0.5
    rm -rf "$T"
}
trap cleanup EXIT

LD_LIBRARY_PATH="$ROOT/pkg/contrib/wasmtime/artifacts/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH

"$ROOT/build/sbin/unitd" --control "unix:$SOCK" --pid "$T/unit.pid" \
    --log "$T/unit.log" --modulesdir "$ROOT/build/lib/unit/modules" \
    --statedir "$T" --tmpdir "$T" 2>/dev/null

for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.1; done

luw='"request_handler": "luw_request_handler", "malloc_handler": "luw_malloc_handler", "free_handler": "luw_free_handler", "module_init_handler": "luw_module_init_handler", "module_end_handler": "luw_module_end_handler"'
p='"processes": {"max": 1, "spare": 0, "idle_timeout": 3600}'

ctl() { curl -sS --unix-socket "$SOCK" "$@" >/dev/null; }

ctl -X PUT -d @- http://localhost/config <<JSON
{
  "listeners": { "127.0.0.1:8410": { "pass": "applications/app" } },
  "applications": { "app": { "type": "wasm", "module": "$B/c-echo.wasm", $luw, $p } }
}
JSON

printf '| guest | type | first request after restart ms (cold) | second request ms (warm) |\n'
printf '|---|---|---|---|\n'

for g in ci-hello c-echo c-json ci-hello-world rust-hello rust-json rust-image; do
    [ -f "$B/$g.wasm" ] || continue
    case $g in
        c-*|ci-hello) type=wasm
            app="{\"type\": \"wasm\", \"module\": \"$B/$g.wasm\", $luw, $p}" ;;
        *) type=wasm-wasi-component
            app="{\"type\": \"wasm-wasi-component\", \"component\": \"$B/$g.wasm\", $p}" ;;
    esac
    ctl -X PUT -d "$app" http://localhost/config/applications/app

    cold=""; warm=""
    for _ in $(seq "$RUNS"); do
        ctl http://localhost/control/applications/app/restart
        sleep 0.3
        c=$(curl -s -o /dev/null -w '%{time_total}' http://127.0.0.1:8410/)
        w=$(curl -s -o /dev/null -w '%{time_total}' http://127.0.0.1:8410/)
        cold="$cold $c"; warm="$warm $w"
    done

    printf '| %s | %s | %s | %s |\n' "$g" "$type" \
        "$(echo "$cold" | python3 -c 'import sys,statistics as s; x=[float(v)*1e3 for v in sys.stdin.read().split()]; print(f"{s.mean(x):.1f} ± {s.stdev(x) if len(x)>1 else 0:.1f} ({min(x):.1f}–{max(x):.1f})")')" \
        "$(echo "$warm" | python3 -c 'import sys,statistics as s; x=[float(v)*1e3 for v in sys.stdin.read().split()]; print(f"{s.mean(x):.2f} ± {s.stdev(x) if len(x)>1 else 0:.2f} ({min(x):.2f}–{max(x):.2f})")')"
done
