#!/bin/sh
#
# Copyright (C) FreeUnit contributors.
#
# This script compares the layout of the structs in
# tools/perf/layout-structs.txt between two commits.
#
#   tools/perf/layout-check.sh BASE [HEAD]
#   tools/perf/layout-check.sh --self-test
#
# HEAD is "HEAD" if you do not give it.  For each commit, the script copies
# the tree to a temporary directory and builds unitd and nxt_unit.o there,
# with debug information, once as is and once with -DNXT_MMAP_TINY_CHUNK:
# that macro selects a second shared-memory geometry, and a struct can
# differ between the two arms.  Then it runs pahole on each struct in each
# arm and compares the results of the two commits, arm by arm.  The two
# arms are not compared with each other: nxt_port_mmap_header_s differs
# between them by design, and a process of one arm never maps the memory
# of the other.  For a "shm" struct, pahole also shows the layout of
# the structs in it.  For a "const" macro, a small program that is built
# with the headers of the commit prints its value in each geometry arm;
# the same program prints the value of each "enum" constant.
#
#   - A change to a "shm" struct fails.  Processes that are built from
#     different commits share these structs, so the change breaks the ABI
#     between them.  If the change is necessary, explain it in the pull
#     request.
#   - A change to a "const" value or an "enum" constant fails, for the
#     same reason.
#   - A change to a "local" struct fails too.  It is in the memory of one
#     process, but a module is built against the installed headers and
#     lays the struct out itself, so a module built from the other commit
#     reads it wrong.
#   - An entry that is in the BASE list and not in the HEAD list fails.
#     A "shm" or "const" entry with an other kind in the HEAD list also
#     fails.
#   - A struct that pahole does not find in the HEAD build fails.  So does
#     a "const" macro that the HEAD headers do not define.
#   - A "shm" struct with a different layout in unitd and in nxt_unit.o of
#     the HEAD build, in either arm, fails.
#
# The output for a change names the arm or arms that changed.  --self-test
# runs the comparison on fixed layouts, with no build and no pahole.
#
# The list must also cover every struct defined in a header that "make
# install" copies, as a "local" entry if no process shares it: a module is
# built against those headers and lays the struct out itself, so a change
# to one of them breaks the module ABI as a change to a shared struct
# breaks the process ABI, and it fails in the same way.
# tools/perf/layout-coverage.py checks that half, and the layout job runs
# it before this comparison.
#
# Exit status: 0 if the check passes, 1 if a layout check fails, 2 for an
# error of the check itself (a bad argument, a bad line in the list, a
# failed build).  CC selects the compiler.  PAHOLE selects pahole, which is
# in the dwarves package.  The builds go to a directory that mktemp makes
# in TMPDIR (default /tmp); it must be writable and hold four builds.

set -eu

LIST=tools/perf/layout-structs.txt
PAHOLE=${PAHOLE:-pahole}
ARMS="default tiny"

self_test=
if [ $# -eq 1 ] && [ "$1" = --self-test ]; then
    self_test=1

elif [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "usage: $0 BASE [HEAD]" >&2
    exit 2
fi

cd "$(dirname "$0")/../.."

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
trap 'exit 2' HUP INT TERM


# arm FILE ARM: the part of the layout in FILE that belongs to ARM.  A
# layout is a sequence of blocks, and each block starts with a
# "## ARM ..." line.

arm() {
    awk -v arm="$2" '/^## / { on = ($2 == arm) } on' "$1"
}


# verdict KIND NAME OLD NEW: compare the layouts of NAME in the base build
# (OLD) and in the head build (NEW), and print the result.  A failure is
# added to $fail, and an entry that the base build does not have to $skip.

verdict() {
    if [ ! -s "$4" ]; then
        echo "::error::$2 is not in the build of the head commit"
        fail="$fail $2"

    elif grep -q '^## MISMATCH' "$4"; then
        echo "::error::$2: unitd and nxt_unit.o of the head commit have different layouts"
        cat "$4"
        fail="$fail $2"

    elif [ ! -s "$3" ]; then
        echo "::warning::$2 ($1) is not in the base build; its layout was not compared"
        skip="$skip $2"

    elif cmp -s "$3" "$4"; then
        echo "same:    $2 ($1)"

    else
        changed=
        for a in $ARMS; do
            arm "$3" "$a" > "$work/arm.old"
            arm "$4" "$a" > "$work/arm.new"
            cmp -s "$work/arm.old" "$work/arm.new" || changed="$changed $a"
        done

        echo "changed: $2 ($1), arm:$changed"
        diff -u -L "$2 (base)" -L "$2 (head)" "$3" "$4" || true
        fail="$fail $2"
    fi
}


# The self-test runs verdict on layouts with a known result.  Each case
# gives the kind, the base and the head layout, and the result.

if [ -n "$self_test" ]; then
    printf '## default unitd\nsize: 8\n## tiny unitd\nsize: 8\n' > "$work/t.a"
    printf '## default unitd\nsize: 8\n## tiny unitd\nsize: 16\n' > "$work/t.tiny"
    printf '## default unitd\nsize: 16\n## tiny unitd\nsize: 8\n' > "$work/t.default"
    { cat "$work/t.a"; echo '## MISMATCH tiny: unitd and nxt_unit.o differ'; } \
        > "$work/t.mismatch"
    : > "$work/t.none"

    bad=0
    while read -r kind old new want arms; do
        fail='' skip=''
        verdict "$kind" s "$work/t.$old" "$work/t.$new" > "$work/t.out"
        got=ok
        [ -z "$skip" ] || got=skip
        [ -z "$fail" ] || got=fail

        if [ "$got" != "$want" ]; then
            echo "FAIL: $kind $old -> $new: $got, expected $want"
            cat "$work/t.out"
            bad=1

        elif [ -n "$arms" ] \
             && ! grep -qx "changed: s ($kind), arm: $arms" "$work/t.out"
        then
            echo "FAIL: $kind $old -> $new: the change is not in the $arms arm"
            cat "$work/t.out"
            bad=1

        else
            echo "ok:   $kind $old -> $new: $got"
        fi
    done <<'END'
shm   a       a        ok
shm   a       tiny     fail  tiny
shm   a       default  fail  default
shm   a       mismatch fail
shm   none    a        skip
shm   a       none     fail
const a       tiny     fail  tiny
local a       a        ok
local a       tiny     fail  tiny
local a       default  fail  default
local none    a        skip
END

    exit $bad
fi

base=$(git rev-parse --verify "$1^{commit}")
head=$(git rev-parse --verify "${2:-HEAD}^{commit}")

command -v "$PAHOLE" > /dev/null || {
    echo "error: $PAHOLE not found" >&2
    exit 2
}

# list COMMIT: the "kind name" lines of the list in COMMIT.  The output is
# empty if COMMIT has no list.

list() {
    git cat-file -e "$1:$LIST" 2> /dev/null || return 0

    git show "$1:$LIST" | awk -v file="$1:$LIST" '
        { sub(/#.*/, "") }
        NF == 0 { next }
        NF != 2 || ($1 != "shm" && $1 != "local" && $1 != "const" && $1 != "enum") || seen[$2]++ {
            print "::error::" file ": bad line: " $0 > "/dev/stderr"
            exit 2
        }
        { print $1, $2 }'
}


# build COMMIT DIR LIST: build unitd and nxt_unit.o of COMMIT in DIR, and
# again with -DNXT_MMAP_TINY_CHUNK in DIR.tiny.  Then
# build and run a program that prints the "const" values of the HEAD list
# with the headers of COMMIT, to DIR.const.  The geometry macros have a
# definition in each NXT_MMAP_TINY_CHUNK arm, so the program is built
# twice, and each value is tagged with its arm.  A macro that an arm does
# not define prints no line for it.

build() {
    echo "build $1"

    mkdir "$2"
    git archive --format=tar -o "$2.tar" "$1"
    tar -xf "$2.tar" -C "$2"

    cp -R "$2" "$2.tiny"

    # --no-regex: the build needs no PCRE library.  E=0: a new compiler
    # warning in an old commit must not stop the check.  The tiny arm is
    # set with --cc-opt, so every object of that build sees the macro.
    for a in $ARMS; do
        d=$2 opt=
        [ "$a" = default ] || { d=$2.$a; opt=-DNXT_MMAP_TINY_CHUNK; }

        (cd "$d" && ./configure --no-regex ${opt:+--cc-opt="$opt"} \
         && make -j"$(nproc)" E=0 build/sbin/unitd build/src/nxt_unit.o \
            build/lib/libunit.a) \
            > "$d.log" 2>&1 || {
            tail -n 50 "$d.log"
            echo "error: the build of $1 ($a) failed" >&2
            exit 2
        }
    done

    {
        echo '#include <nxt_main.h>'
        echo '#include <nxt_port_memory_int.h>'
        echo '#include <nxt_unit.h>'
        echo '#include <nxt_unit_field.h>'
        echo '#include <nxt_websocket_header.h>'
        echo '#include <stdio.h>'
        echo 'int main(void) {'
        awk '$1 == "const" {
            printf "#ifdef %s\n", $2
            printf "printf(\"%%s %%llu\\n\", \"%s\", ", $2
            printf "(unsigned long long) (%s));\n#endif\n", $2
        }' "$work/head.list"
        # An enum constant is not a macro, so #ifdef cannot test it.  The
        # constants come from the list of COMMIT (and the HEAD list), so a
        # constant that COMMIT does not have is not compiled in, and is
        # reported as not in that build.
        awk 'NR == FNR { if ($1 == "enum") want[$2] = 1; next }
             $1 == "enum" && want[$2] {
            printf "printf(\"%%s %%llu\\n\", \"%s\", ", $2
            printf "(unsigned long long) (%s));\n", $2
        }' "$3" "$work/head.list"
        echo 'return 0; }'
    } > "$2/layout_const.c"

    (cd "$2" && "${CC:-cc}" -funsigned-char -std=gnu11 -I src -I build/include \
         -o layout_const layout_const.c \
         && ./layout_const | sed 's/^/default /') \
        > "$2.const" 2> "$2.log" || {
        cat "$2.log"
        echo "error: the const program of $1 (default) failed" >&2
        exit 2
    }

    # The tiny arm is a separate compilation: a macro definition chosen by
    # #ifdef NXT_MMAP_TINY_CHUNK is not reached in the build above, so a
    # change to it would otherwise read as "same" here.
    #
    # The pipe to sed hides the exit status of the program: a program that
    # dies after printing some lines is not reported here, and the missing
    # value is then reported as "is not in the build of the head commit".
    # A macro that the headers do not define prints no line for it, and a
    # macro that is defined but is not a value is a compile error.
    #
    # The log is appended to, not truncated, as in the build above, so
    # that no compiler output of this function is discarded.
    (cd "$2" && "${CC:-cc}" -funsigned-char -std=gnu11 -DNXT_MMAP_TINY_CHUNK \
         -I src -I build/include \
         -o layout_const layout_const.c \
         && ./layout_const | sed 's/^/tiny /') \
        >> "$2.const" 2>> "$2.log" || {
        cat "$2.log"
        echo "error: the const program of $1 (tiny) failed" >&2
        exit 2
    }
}


# layout DIR KIND NAME: the pahole output for NAME in the builds in DIR
# and DIR.tiny.  The output is empty if the builds do not have NAME.  nxt_unit.o is the
# libunit object.  It is not in unitd, and it has nxt_unit_ctx_impl_s.
#
# pahole -C stops at the first file that has NAME, thus each file is read
# on its own.  The output has a "## file" line before the layout from that
# arm and file.  For a "shm" struct, the two layouts of an arm must be the
# same: unitd and an application that uses libunit share it.

layout() {
    if [ "$2" = const ] || [ "$2" = enum ]; then
        # DIR.const holds "<arm> <name> <value>" lines for both arms, so
        # both are returned for the macro: the comparison below then sees
        # a change to either the default or the tiny geometry.  The name
        # is in the second field, because the arm tag is the first.
        # The lines are put in "## ARM" blocks, as for a struct.
        awk -v name="$3" '$2 == name {
            if ($1 != last) print "## " $1
            last = $1
            print
        }' "$1.const"
        return
    fi

    expand=--expand_types
    [ "$2" = shm ] || expand=

    for a in $ARMS; do
        d=$1
        [ "$a" = default ] || d=$1.$a

        for f in sbin/unitd src/nxt_unit.o; do
            # shellcheck disable=SC2086
            "$PAHOLE" -F dwarf $expand -C "$3" "$d/build/$f" \
                > "$work/one" 2> "$work/err" || {
                cat "$work/err" >&2
                echo "error: pahole failed on $d/build/$f" >&2
                exit 2
            }

            if [ -s "$work/one" ]; then
                echo "## $a ${f##*/}"
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
            echo "## MISMATCH $a: unitd and nxt_unit.o differ"
        fi
    done
}


# strip_comments: the headers without their comments.  A usage example in
# src/nxt_unit.h names a static helper (nxt_unit_process_msg), and a name in
# a comment is not a declaration.

strip_comments() {
    awk '
        {
            out = ""
            i = 1
            while (i <= length($0)) {
                two = substr($0, i, 2)
                if (block) {
                    if (two == "*/") { block = 0; i += 2 } else { i++ }
                    continue
                }
                if (two == "/*") { block = 1; i += 2; continue }
                if (two == "//") { break }
                out = out substr($0, i, 1)
                i++
            }
            print out
        }'
}


# symbols DIR: the functions the installed headers of DIR declare and the
# object exports, one per line.  A module or an embedder links those by
# name: a name that the base has and the head does not is a break, even
# when the tree itself builds.  The archive is read, not one object, so
# moving a function between the objects of libunit.a stays free.  A static
# function is not exported, thus it is not in the second set, and renaming
# a helper stays free.

symbols() {
    cat "$1"/src/nxt_unit*.h 2>/dev/null \
        | strip_comments \
        | grep -hoE '\bnxt_unit_[a-z0-9_]+\(' \
        | tr -d '(' | sort -u > "$work/declared"

    nm --extern-only --defined-only --format=posix \
        "$1/build/lib/libunit.a" 2>/dev/null \
        | awk '$2 ~ /^[TDBR]$/ { print $1 }' | sort -u > "$work/defined"

    grep -F -x -f "$work/defined" "$work/declared" || true
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

build "$base" "$work/base" "$work/base.list"
build "$head" "$work/head" "$work/head.list"

fail=
skip=
wire=

symbols "$work/base" > "$work/base.sym"
symbols "$work/head" > "$work/head.sym"

while read -r name <&3; do
    grep -q -x -F "$name" "$work/head.sym" && continue

    echo "::error::$name is declared in the installed headers of the base and not defined by the head build"
    fail="$fail $name"
done 3< "$work/base.sym"

while read -r kind name <&3; do
    layout "$work/base" "$kind" "$name" > "$work/old"
    layout "$work/head" "$kind" "$name" > "$work/new"
    before=$fail
    verdict "$kind" "$name" "$work/old" "$work/new"
    [ "$fail" = "$before" ] || [ "$kind" = local ] || wire=1
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

if [ -n "$skip" ]; then
    echo "::warning::Not compared, absent in the base build:$skip"
fi

if [ -n "$fail" ]; then
    echo "::error::The layout check failed:$fail (see $0)"

    # A "local" struct is in the memory of one process: no protocol.
    if [ -n "$wire" ]; then
        echo "A shm, const or enum entry changed: the change needs a new" \
             "NXT_UNIT_PROTOCOL_VERSION in src/nxt_unit.h."
    fi

    exit 1
fi

if [ -n "$skip" ]; then
    echo "The compared layouts did not change;$skip was not checked."
else
    echo "The listed layouts did not change."
fi
