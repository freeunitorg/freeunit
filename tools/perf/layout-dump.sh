#!/bin/sh
#
# Copyright (C) FreeUnit contributors.
#
# Prints the libunit ABI that an embedder mirrors when it calls libunit
# from another language: the layout of each struct in the installed
# headers, with the offset and the size of each member, and the value of
# each enum constant.  Compare it with your mirror, or diff the output of
# two releases on the same platform.  docs/libunit-abi.md says which parts
# an embedder uses.
#
#   tools/perf/layout-dump.sh [BUILD]
#
# BUILD is the build directory of a configured tree ("build" if you do not
# give it).  The script makes BUILD/src/nxt_unit.o, the libunit object, and
# runs pahole on it; configure builds with -g.  The structs and the enum
# constants come from tools/perf/layout-coverage.py --list, so a struct
# added to an installed header is printed with no change here.  A small
# program built with the installed headers prints the constants.
#
# The output depends on the compiler and on the platform: a mirror for
# another one needs the output from there.
#
# Exit status: 0, or 2 for an error (no pahole, no configured tree, a
# struct that pahole does not find).  CC selects the compiler of the
# program.  PAHOLE selects pahole, which is in the dwarves package.

set -eu

PAHOLE=${PAHOLE:-pahole}

cd "$(dirname "$0")/../.."

build=${1:-build}

command -v "$PAHOLE" > /dev/null || {
    printf 'error: %s not found\n' "$PAHOLE" >&2
    exit 2
}

[ -f "$build/Makefile" ] || {
    printf 'error: no %s/Makefile; run ./configure first\n' "$build" >&2
    exit 2
}

make -s "$build/src/nxt_unit.o" > /dev/null || {
    printf 'error: cannot make %s/src/nxt_unit.o\n' "$build" >&2
    exit 2
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
trap 'exit 2' HUP INT TERM

tools/perf/layout-coverage.py --list > "$work/list" || exit 2

# The program: the version, the protocol, the data model, and the enum
# constants, with the installed headers only.
{
    awk '{ print $3 }' "$work/list" | awk '!seen[$0]++' \
        | sed 's|.*/||; s|.*|#include "&"|'
    # printf, not echo: the echo of dash expands the "\n" below.
    printf '%s\n' '#include <stdio.h>' \
        'int main(void) {' \
        'printf("# libunit %s, protocol %d, %d-bit pointers\n",' \
        '       NXT_VERSION, NXT_UNIT_PROTOCOL_VERSION,' \
        '       (int) (8 * sizeof(void *)));'
    awk '$1 == "enum" {
        printf "printf(\"%%-40s %%lld\\n\", \"%s\", (long long) (%s));\n",
               $2, $2
    }' "$work/list"
    echo 'return 0; }'
} > "$work/abi.c"

"${CC:-cc}" -std=gnu11 -funsigned-char -I src -I "$build/include" \
    -o "$work/abi" "$work/abi.c" || {
    echo "error: the constants program does not build" >&2
    exit 2
}

"$work/abi" > "$work/const"

head -n 1 "$work/const"
printf '# %s, %s, pahole %s\n\n' "$(uname -m)" \
    "$("${CC:-cc}" --version | head -n 1)" "$("$PAHOLE" --version)"

awk '$1 == "struct" { print $2 }' "$work/list" | while read -r name; do
    "$PAHOLE" -F dwarf -C "$name" "$build/src/nxt_unit.o" > "$work/one" || {
        printf 'error: pahole failed on %s/src/nxt_unit.o\n' "$build" >&2
        exit 2
    }

    [ -s "$work/one" ] || {
        printf 'error: %s is not in %s/src/nxt_unit.o\n' "$name" \
            "$build" >&2
        exit 2
    }

    cat "$work/one"
    echo
done

echo "# enum constants"
tail -n +2 "$work/const"
