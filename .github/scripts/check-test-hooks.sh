#!/bin/sh
#
# Check that the NXT_TESTS hooks reach the test programs and nothing else.
#
# A "hook" is a fault-injection branch or counter compiled into the core or
# into libunit only for the C test suite, inside "#if (NXT_TESTS)".  With
# "./configure --tests", "make tests" compiles every core and libunit source
# twice: build/src/X.o without the macro and build/src/X.test.o with it
# (auto/make).  unitd, libnxt.a, libunit.a and the language modules are built
# from the plain objects; the test programs link libnxt-tests.a and
# libunit-tests.a, built from the instrumented ones.
#
# The script derives everything from the build.  It keeps no list of hook
# names or hooked sources, so a new hook needs no change here.
#
# hook symbols:  the symbols defined in X.test.o and not in X.o.  Names that
#                contain a "." are compiler-made clones (foo.isra.0, foo.cold)
#                and are skipped; a hook is a C identifier.
# members:       the global symbols of X.test.o.  A program that defines one
#                of them linked object X, from one of the two archives.
#
# Checks, in order:
#
#   0. NXT_TESTS is not in build/include/nxt_auto_config.h, and at least one
#      hook symbol exists.  If the macro leaked into the plain objects, X.o and
#      X.test.o are the same and the hook set is empty; that is a failure, not
#      a pass.
#   1. Every src/**/*.c outside src/test/ that mentions NXT_TESTS has a
#      build/src/X.test.o, and that object differs from X.o once debug info
#      is removed.  A hook in a source that is compiled once (for example
#      src/nxt_main.c or a module source) would ship; a plain object that
#      equals its instrumented twin was compiled with the macro.
#   2. No shipped artifact defines a hook symbol: build/sbin/unitd,
#      build/lib/libnxt.a, build/lib/libunit.a, build/src/nxt_unit.o (the
#      object the language modules link), and, when present, build/lib/libnxt.so
#      and every build/*.unit.so.
#   3. Every test program that linked object X defines every hook of X.  This
#      proves it took X from the instrumented archive, not the plain one.
#
# Usage: .github/scripts/check-test-hooks.sh [build-dir]
#
# Run it after "./configure --tests", "make" and "make tests".  It needs
# nm(1), objcopy(1) and cmp(1).

set -eu

# sort(1) and comm(1) must agree on the collation order
LC_ALL=C
export LC_ALL

build=${1:-build}

tmp=$(mktemp -d)
finished=

# With "set -e" a failing nm, comm or awk would end the script without a
# word; say so, and only then is a silent exit impossible.
finish()
{
    status=$?
    rm -rf "$tmp"

    if [ -z "$finished" ] && [ $status -ne 0 ]; then
        echo "::error::check-test-hooks.sh aborted with status $status" >&2
    fi
}

trap finish EXIT

fail=0

error()
{
    echo "::error::$*" >&2
    fail=1
}

# defined symbol names of one file (an object, an archive or a program)
names()
{
    nm --defined-only -p "$1" 2>/dev/null | awk 'NF >= 3 { print $NF }' \
        | sort -u
}

# defined global symbol names of one object
global_names()
{
    nm --defined-only -p "$1" 2>/dev/null \
        | awk 'NF >= 3 && $(NF - 1) ~ /^[A-Z]$/ { print $NF }' | sort -u
}


# 0. The macro is per object, and the derivation works.

if grep -q 'NXT_TESTS' "$build/include/nxt_auto_config.h"; then
    error "NXT_TESTS is defined in $build/include/nxt_auto_config.h;" \
          "it must stay a per-object macro (see auto/make)"
fi

test_objs=$(find "$build/src" -name '*.test.o' | sort)

if [ -z "$test_objs" ]; then
    error "no *.test.o under $build/src; run 'make tests' first"
    finished=1
    exit 1
fi

: > "$tmp/hooks"

for t in $test_objs; do
    p=${t%.test.o}.o

    if [ ! -f "$p" ]; then
        error "$t has no plain object $p"
        continue
    fi

    names "$p" > "$tmp/plain"
    names "$t" > "$tmp/instr"

    # symbols only the instrumented object defines
    comm -13 "$tmp/plain" "$tmp/instr" | grep -v '\.' > "$tmp/hooks.$$" || true

    if [ -s "$tmp/hooks.$$" ]; then
        rel=${t#"$build"/}
        rel=${rel%.test.o}
        awk -v o="$rel" '{ print o, $0 }' "$tmp/hooks.$$" >> "$tmp/hooks"
    fi
done

nhooks=$(wc -l < "$tmp/hooks")

if [ "$nhooks" -eq 0 ]; then
    error "no symbol differs between the plain and the instrumented objects;" \
          "NXT_TESTS leaked into the plain build or is missing from the" \
          "instrumented one"
    finished=1
    exit 1
fi

echo "hook symbols: $nhooks"
sed 's/^/  /' "$tmp/hooks"


# 1. Every hooked source is compiled twice, and the two objects differ.
#
# The symbol diff above cannot see a hook that adds no symbol, and it goes
# empty for an object that was compiled with the macro both times.  So for
# every source that tests NXT_TESTS in a preprocessor directive the plain and
# the instrumented object, debug info removed, must not be the same file.  A
# mention in a comment is not a hook and is not counted.

for src in $(find src -name '*.c' -not -path 'src/test/*' | sort); do
    grep -Eq '^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif)\b.*NXT_TESTS' \
        "$src" || continue

    p=$build/${src%.c}.o
    t=$build/${src%.c}.test.o

    if [ ! -f "$t" ]; then
        error "$src tests NXT_TESTS but is compiled once, without the" \
              "macro; its hooks would ship or never link.  Hooks belong in" \
              "a source of NXT_LIB_SRCS or NXT_LIB_UNIT_SRCS (auto/sources)"
        continue
    fi

    objcopy --strip-debug "$p" "$tmp/plain.o"
    objcopy --strip-debug "$t" "$tmp/instr.o"

    if cmp -s "$tmp/plain.o" "$tmp/instr.o"; then
        error "$src tests NXT_TESTS but $p and $t are the same object;" \
              "the plain object was compiled with the macro"
    fi
done


# 2. Shipped artifacts carry no hook symbol.
#
# A static function that the plain compile inlines and the instrumented one
# does not is in the hook set too.  Another plain object may define a static
# of the same name, and that name is then legitimately in unitd.  Such names
# cannot be checked by name; they are reported and left out.  This hides no
# leak: a plain object compiled with the macro is caught by check 0, because
# auto/make has one rule template for all plain objects.

awk '{ print $2 }' "$tmp/hooks" | sort -u > "$tmp/hooknames"

for t in $test_objs; do
    names "${t%.test.o}.o"
done | sort -u > "$tmp/plainnames"

ambiguous=$(comm -12 "$tmp/hooknames" "$tmp/plainnames")

if [ -n "$ambiguous" ]; then
    echo "not checked by name, defined by another plain object:" \
         "$(echo $ambiguous)"
    comm -23 "$tmp/hooknames" "$tmp/plainnames" > "$tmp/hooknames.$$"
    mv "$tmp/hooknames.$$" "$tmp/hooknames"
fi

shipped="$build/sbin/unitd $build/lib/libnxt.a $build/lib/libunit.a \
    $build/src/nxt_unit.o"

if [ -f "$build/lib/libnxt.so" ]; then
    shipped="$shipped $build/lib/libnxt.so"
fi

shipped="$shipped $(find "$build" -name '*.unit.so' | sort)"

for f in $shipped; do
    if [ ! -f "$f" ]; then
        error "$f is missing; run 'make', 'make $build/lib/libunit.a' and" \
              "'make tests' first"
        continue
    fi

    names "$f" > "$tmp/defined"

    leaked=$(comm -12 "$tmp/hooknames" "$tmp/defined")

    if [ -n "$leaked" ]; then
        error "$f defines NXT_TESTS hook symbols: $(echo $leaked)"
    fi
done


# 3. The test programs took every linked object from the instrumented archive.

programs="$build/tests $build/ncq_test $build/vbcq_test $build/unit_app_test \
    $build/unit_close_test $build/unit_port_recv_test \
    $build/unit_websocket_chat $build/unit_websocket_echo"

for prog in $programs; do
    if [ ! -f "$prog" ]; then
        error "$prog is missing; run 'make tests' first"
        continue
    fi

    names "$prog" > "$tmp/defined"

    for t in $test_objs; do
        rel=${t#"$build"/}
        rel=${rel%.test.o}

        awk -v o="$rel" '$1 == o { print $2 }' "$tmp/hooks" | sort -u \
            > "$tmp/want"

        [ -s "$tmp/want" ] || continue

        global_names "$t" > "$tmp/members"

        # not linked at all: nothing to prove
        [ -n "$(comm -12 "$tmp/members" "$tmp/defined")" ] || continue

        missing=$(comm -23 "$tmp/want" "$tmp/defined")

        if [ -n "$missing" ]; then
            error "$prog linked $rel from the plain archive:" \
                  "missing hook symbols $(echo $missing)"
        fi
    done
done


finished=1

if [ $fail -ne 0 ]; then
    exit 1
fi

echo "ok: hooks are in the test programs only"
