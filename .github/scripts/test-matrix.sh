#!/bin/bash
#
# Print the test matrix of build-test.yml as a JSON list of leg names.
#
# Usage: test-matrix.sh EOL_JSON [CHANGED_FILES]
#        test-matrix.sh --self-test
#
# The versions come from EOL_JSON (pkg/eol.json).  Without CHANGED_FILES,
# every version of every runtime gets a leg, apart from node and java, which
# get the legs of the rule below.  That is the matrix for master pushes and
# for any other event that is not a pull request.
#
# CHANGED_FILES is a file with one changed path per line, the list of a pull
# request.  Then each of go, java, node, php and ruby gets one leg, unless a
# changed path belongs to that runtime: then it gets every version, and node
# and java get the legs of the rule below.  A change to pkg/eol.json, to
# build-test.yml or to this script gives every runtime every version.  A
# missing or empty list also gives the full matrix.
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
# use longest.  The other versions still run on every master push, apart
# from node and java.
#
# node and java do not run every version, not even in the full matrix.
# They run the oldest supported version, the newest supported version, and
# the newest supported LTS when it is neither of those.  Supported means
# that the note does not contain "EOL", as in pkg/eol/src/main.rs.  node
# gets two legs today, because node-26 is both the newest and the LTS:
# node-22 and node-26.  java gets three, because the newest version is not
# the LTS: java-17, java-25 and java-27.  java-25 is the one leg that a
# pull request gets, so a change to java paths runs the oldest, the newest
# and the LTS together.  The legs follow pkg/eol.json, so a new release or
# a new EOL note moves them.  The versions in between, and the EOL ones,
# keep their Docker images, which pkg/eol.json alone drives.  Only node and
# java get this rule: go-1.25 and perl-5.38 carry the same EOL note and
# still run every version.
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

# Runtimes whose legs are the oldest supported, the newest supported and the
# newest supported LTS, not every version.
minmax_rts="java node"

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
        echo "no list of changed files: the full matrix" >&2
        every=$versioned

    elif grep -qE "$matrix_re" "$changed"; then
        echo "the matrix definition changed: the full matrix" >&2
        every=$versioned

    else
        for rt in $versioned; do
            if grep -qE "${own[$rt]}" "$changed"; then
                echo "$rt: changed paths, all its legs" >&2
                every="$every $rt"
            else
                echo "$rt: one version" >&2
            fi
        done
    fi

    # Slow legs come first, as before; see the comment in build-test.yml.
    jq -c --arg every "$every" --arg minmax "$minmax_rts" '
        def vkey: .version | split(".") | map(tonumber? // 0);

        def one($v):
            ($v | map(select(.lts == true))) as $lts
            | if ($lts | length) > 0 then $lts else $v end
            | sort_by(vkey) | [last];

        # The oldest supported version, the newest supported version, and
        # the newest supported LTS when it is neither of those.  A note that
        # contains "EOL" marks a version as unsupported, as in
        # pkg/eol/src/main.rs.  With no supported version, fall back to the
        # oldest and the newest, with no LTS leg.
        def minmax($v):
            ($v | map(select((.note // "") | contains("EOL") | not))) as $sup
            | (if ($sup | length) > 0 then $sup else $v end
               | sort_by(vkey)) as $s
            | ($sup | map(select(.lts == true)) | sort_by(vkey) | last) as $lts
            | [$s[0], $s[-1], $lts]
            | map(select(. != null))
            | unique_by(.version) | sort_by(vkey);

        def legs($rt):
            (.runtimes[$rt] // []) as $v
            | if ($v | length) == 0 then []
              elif ($every | split(" ") | any(. == $rt)) then
                  if ($minmax | split(" ") | any(. == $rt)) then minmax($v)
                  else $v end
              else one($v)
              end
            | map($rt + "-" + .version);

        [ (.runtimes.python // [])[] | "python-" + .version ]
        + ["wasm-wasi-component"]
        + legs("java")
        + legs("php")
        + ["perl", "wasm"]
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
    # to check that versions sort as numbers.  node-20 and go-1.9 carry an
    # EOL note: node drops node-20, go keeps go-1.9 because only node and
    # java have the min+max rule.  The go note carries a date, to check that
    # a note counts when it contains "EOL", not only when it is exactly
    # that.
    cat > "$dir/eol.json" <<'EOF'
{"runtimes": {
  "python": [{"version": "3.12"}, {"version": "3.13"}],
  "java": [{"version": "17", "lts": true}, {"version": "21", "lts": true},
           {"version": "25", "lts": true}, {"version": "26"}, {"version": "27"}],
  "php": [{"version": "8.4"}, {"version": "8.5"}],
  "go": [{"version": "1.9", "note": "EOL (2026-04)"}, {"version": "1.10"}],
  "node": [{"version": "20", "note": "EOL"}, {"version": "22"},
           {"version": "24"}, {"version": "26", "lts": true}],
  "ruby": [{"version": "3.4"}, {"version": "4.0"}]
}}
EOF

    local py="python-3.12 python-3.13"
    local j1="java-25" jall="java-17 java-25 java-27"
    local p1="php-8.5" pall="php-8.4 php-8.5"
    local g1="go-1.10" gall="go-1.9 go-1.10"
    local n1="node-26" nall="node-22 node-26"
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

    # min+max follows eol.json: a new version moves max, a new EOL note
    # moves min, EOL everywhere falls back to oldest and newest, and the
    # newest supported LTS is added when it is neither of those.
    # mm NAME EXPECTED VERSIONS [RUNTIME]
    mm() {
        local name=$1 want=$2 vers=$3 rt=${4:-node} got
        echo "{\"runtimes\": {\"$rt\": [$vers]}}" > "$dir/mm.json"
        got=$(matrix "$dir/mm.json" "" 2>/dev/null \
              | jq -r --arg p "$rt-" '[.[] | select(startswith($p))] | join(" ")')
        if [ "$got" = "$want" ]; then echo "ok   $name"
        else echo "FAIL $name"; echo "     want: $want"; echo "     got:  $got"; fail=1
        fi
    }
    local e='"note": "EOL"' l='"lts": true' d='"note": "EOL (2026-04)"'

    # node: the newest version is the LTS today, so the two legs are also
    # the oldest and the newest.  A newer version that is not LTS adds the
    # LTS as a third leg.
    mm "node min+max: new node-28" "node-22 node-26 node-28" \
        "{\"version\": \"20\", $e}, {\"version\": \"22\"}, {\"version\": \"24\"}, {\"version\": \"26\", $l}, {\"version\": \"28\"}"
    mm "node min+max: new node-28 is LTS" "node-22 node-28" \
        "{\"version\": \"20\", $e}, {\"version\": \"22\"}, {\"version\": \"24\"}, {\"version\": \"26\"}, {\"version\": \"28\", $l}"
    mm "node min+max: node-22 goes EOL" "node-24 node-26" \
        "{\"version\": \"20\", $e}, {\"version\": \"22\", $e}, {\"version\": \"24\"}, {\"version\": \"26\", $l}"
    mm "node min+max: one supported" "node-26" \
        "{\"version\": \"24\", $e}, {\"version\": \"26\", $l}"
    mm "node min+max: all EOL" "node-20 node-24" \
        "{\"version\": \"20\", $e}, {\"version\": \"22\", $e}, {\"version\": \"24\", $e}"
    mm "node min+max: sorts as numbers" "node-9 node-10" \
        "{\"version\": \"10\"}, {\"version\": \"9\"}"
    mm "node min+max: EOL note with a date" "node-22 node-26" \
        "{\"version\": \"20\", $d}, {\"version\": \"22\"}, {\"version\": \"24\"}, {\"version\": \"26\", $l}"
    mm "node min+max: no LTS" "node-22 node-26" \
        "{\"version\": \"20\", $e}, {\"version\": \"22\"}, {\"version\": \"24\"}, {\"version\": \"26\"}"

    # java: java-25 is the newest LTS and sits between the oldest and the
    # newest.
    local jv="{\"version\": \"17\", $l}, {\"version\": \"21\", $l}, {\"version\": \"25\", $l}, {\"version\": \"26\", $e}"
    mm "java min+max: today" "java-17 java-25 java-27" \
        "$jv, {\"version\": \"27\"}" java
    mm "java min+max: new java-29" "java-17 java-29" \
        "$jv, {\"version\": \"27\"}, {\"version\": \"29\", $l}" java
    mm "java min+max: java-17 goes EOL" "java-21 java-25 java-27" \
        "{\"version\": \"17\", $l, $e}, {\"version\": \"21\", $l}, {\"version\": \"25\", $l}, {\"version\": \"27\"}" java
    mm "java min+max: newest is LTS" "java-17 java-25" \
        "{\"version\": \"17\", $l}, {\"version\": \"21\", $l}, {\"version\": \"25\", $l}" java
    mm "java min+max: newest is EOL" "java-17 java-25" \
        "{\"version\": \"17\", $l}, {\"version\": \"25\", $l}, {\"version\": \"26\", $e}" java
    mm "java min+max: LTS is the oldest" "java-17 java-27" \
        "{\"version\": \"17\", $l}, {\"version\": \"26\"}, {\"version\": \"27\"}" java
    mm "java min+max: no LTS" "java-17 java-27" \
        "{\"version\": \"17\"}, {\"version\": \"27\"}" java
    mm "java min+max: all EOL keeps two" "java-17 java-25" \
        "{\"version\": \"17\", $l, $e}, {\"version\": \"21\", $l, $e}, {\"version\": \"25\", $l, $e}" java

    return $fail
}

if [ "${1:-}" = --self-test ]; then
    self_test
    exit
fi

matrix "$1" "${2:-}"
