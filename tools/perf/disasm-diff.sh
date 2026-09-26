#!/bin/sh
#
# Copyright (C) NGINX, Inc.
#
# Build unitd with a compiler and diff the normalized disassembly of the
# functions in tools/perf/hot-functions.txt against a local baseline in
# tools/perf/baseline/<cc>-<version>-<libc>/ (see tools/perf/README.md).
#
#   tools/perf/disasm-diff.sh --cc clang [--update]
#
# $OBJDUMP picks the disassembler (default objdump).

set -eu

# shellcheck source-path=SCRIPTDIR source=lib.sh
. "$(dirname "$0")/lib.sh"

OBJDUMP=${OBJDUMP:-objdump}
need "$OBJDUMP"

BASELINE_DIR="tools/perf/baseline/$TOOLCHAIN"

build disasm

# The queue code is also in libunit, linked into unit_app_test.  The harness
# wraps the header-only inlines in noinline functions; it is compiled with
# the build's own CFLAGS and only disassembled.
BINARIES="$BUILD_DIR/sbin/unitd"
[ -f "$BUILD_DIR/unit_app_test" ] && BINARIES="$BINARIES $BUILD_DIR/unit_app_test"

# shellcheck disable=SC2016
cflags=$(sed -n 's/^CFLAGS = \(.*\) \$(EXTRA_CFLAGS)$/\1/p' \
         "$BUILD_DIR/Makefile" | head -1)

# shellcheck disable=SC2086
"$CC" -c $cflags -I src -I "$BUILD_DIR/include" -o "$WORK/mca_harness.o" \
    tools/perf/harness/mca_harness.c >"$BUILD_DIR.harness.log" 2>&1 || {
    echo "harness build failed, see $BUILD_DIR.harness.log" >&2
    exit 1
}

BINARIES="$BINARIES $WORK/mca_harness.o"

# Drop addresses, and the %rip displacements that shift with .rodata layout,
# keeping the symbols referenced.
normalize() {
    sed -E \
        -e 's/^[[:space:]]*[0-9a-f]+:[[:space:]]*//' \
        -e 's/^([a-z0-9]+) <([^>]+)>:/\2:/' \
        -e 's/-?0x[0-9a-f]+\(%rip\)/OFF(%rip)/' \
        -e 's/<_IO_stdin_used\+0x[0-9a-f]+>/<rodata>/' \
        -e 's/\b[0-9a-f]{4,}[[:space:]]*<([^>]+)>/<\1>/' \
        -e 's/[[:space:]]+$//'
}

# Whole dumps, sliced with awk: --disassemble=<sym> can miss static symbols.
n=0
for bin in $BINARIES; do
    n=$((n + 1))
    "$OBJDUMP" -d --no-show-raw-insn "$bin" > "$WORK/dump.$n" 2>/dev/null
done

extract() {
    for dump in "$WORK"/dump.*; do
        text=$(awk -v f="$1" '
                $0 ~ "<" f ">:" { grab=1 }
                grab { print }
                grab && /^$/ && NR>1 { exit }
            ' "$dump")

        if [ -n "$text" ]; then
            echo "$text" | normalize
            return
        fi
    done
}

status=0
missing=
diffs=

mkdir -p "$BASELINE_DIR"

while IFS= read -r func || [ -n "$func" ]; do
    case "$func" in
        ''|'#'*) continue ;;
    esac

    out="$WORK/$func.s"
    base="$BASELINE_DIR/$func.s"
    extract "$func" > "$out"

    if [ ! -s "$out" ]; then
        # A tracked function the baseline has must still be in the build:
        # inlined or renamed, it is no longer checked.
        if [ "$UPDATE" -eq 0 ] && [ -f "$base" ]; then
            echo "MISSING (in the baseline, not in this build): $func"
            status=1
        else
            missing="$missing $func"
        fi

    elif [ "$UPDATE" -eq 1 ]; then
        cp "$out" "$base"

    elif [ ! -f "$base" ]; then
        echo "NEW (no baseline yet): $func"
        status=1

    elif ! diff -u "$base" "$out" > "$WORK/$func.diff"; then
        echo "=== $func differs from baseline ($BASELINE_DIR) ==="
        cat "$WORK/$func.diff"
        echo
        diffs="$diffs $func"
        status=1
    fi
done < tools/perf/hot-functions.txt

# A baseline for a function no longer in the list: in CI the baseline comes
# from the base commit's list, so dropping an entry drops its check.
for base in "$BASELINE_DIR"/*.s; do
    [ -f "$base" ] || continue
    func=$(basename "$base" .s)

    grep -qxF "$func" tools/perf/hot-functions.txt && continue

    if [ "$UPDATE" -eq 1 ]; then
        rm -f "$base"
    else
        echo "REMOVED (in the baseline, not in hot-functions.txt): $func"
        status=1
    fi
done

[ "$UPDATE" -eq 0 ] || echo "Baseline written to $BASELINE_DIR"
[ -z "$missing" ] || echo "warning: no disassembly found for:$missing" >&2
[ -z "$diffs" ] || echo "functions with disassembly changes:$diffs"

exit $status
