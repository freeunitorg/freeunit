#!/usr/bin/env bash
#
# Local gates.  Each gate prints "G<n>: PASS" or "G<n>: FAIL -- ...".
# The exit status is not zero when a gate failed.  G1 and G3 rebuild
# ./build.
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
# G1 builds no language module.  Thus the slice has only tests that need
# no language module.
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

# run <gate> <log name> <command...>: on failure, show the tail of the log.
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
    # G2 can run alone.  Then no build is configured yet.
    if [ ! -f build/Makefile ] || [ ! -f Makefile ]; then
        run 2 configure ./configure --openssl || return 1
    fi

    run 2 build make -j2 \
        && run 2 pytest sh -c "cd test && $PYTEST $SLICE" \
        || return 1

    # A slice in which each test is skipped also exits with 0.
    grep -qE '\b[1-9][0-9]* passed\b' /tmp/g2-pytest.log \
        || fail 2 "no test ran, see /tmp/g2-pytest.log"
}

gate_3() {
    local asan="-fsanitize=address,undefined" logs rc

    rm -rf build
    run 3 configure env ASAN_OPTIONS=detect_leaks=0 ./configure --debug \
        --tests --openssl --cc-opt="$asan -fno-omit-frame-pointer -O1" \
        --ld-opt="$asan" \
        && run 3 build make -j2 unitd \
        || return 1

    # A sanitizer report from unitd goes only to unit.log.  pytest can pass
    # with such a report.  Keep the logs in a private directory, and scan
    # them as CI does (.github/workflows/sanitize.yml).
    logs=$(mktemp -d)

    # The router runs as an unprivileged user.  It must reach the files
    # under this directory.
    chmod 755 "$logs"

    run 3 pytest env ASAN_OPTIONS=detect_leaks=0 \
        UBSAN_OPTIONS=print_stacktrace=1 TMPDIR="$logs" \
        sh -c "cd test && $PYTEST --save-log test_static.py"
    rc=$?

    run 3 sanitizer-reports .github/scripts/check-sanitizer-reports.sh \
        "$logs" .github/sanitizer-allowlist.txt || rc=1

    rm -rf "$logs"

    return $rc
}

# The fuzz build as in fuzzing/build-fuzz.sh, but a failed configure or
# make fails G4: build-fuzz.sh returns 0 even then, and a parallel make can
# build fuzz_basic before another target fails.  run-ci.sh makes a UBSan
# report fail the run (halt_on_error=1), and it writes new inputs to
# build/fuzz-corpora/, not to the tracked seed corpus.
gate_4() {
    local flags="-g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION"

    flags="$flags -fsanitize=address,undefined -fsanitize=fuzzer-no-link"

    rm -f build/fuzz_basic

    run 4 configure env CC=clang CXX=clang++ CFLAGS="$flags" \
            CXXFLAGS="$flags" \
            ./configure --no-regex --no-pcre2 --fuzz=-fsanitize=fuzzer \
        && run 4 build make fuzz -j"$(nproc)" \
        && run 4 fuzz sh fuzzing/run-ci.sh -t "$FUZZ_SECONDS" fuzz_basic
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

# contract_above <file> <line>: the line starts a function definition,
# and the comment block directly above it has an "@contract" line.  A
# declaration ends with ";" before any "{", so it does not count.  Only
# the lines of the type above the name are skipped: a line with other
# text, or a blank line, ends the search, so a comment of an earlier
# function or declaration does not count.
contract_above() {
    awk -v def="$2" '
        { text[NR] = $0 }
        END {
            # A definition: "{" comes before ";" after the name.
            for (i = def; i <= NR; i++) {
                if (text[i] ~ /;/) {
                    exit 1
                }

                if (text[i] ~ /\{/) {
                    break
                }
            }

            if (i > NR) {
                exit 1
            }

            i = def - 1

            # The type lines, for example "static nxt_int_t": names,
            # blanks and "*" only.  Any other line, such as a declaration
            # that ends with ";", a blank line or a "#" line, ends the search.
            while (i > 0 && text[i] ~ /^[[:space:]]*[A-Za-z_][A-Za-z0-9_[:space:]*]*$/) {
                i--
            }

            if (i < 1 || text[i] !~ /\*\/[[:space:]]*$/) {
                exit 1
            }

            # The comment block, from its end up to its start.
            for (; i > 0; i--) {
                if (text[i] ~ /@contract/) {
                    exit 0
                }

                if (text[i] ~ /\/\*/) {
                    exit 1
                }
            }

            exit 1
        }
    ' "$1"
}

gate_6() {
    local fn file line found rc=0

    while IFS= read -r fn; do
        case "$fn" in
            ''|'#'*) continue ;;
        esac

        found=0

        while IFS=: read -r file line _; do
            if contract_above "$file" "$line"; then
                found=1
                break
            fi
        # The usual style has the type on the line above the name:
        #   static void
        #   nxt_conn_wait(...)
        # Also accept the type and the name on one line.
        done < <(grep -rn -E "^([A-Za-z_][A-Za-z0-9_ *]*[ *])?$fn\(" src \
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
