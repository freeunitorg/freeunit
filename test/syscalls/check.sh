#!/bin/sh
#
# Compare a captured syscall list (test/syscalls/capture.sh) against a
# checked-in baseline.
#
#   New syscalls  -> failure.  Unit reached for a kernel interface it did not
#                    use before; that is a deliberate change and the baseline
#                    has to be updated in the same commit.
#   Gone syscalls -> warning.  Losing a syscall is not a hazard, and a runner
#                    or libc update legitimately drops one now and then.
#
# Usage: test/syscalls/check.sh BASELINE CAPTURED
#
# See test/syscalls/README.md for how to refresh a baseline.

set -eu

BASELINE=${1:?usage: $0 BASELINE CAPTURED}
CAPTURED=${2:?usage: $0 BASELINE CAPTURED}

die() { echo "check.sh: $*" >&2; exit 1; }

[ -f "$BASELINE" ] || die "no baseline at $BASELINE"
[ -s "$CAPTURED" ] || die "captured list $CAPTURED is missing or empty"

# An empty or unsorted input compares clean against anything, so the two
# failure modes that would make this gate silently pass are checked first.
n_captured=$(grep -c '^[a-z]' "$CAPTURED" || :)
[ "$n_captured" -ge 20 ] \
    || die "captured list has only $n_captured entries -- refusing to compare"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

# Baselines carry '#' comments and blank lines; captures do not.
# LC_ALL=C throughout: comm rejects input sorted under a locale whose collation
# differs from the one that produced it, and '_' is exactly where they differ.
sed -e 's/#.*//' -e 's/[ \t]*$//' -e '/^$/d' "$BASELINE" | LC_ALL=C sort -u > "$work/base"
sed -e 's/#.*//' -e 's/[ \t]*$//' -e '/^$/d' "$CAPTURED" | LC_ALL=C sort -u > "$work/have"

LC_ALL=C comm -13 "$work/base" "$work/have" > "$work/new"
LC_ALL=C comm -23 "$work/base" "$work/have" > "$work/gone"

echo "baseline: $BASELINE ($(wc -l < "$work/base" | tr -d ' ') syscalls)"
echo "captured: $CAPTURED ($(wc -l < "$work/have" | tr -d ' ') syscalls)"

if [ -s "$work/gone" ]; then
    echo
    echo "WARNING: syscalls in the baseline that were not observed:"
    sed 's/^/  - /' "$work/gone"
    echo "  (not a failure; refresh the baseline when the drop is intended)"
fi

if [ -s "$work/new" ]; then
    echo
    echo "FAIL: syscalls observed that are not in the baseline:"
    sed 's/^/  + /' "$work/new"
    echo
    echo "If this is intended, refresh the baseline:"
    echo "  test/syscalls/run-in-docker.sh <glibc|musl> --update"
    echo "and commit it with the change that introduced the syscall."
    exit 1
fi

echo
echo "OK: no new syscalls."
