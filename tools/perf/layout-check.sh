#!/bin/sh
#
# Copyright (C) NGINX, Inc.
#
# Diff the pahole layouts of the shared-memory and key process-local structs
# against tools/perf/layout-baseline/<cc>-<version>-<libc>.txt.  A changed
# shared-memory struct fails unless --allow-abi-bump; a process-local one
# only warns.
#
#   tools/perf/layout-check.sh --cc clang [--update] [--allow-abi-bump]
#   tools/perf/layout-check.sh --cc musl-gcc --configure-opt=--no-regex
#
# $PAHOLE picks the pahole binary.

set -eu

# shellcheck source-path=SCRIPTDIR source=lib.sh
. "$(dirname "$0")/lib.sh"

PAHOLE=${PAHOLE:-pahole}
need "$PAHOLE"

BASELINE_FILE="tools/perf/layout-baseline/$TOOLCHAIN.txt"

build layout

UNITD="$BUILD_DIR/sbin/unitd"
UNIT_O="$BUILD_DIR/src/nxt_unit.o"

# name:binary:kind
STRUCTS="
nxt_nncq_t:$UNITD:shm
nxt_app_nncq_t:$UNITD:shm
nxt_port_queue_t:$UNITD:shm
nxt_app_queue_t:$UNITD:shm
nxt_port_queue_item_t:$UNITD:shm
nxt_app_queue_item_t:$UNITD:shm
nxt_port_mmap_header_s:$UNITD:shm
nxt_unit_request_s:$UNITD:shm
nxt_port_s:$UNITD:local
nxt_unit_ctx_impl_s:$UNIT_O:local
"

dump="$WORK/layout.txt"
: > "$dump"

for entry in $STRUCTS; do
    name=${entry%%:*}
    rest=${entry#*:}
    binary=${rest%%:*}

    echo "### $name (${rest#*:})" >> "$dump"

    if "$PAHOLE" -C "$name" "$binary" 2>>"$dump" >> "$WORK/one"; then
        cat "$WORK/one" >> "$dump"
    else
        echo "(pahole found no output for $name in $binary)" >> "$dump"
    fi

    rm -f "$WORK/one"
    echo >> "$dump"
done

# No trailing blank lines: git diff --check rejects them in the baseline.
awk 'NF { while (n) { print ""; n-- } print; next } { n++ }' "$dump" > "$dump.t"
mv "$dump.t" "$dump"

if [ "$UPDATE" -eq 1 ]; then
    cp "$dump" "$BASELINE_FILE"
    echo "Baseline written to $BASELINE_FILE"
    exit 0
fi

if [ ! -f "$BASELINE_FILE" ]; then
    echo "NEW (no baseline yet): $BASELINE_FILE; run with --update"
    exit 1
fi

if diff -u "$BASELINE_FILE" "$dump" > "$WORK/diff"; then
    echo "no layout changes ($TOOLCHAIN)"
    exit 0
fi

echo "=== layout differs from baseline ($BASELINE_FILE) ==="
cat "$WORK/diff"

section() {
    awk -v hdr="### $1 (" '
        index($0, hdr) == 1 { grab=1; print; next }
        grab && index($0, "### ") == 1 { exit }
        grab { print }
    ' "$2"
}

shm=
proc=

for entry in $STRUCTS; do
    name=${entry%%:*}

    if [ "$(section "$name" "$BASELINE_FILE")" != "$(section "$name" "$dump")" ]
    then
        case "$entry" in
            *:shm) shm="$shm $name" ;;
            *)     proc="$proc $name" ;;
        esac
    fi
done

[ -z "$proc" ] || echo "warning: process-local layout changed:$proc"

[ -n "$shm" ] || exit 0

if [ "$ALLOW_ABI_BUMP" -eq 1 ]; then
    echo "warning: shared-memory layout changed, --allow-abi-bump:$shm"
    exit 0
fi

echo "FAIL: shared-memory layout changed:$shm"
echo "For a reviewed ABI bump, rerun with --update in the same commit."
exit 1
