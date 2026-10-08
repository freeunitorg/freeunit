#!/bin/bash
#
# Print the test matrix of build-test.yml as a JSON list of leg names.
#
# Usage: test-matrix.sh EOL_JSON [CHANGED_FILES]
#        test-matrix.sh --self-test
#
# The versions come from EOL_JSON (pkg/eol.json).  Without CHANGED_FILES,
# every version of every runtime gets a leg.  That is the matrix for master
# pushes and for any other event that is not a pull request.
#
# CHANGED_FILES is a file with one changed path per line, the list of a pull
# request.  Then each of go, java, node, php and ruby gets one leg, unless a
# changed path belongs to that runtime: then it gets every version.  A change
# to pkg/eol.json, to build-test.yml or to this script gives every runtime
# every version.  A missing or empty list also gives the full matrix.
#
# A pull request with the label "ci-full" gets no list from build-test.yml,
# so it runs the full matrix.  Use it for a shared change that one version
# of each runtime does not cover.  The label counts from the next run that
# a push, or a close and reopen, starts.  A re-run uses the event of the old
# run and does not see a new label.
#
# The one leg is the newest version that pkg/eol.json marks "lts", or the
# newest version when the runtime marks none: java-25, node-26, php-8.5,
# go-1.27 and ruby-4.0 today.  A java version that is not LTS gets six
# months of updates, so for java the newest LTS is the version that stays in
# use longest.  The other versions still run on every master push.
#
# All python legs always run.  Each takes a share of the language-agnostic
# test files (see "Output build metadata" in build-test.yml), so a dropped
# python leg would drop that share.  perl, wasm and wasm-wasi-component have
# one leg each.
#
# --self-test checks these rules on a fixed eol.json and fixed lists of
# changed files.  It prints each case and exits 1 when one fails.  The
# prepare job runs it before it builds the matrix.

set -eu

versioned="go java node php ruby"

# Paths that change what a runtime's legs build or test.  Shared code (src/*.c
# outside these, libunit, the router, test/conftest.py) reaches every runtime
# the same way, so one version of each covers it.
declare -A own=(
    [go]='^(go/|auto/modules/go$|test/test_go|test/go/|test/unit/applications/lang/go\.py$|test/unit/check/go\.py$)'
    [java]='^(src/java/|src/nxt_java\.c$|auto/modules/java|test/test_java|test/java/|test/unit/applications/lang/java\.py$)'
    [node]='^(src/nodejs/|auto/modules/nodejs|test/test_node|test/node/|test/unit/applications/lang/node\.py$|test/unit/check/node\.py$)'
    [php]='^(src/nxt_php|src/php/|auto/modules/php|test/test_php|test/php/|test/unit/applications/lang/php\.py$)'
    [ruby]='^(src/ruby/|auto/modules/ruby|test/test_ruby|test/ruby/|test/unit/applications/lang/ruby\.py$)'
)

matrix_re='^(pkg/eol\.json|\.github/workflows/build-test\.yml|\.github/scripts/test-matrix\.sh)$'

# matrix EOL_JSON [CHANGED_FILES]
matrix() {
    local eol=$1
    local changed=${2:-}
    local every="" rt

    if [ -z "$changed" ] || [ ! -s "$changed" ]; then
        echo "no list of changed files: every version of every runtime" >&2
        every=$versioned

    elif grep -qE "$matrix_re" "$changed"; then
        echo "the matrix definition changed: every version of every runtime" >&2
        every=$versioned

    else
        for rt in $versioned; do
            if grep -qE "${own[$rt]}" "$changed"; then
                echo "$rt: changed paths, every version" >&2
                every="$every $rt"
            else
                echo "$rt: one version" >&2
            fi
        done
    fi

    # Slow legs come first, as before; see the comment in build-test.yml.
    jq -c --arg every "$every" '
        def vkey: .version | split(".") | map(tonumber? // 0);

        def one($v):
            ($v | map(select(.lts == true))) as $lts
            | if ($lts | length) > 0 then $lts else $v end
            | sort_by(vkey) | [last];

        def legs($rt):
            (.runtimes[$rt] // []) as $v
            | if ($v | length) == 0 then []
              elif ($every | split(" ") | any(. == $rt)) then $v
              else one($v)
              end
            | map($rt + "-" + .version);

        [ (.runtimes.python // [])[] | "python-" + .version ]
        + ["wasm-wasi-component"]
        + legs("java")
        + legs("php")
        + ["unit", "perl", "wasm"]
        + legs("go")
        + legs("node")
        + legs("ruby")
    ' "$eol"
}

self_test() {
    local fail=0
    dir=$(mktemp -d)
    trap 'rm -rf "$dir"' EXIT

    # Fixed versions, so the cases do not change when pkg/eol.json does.
    # java and node mark "lts" as pkg/eol.json does.  go has 1.9 and 1.10
    # to check that versions sort as numbers.
    cat > "$dir/eol.json" <<'EOF'
{"runtimes": {
  "python": [{"version": "3.12"}, {"version": "3.13"}],
  "java": [{"version": "17", "lts": true}, {"version": "21", "lts": true},
           {"version": "25", "lts": true}, {"version": "26"}, {"version": "27"}],
  "php": [{"version": "8.4"}, {"version": "8.5"}],
  "go": [{"version": "1.9"}, {"version": "1.10"}],
  "node": [{"version": "20"}, {"version": "22"}, {"version": "24"},
           {"version": "26", "lts": true}],
  "ruby": [{"version": "3.4"}, {"version": "4.0"}]
}}
EOF

    local py="python-3.12 python-3.13"
    local j1="java-25" jall="java-17 java-21 java-25 java-26 java-27"
    local p1="php-8.5" pall="php-8.4 php-8.5"
    local g1="go-1.10" gall="go-1.9 go-1.10"
    local n1="node-26" nall="node-20 node-22 node-24 node-26"
    local r1="ruby-4.0" rall="ruby-3.4 ruby-4.0"
    local full="$py $jall $pall $gall $nall $rall"

    # check NAME EXPECTED [PATH...]
    # With no PATH the list is empty.  NAME "missing" passes no list, and
    # NAME "no such file" passes a list that does not exist.
    check() {
        local name=$1 want=$2 list=$dir/list got
        shift 2
        case $name in
        missing) list= ;;
        "no such file") list=$dir/none ;;
        *) printf '%s\n' "$@" | sed '/^$/d' > "$list" ;;
        esac
        got=$(matrix "$dir/eol.json" "$list" 2>/dev/null) || got="exit $?"
        # Only python and the versioned runtimes: the fixed legs are not
        # part of these rules.
        got=$(jq -r '[.[] | select(test("^(python|go|java|node|php|ruby)-"))]
                     | join(" ")' <<< "$got" 2>&1) || true
        if [ "$got" = "$want" ]; then
            echo "ok   $name"
        else
            echo "FAIL $name"
            echo "     want: $want"
            echo "     got:  $got"
            fail=1
        fi
    }

    check missing "$full"
    check "no such file" "$full"
    check "empty list" "$full"
    check "src/nxt_router.c" "$py $j1 $p1 $g1 $n1 $r1" src/nxt_router.c
    check "test/unit/check/tls.py" "$py $j1 $p1 $g1 $n1 $r1" \
        test/unit/check/tls.py
    check "pkg/eol.json" "$full" pkg/eol.json
    check "build-test.yml" "$full" .github/workflows/build-test.yml
    check "test-matrix.sh" "$full" .github/scripts/test-matrix.sh
    check "matrix file among others" "$full" src/nxt_router.c pkg/eol.json
    check "go/" "$py $j1 $p1 $gall $n1 $r1" go/nxt_cgo_lib.c
    check "test/test_go_*" "$py $j1 $p1 $gall $n1 $r1" \
        test/test_go_application.py
    check "src/java/" "$py $jall $p1 $g1 $n1 $r1" \
        src/java/nginx/unit/Context.java
    check "src/nxt_java.c" "$py $jall $p1 $g1 $n1 $r1" src/nxt_java.c
    check "auto/modules/java_jar.sha512" "$py $jall $p1 $g1 $n1 $r1" \
        auto/modules/java_jar.sha512
    check "src/nodejs/" "$py $j1 $p1 $g1 $nall $r1" \
        src/nodejs/unit-http/http.js
    check "test/unit/check/node.py" "$py $j1 $p1 $g1 $nall $r1" \
        test/unit/check/node.py
    check "src/nxt_php_sapi.c" "$py $j1 $pall $g1 $n1 $r1" src/nxt_php_sapi.c
    check "a new src/nxt_php_*.c" "$py $j1 $pall $g1 $n1 $r1" \
        src/nxt_php_extension.c
    check "test/test_php_*" "$py $j1 $pall $g1 $n1 $r1" \
        test/test_php_targets.py
    check "src/ruby/" "$py $j1 $p1 $g1 $n1 $rall" src/ruby/nxt_ruby.c
    check "two runtimes" "$py $jall $p1 $g1 $n1 $rall" \
        src/nxt_router.c src/java/nxt_jni.c src/ruby/nxt_ruby.c

    return $fail
}

if [ "${1:-}" = --self-test ]; then
    self_test
    exit
fi

matrix "$1" "${2:-}"
