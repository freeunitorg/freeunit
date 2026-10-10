#!/bin/sh
#
# Copyright (C) FreeUnit contributors.
#
# This script compares the layout of the structs in
# tools/perf/layout-structs.txt between two commits.
#
#   tools/perf/layout-check.sh BASE [HEAD]
#
# HEAD is "HEAD" if you do not give it.  For each commit, the script copies
# the tree to a temporary directory and builds unitd and nxt_unit.o there,
# with debug information.  Then it runs pahole on each struct and compares
# the two results.  For a "shm" struct, pahole also shows the layout of
# the structs in it.  For a "const" macro, a small program that is built
# with the headers of the commit prints the value.
#
#   - A change to a "shm" struct fails.  Processes that are built from
#     different commits share these structs, so the change breaks the ABI
#     between them.  If the change is necessary, explain it in the pull
#     request.
#   - A change to a "const" value fails, for the same reason.
#   - A change to a "local" struct gives a warning.
#   - An entry that is in the BASE list and not in the HEAD list fails.
#     A "shm" or "const" entry with an other kind in the HEAD list also
#     fails.
#   - A struct that pahole does not find in the HEAD build fails.  So does
#     a "const" macro that the HEAD headers do not define.
#   - A "shm" struct with a different layout in unitd and in nxt_unit.o of
#     the HEAD build fails.
#
# Exit status: 0 if the check passes, 1 if a layout check fails, 2 for an
# error of the check itself (a bad argument, a bad line in the list, a
# failed build).  CC selects the compiler.  PAHOLE selects pahole, which is
# in the dwarves package.  The builds go to a directory that mktemp makes
# in TMPDIR (default /tmp); it must be writable and hold two builds.

set -eu

LIST=tools/perf/layout-structs.txt
PAHOLE=${PAHOLE:-pahole}

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "usage: $0 BASE [HEAD]" >&2
    exit 2
fi

cd "$(dirname "$0")/../.."

base=$(git rev-parse --verify "$1^{commit}")
head=$(git rev-parse --verify "${2:-HEAD}^{commit}")

command -v "$PAHOLE" > /dev/null || {
    echo "error: $PAHOLE not found" >&2
    exit 2
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
trap 'exit 2' HUP INT TERM


# list COMMIT: the "kind name" lines of the list in COMMIT.  The output is
# empty if COMMIT has no list.

list() {
    git cat-file -e "$1:$LIST" 2> /dev/null || return 0

    git show "$1:$LIST" | awk -v file="$1:$LIST" '
        { sub(/#.*/, "") }
        NF == 0 { next }
        NF != 2 || ($1 != "shm" && $1 != "local" && $1 != "const") || seen[$2]++ {
            print "::error::" file ": bad line: " $0 > "/dev/stderr"
            exit 2
        }
        { print $1, $2 }'
}


# build COMMIT DIR: build unitd and nxt_unit.o of COMMIT in DIR.  Then
# build and run a program that prints the "const" values of the HEAD list
# with the headers of COMMIT, to DIR.const.  A macro that the headers do
# not define is not printed.

build() {
    echo "build $1"

    mkdir "$2"
    git archive --format=tar -o "$2.tar" "$1"
    tar -xf "$2.tar" -C "$2"

    # --no-regex: the build needs no PCRE library.  E=0: a new compiler
    # warning in an old commit must not stop the check.
    (cd "$2" && ./configure --no-regex \
     && make -j"$(nproc)" E=0 build/sbin/unitd build/src/nxt_unit.o) \
        > "$2.log" 2>&1 || {
        tail -n 50 "$2.log"
        echo "error: the build of $1 failed" >&2
        exit 2
    }

    {
        echo '#include <nxt_main.h>'
        echo '#include <nxt_port_memory_int.h>'
        echo '#include <stdio.h>'
        echo 'int main(void) {'
        awk '$1 == "const" {
            printf "#ifdef %s\n", $2
            printf "printf(\"%%s %%llu\\n\", \"%s\", ", $2
            printf "(unsigned long long) (%s));\n#endif\n", $2
        }' "$work/head.list"
        echo 'return 0; }'
    } > "$2/layout_const.c"

    (cd "$2" && "${CC:-cc}" -funsigned-char -std=gnu11 -I src -I build/include \
         -o layout_const layout_const.c && ./layout_const) \
        > "$2.const" 2> "$2.log" || {
        cat "$2.log"
        echo "error: the const program of $1 failed" >&2
        exit 2
    }
}


# layout DIR KIND NAME: the pahole output for NAME in the build in DIR.
# The output is empty if the build does not have NAME.  nxt_unit.o is the
# libunit object.  It is not in unitd, and it has nxt_unit_ctx_impl_s.
#
# pahole -C stops at the first file that has NAME, thus each file is read
# on its own.  The output has a "## file" line before the layout from that
# file.  For a "shm" struct, the two layouts must be the same: unitd and
# an application that uses libunit share it.

layout() {
    if [ "$2" = const ]; then
        awk -v name="$3" '$1 == name' "$1.const"
        return
    fi

    expand=--expand_types
    [ "$2" = shm ] || expand=

    for f in sbin/unitd src/nxt_unit.o; do
        # shellcheck disable=SC2086
        "$PAHOLE" -F dwarf $expand -C "$3" "$1/build/$f" \
            > "$work/one" 2> "$work/err" || {
            cat "$work/err" >&2
            echo "error: pahole failed on $1/build/$f" >&2
            exit 2
        }

        if [ -s "$work/one" ]; then
            echo "## ${f##*/}"
            cat "$work/one"
            cp "$work/one" "$work/one.${f##*/}"
        else
            rm -f "$work/one.${f##*/}"
        fi
    done

    if [ "$2" = shm ] && [ -f "$work/one.unitd" ] \
       && [ -f "$work/one.nxt_unit.o" ] \
       && ! cmp -s "$work/one.unitd" "$work/one.nxt_unit.o"
    then
        echo "## MISMATCH: unitd and nxt_unit.o differ"
    fi
}


list "$base" > "$work/base.list"
list "$head" > "$work/head.list"

[ -s "$work/head.list" ] || {
    echo "error: $head has no structs in $LIST" >&2
    exit 2
}

echo "base: $(git log -1 --format='%h %s' "$base")"
echo "head: $(git log -1 --format='%h %s' "$head")"
"${CC:-cc}" --version | head -n 1
"$PAHOLE" --version

build "$base" "$work/base"
build "$head" "$work/head"

fail=
warn=
skip=

while read -r kind name <&3; do
    layout "$work/base" "$kind" "$name" > "$work/old"
    layout "$work/head" "$kind" "$name" > "$work/new"

    if [ ! -s "$work/new" ]; then
        echo "::error::$name is not in the build of the head commit"
        fail="$fail $name"

    elif grep -q '^## MISMATCH' "$work/new"; then
        echo "::error::$name: unitd and nxt_unit.o of the head commit have different layouts"
        cat "$work/new"
        fail="$fail $name"

    elif [ ! -s "$work/old" ]; then
        echo "::warning::$name ($kind) is not in the base build; its layout was not compared"
        skip="$skip $name"

    elif cmp -s "$work/old" "$work/new"; then
        echo "same:    $name ($kind)"

    else
        echo "changed: $name ($kind)"
        diff -u -L "$name (base)" -L "$name (head)" \
            "$work/old" "$work/new" || true

        case $kind in
            shm|const) fail="$fail $name" ;;
            *)   warn="$warn $name" ;;
        esac
    fi
done 3< "$work/head.list"

while read -r kind name <&3; do
    now=$(awk -v name="$name" '$2 == name { print $1 }' "$work/head.list")

    if [ -z "$now" ]; then
        echo "::error::$name is not in $LIST any more"
        fail="$fail $name"

    elif [ "$kind" != local ] && [ "$now" != "$kind" ]; then
        echo "::error::$name is not $kind in $LIST any more"
        fail="$fail $name"
    fi
done 3< "$work/base.list"

if [ -n "$warn" ]; then
    echo "::warning::The layout of process-local structs changed:$warn"
fi

if [ -n "$skip" ]; then
    echo "::warning::Not compared, absent in the base build:$skip"
fi

if [ -n "$fail" ]; then
    echo "::error::The layout check failed:$fail (see $0)"
    exit 1
fi

if [ -n "$skip" ]; then
    echo "The compared shared-memory structs did not change;$skip was not checked."
else
    echo "The layout of the shared-memory structs did not change."
fi
