#!/bin/sh
#
# Run libFuzzer targets for a bounded time against their seed corpora and fail
# on the first crash.  This is a smoke gate, not a fuzzing campaign: the
# question it answers is "does the HTTP parser still survive its own seed
# corpus and a minute of shallow mutation", which is the part worth blocking a
# pull request on.
#
# Usage:
#   fuzzing/run-ci.sh [-t SECONDS] [TARGET ...]
#
#   -t SECONDS   libFuzzer budget per target (default 60)
#   TARGET ...   fuzzer names; default is all five
#
# Expects the fuzzers to be built already:
#   CC=clang CXX=clang++ \
#   CFLAGS="-g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION \
#           -fsanitize=address,undefined -fsanitize=fuzzer-no-link" \
#   ./configure --no-regex --no-pcre2 --fuzz=-fsanitize=fuzzer && make fuzz
#
# Any crash, leak or sanitizer report leaves the reproducer in
# build/fuzz-artifacts/ for CI to upload.

set -eu

BUDGET=60
BUILD=${BUILD_DIR:-build}

die() { echo "run-ci.sh: $*" >&2; exit 1; }

while getopts 't:' opt; do
    case "$opt" in
        t) BUDGET=$OPTARG ;;
        *) die "usage: $0 [-t SECONDS] [TARGET ...]" ;;
    esac
done
shift $((OPTIND - 1))

# The budget reaches this from a workflow input and is then used in shell
# arithmetic, where a non-numeric value is at best a confusing error.
case "$BUDGET" in
    ''|*[!0-9]*) die "budget must be a whole number of seconds, got '$BUDGET'" ;;
esac

# Seed corpus and dictionary per target.  fuzz_http_* share the HTTP corpus.
corpus_for() {
    case "$1" in
        fuzz_basic)     echo fuzz_basic_seed_corpus ;;
        fuzz_json)      echo fuzz_json_seed_corpus ;;
        fuzz_http_*)    echo fuzz_http_seed_corpus ;;
        *)              die "unknown target: $1" ;;
    esac
}

dict_for() {
    case "$1" in
        fuzz_http_*)    echo fuzz_http.dict ;;
        *)              echo "" ;;
    esac
}

[ $# -gt 0 ] || set -- fuzz_basic fuzz_json fuzz_http_controller \
                       fuzz_http_h1p fuzz_http_h1p_peer

# Freeze the list: the loop below rebuilds "$@" to hold each target's libFuzzer
# arguments, so it cannot also be the list being iterated.
TARGETS=$*

# -fsanitize=undefined is recoverable by default: UBSan prints the report and
# execution continues, so libFuzzer exits 0 and a green check hides the
# finding.  halt_on_error turns a report into an abort, which libFuzzer
# records as a crash and saves a reproducer for.  Set here rather than in the
# build flags so the guarantee travels with the runner, whoever built the
# binaries.
#
# Deliberately without print_stacktrace=1: that pulls in llvm-symbolizer, the
# same thirteen-seconds-a-call cost that -print_funcs=0 exists to avoid, and
# it turns the abort into a stall the wall-clock cap then files as "slow".
# The report keeps its file:line and the reproducer is saved, which is what a
# rerun needs.
# Appended rather than defaulted: sanitizer flags are parsed left to right and
# the last wins, so this stays authoritative even when the caller already has
# UBSAN_OPTIONS set to something (including halt_on_error=0).
UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1"
export UBSAN_OPTIONS

ART="$BUILD/fuzz-artifacts"
CORPORA="$BUILD/fuzz-corpora"
mkdir -p "$ART" "$CORPORA" || die "cannot write under $BUILD -- wrong BUILD_DIR?"

# Check every binary up front: a build that silently produced nothing must not
# be mistaken for a clean run.
for target in $TARGETS; do
    [ -x "$BUILD/$target" ] || die "no fuzzer at $BUILD/$target -- was 'make fuzz' run?"
    [ -d "fuzzing/$(corpus_for "$target")" ] \
        || die "no seed corpus fuzzing/$(corpus_for "$target") for $target"
done

# -print_funcs=0 is not cosmetic.  libFuzzer symbolizes every newly covered
# function to print its NEW_FUNC line, and llvm-symbolizer takes about thirteen
# seconds per call against a 14 MB ASan+UBSan binary, so the exploration phase
# -- exactly the part a fresh CI corpus is all of -- stalls on the symbolizer.
# Measured here, 30-second budget, before and after:
#
#   fuzz_json              7 runs / 90s   ->  1190884 runs / 31s
#   fuzz_http_h1p        161 runs / 91s   ->  1267717 runs / 31s
#   fuzz_http_controller                  ->  1567704 runs / 31s
#   fuzz_http_h1p_peer                    ->  1442178 runs / 32s
#
# Crash reports keep their symbols either way: those go through the sanitizer's
# own stack printer, not libFuzzer's coverage output.  Verified on a purpose-
# built crashing target.
PRINT_FUNCS=-print_funcs=0

# -max_total_time is only checked between runs and libFuzzer's own -timeout
# defaults to twenty minutes, so a single pathological unit can still run far
# past the budget.  Bound the wall clock rather than tightening -timeout: a
# slow target is not a finding, and turning slowness into a red check would
# only teach people to ignore the job.  A minute of headroom is ample now that
# a target finishes within a second or two of its budget.
#
# FUZZ_WALL_MARGIN exists so the two branches below can be exercised in a
# test without waiting a minute for the cap.
WALL=$((BUDGET + ${FUZZ_WALL_MARGIN:-60}))

status=0
attempted=0
ran=0
slow=0

for target in $TARGETS; do
    seed=$(corpus_for "$target")
    dict=$(dict_for "$target")

    mkdir -p "$CORPORA/$target"
    set -- "$CORPORA/$target" "fuzzing/$seed" \
        "-max_total_time=$BUDGET" \
        "-artifact_prefix=$ART/$target-" \
        "$PRINT_FUNCS" -print_final_stats=1 -rss_limit_mb=2560
    [ -n "$dict" ] && set -- "$@" "-dict=fuzzing/$dict"

    attempted=$((attempted + 1))
    echo "=== $target (${BUDGET}s budget, ${WALL}s wall cap, seeds: $seed${dict:+, dict: $dict}) ==="
    rc=0
    started=$(date +%s)
    timeout -k 5 "$WALL" "$BUILD/$target" "$@" || rc=$?
    elapsed=$(( $(date +%s) - started ))

    # 137 is 128+SIGKILL, which is what `timeout -k` reports for a target it
    # had to kill -- and also what the kernel's OOM killer produces mid-run.
    # Distinguishing them matters: an OOM is a finding and must not be filed
    # under "slow".  Only a kill at or past the cap is the cap.
    case "$rc" in
        0)
            ran=$((ran + 1))
            ;;
        124|137)
            if [ "$elapsed" -ge "$WALL" ]; then
                echo "run-ci.sh: $target hit the ${WALL}s wall cap (not a finding)" >&2
                slow=$((slow + 1))
                ran=$((ran + 1))
            else
                echo "run-ci.sh: $target KILLED after ${elapsed}s, well inside the" \
                     "${WALL}s cap -- treating as a failure (OOM killer?)" >&2
                status=1
            fi
            ;;
        *)
            echo "run-ci.sh: $target FAILED (exit $rc after ${elapsed}s)" >&2
            status=1
            ;;
    esac
    echo ""
done

# Guard on what was attempted, not on what succeeded: an empty target list
# would otherwise be a green run that fuzzed nothing.
[ "$attempted" -gt 0 ] || die "no fuzzer actually ran"
echo "run-ci.sh: $attempted target(s) attempted, $ran finished, $slow hit the wall cap, ${BUDGET}s budget each"
exit "$status"
