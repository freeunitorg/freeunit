#!/usr/bin/env bash
#
# Local gates.  Each prints "G<n>: PASS" or "G<n>: FAIL -- ..."; the exit
# status is non-zero if any failed.  G1 and G3 rebuild ./build.
#
#   G1  --hardening=strict build, gcc and clang
#   G2  build and a pytest slice (-t)
#   G3  ASan+UBSan build and test_static.py
#   G4  short fuzz run (not run by default)
#   G5  ast-grep baseline, and disasm-diff if tools/perf/baseline exists
#   G6  @contract comments for tools/gates/contract_functions.txt
#
#   tools/gates/run_gates.sh [-g 1,2,3,5,6] [-t "test_a.py test_b.py"]
#                            [--fuzz-seconds 20]

set -u
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit

GATES=1,2,3,5,6
# Tests that need no language module: G1 builds none.
SLICE="test_static.py test_variables.py test_return.py"
FUZZ_SECONDS=20
PYTEST=${PYTEST:-pytest}
FAILED=0

export JAVA_TOOL_OPTIONS=

while [ $# -gt 0 ]; do
    case "$1" in
        -g|--gates)     GATES=$2; shift 2 ;;
        -t|--tests)     SLICE=$2; shift 2 ;;
        --fuzz-seconds) FUZZ_SECONDS=$2; shift 2 ;;
        -h|--help)      sed -n '3,/^$/s/^# \{0,1\}//p' "$0"; exit 0 ;;
        *)              echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

fail() {
    echo "G$1: FAIL -- $2"
    FAILED=1
    return 1
}

# run <gate> <log name> <command...>: the log's tail is shown on failure.
run() {
    local log="/tmp/g$1-$2.log"

    if "${@:3}" >"$log" 2>&1; then
        return 0
    fi

    tail -n 40 "$log"
    fail "$1" "$2 failed, see $log"
}

gate_1() {
    local cc rc=0

    for cc in gcc clang; do
        if ! command -v "$cc" >/dev/null; then
            echo "G1 ($cc): SKIP -- not found"
            continue
        fi

        rm -rf build
        run 1 "$cc-configure" \
            env CC="$cc" ./configure --zlib --openssl --hardening=strict \
            && run 1 "$cc-build" make -j2 || rc=1
    done

    return $rc
}

gate_2() {
    run 2 build make -j2 \
        && run 2 pytest sh -c "cd test && $PYTEST $SLICE" \
        || return 1

    # A slice whose every test is skipped exits 0 too.
    grep -qE '\b[1-9][0-9]* passed\b' /tmp/g2-pytest.log \
        || fail 2 "no test ran, see /tmp/g2-pytest.log"
}

gate_3() {
    local asan="-fsanitize=address,undefined"

    rm -rf build
    run 3 configure env ASAN_OPTIONS=detect_leaks=0 ./configure --debug \
        --tests --openssl --cc-opt="$asan -fno-omit-frame-pointer -O1" \
        --ld-opt="$asan" \
        && run 3 build make -j2 unitd \
        && run 3 pytest env ASAN_OPTIONS=detect_leaks=0 \
               UBSAN_OPTIONS=print_stacktrace=1 \
               sh -c "cd test && $PYTEST test_static.py"
}

gate_4() {
    run 4 build bash fuzzing/build-fuzz.sh \
        && run 4 fuzz build/fuzz_basic -max_total_time="$FUZZ_SECONDS" \
               fuzzing/fuzz_basic_seed_corpus
}

gate_5() {
    local rc=0

    if ! command -v ast-grep >/dev/null; then
        fail 5 "ast-grep not installed"
        return
    fi

    python3 tools/ast-grep/check_baseline.py \
        || fail 5 "new ast-grep violations" || rc=1

    if [ -n "$(ls -A tools/perf/baseline 2>/dev/null)" ]; then
        run 5 disasm tools/perf/disasm-diff.sh --cc clang || rc=1
    else
        echo "G5 (disasm-diff): SKIP -- no tools/perf/baseline"
    fi

    return $rc
}

gate_6() {
    local fn file line found rc=0

    while IFS= read -r fn; do
        case "$fn" in
            ''|'#'*) continue ;;
        esac

        found=0

        while IFS=: read -r file line _; do
            if sed -n "$((line > 15 ? line - 15 : 1)),${line}p" "$file" \
               | grep -q '@contract'
            then
                found=1
                break
            fi
        done < <(grep -rn -E "^[A-Za-z_][A-Za-z0-9_ *]*\b$fn\(" src \
                 --include='*.c')

        if [ "$found" = 0 ]; then
            echo "  missing @contract for: $fn"
            rc=1
        fi
    done < tools/gates/contract_functions.txt

    [ "$rc" = 0 ] || fail 6 "functions without @contract"
}

ran=0

for g in 1 2 3 4 5 6; do
    case ",$GATES," in
        *",$g,"*) ;;
        *) continue ;;
    esac

    ran=1
    echo "==> G$g"

    if "gate_$g"; then
        echo "G$g: PASS"
    fi

    echo
done

if [ "$ran" = 0 ]; then
    echo "no gates matched '$GATES'" >&2
    exit 2
fi

if [ "$FAILED" != 0 ]; then
    echo "one or more gates FAILED"
    exit 1
fi

echo "all selected gates PASSED"
