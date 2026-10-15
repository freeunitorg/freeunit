#!/usr/bin/env python3

# Copyright (C) FreeUnit contributors.
#
# Checks that every struct and union a language module can see has an entry in
# tools/perf/layout-structs.txt, so that tools/perf/layout-check.sh watches
# it.
#
# A module is built against the headers that "make install" copies, and it
# lays those structs out itself.  A union is the same: pahole -C dumps it in
# the same layout form, and a union that is nested only in "local" structs
# is compared nowhere else.  A change under a module that was built
# against the old header is silent: the module reads the wrong offsets.
# The list is what makes the layout check look, and this script is what
# keeps the list complete.
#
#   tools/perf/layout-coverage.py [--self-test]
#
# The headers come from the install rule in auto/make, so a new installed
# header needs no edit here.  A new struct in one of them fails this check
# until it is listed: "local" unless both processes read it through shared
# memory, in which case it is "shm".
#
# Run it from the repository root.
#
# Exit status: 0 when every struct has an entry, 1 when one is missing,
# 2 for an error of the check itself.

import re
import sys
from pathlib import Path

LIST = Path('tools/perf/layout-structs.txt')
MAKE = Path('auto/make')

# The install rule names the first header; the rest follow, one per line,
# each but the last ending in a backslash.
FIRST = 'install -p -m u=rw,go=r src/nxt_unit.h'
# The start of a definition: "struct tag {", "union tag {",
# "typedef struct tag {", or "typedef struct {".  The body is then read by
# counting braces, so a nested struct or union does not end it.
OPEN = re.compile(r'\b(typedef\s+)?(struct|union)\s+(?:([A-Za-z_]\w*)\s*)?\{')
NAME = re.compile(r'\s*([A-Za-z_]\w*)\s*;')
COMMENT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)


def die(message):
    print(f'layout-coverage: {message}', file=sys.stderr)
    sys.exit(2)


def installed_headers():
    """The headers "make install" copies, in the order of the rule."""

    if not MAKE.exists():
        die(f'no {MAKE}')

    lines = MAKE.read_text().splitlines()
    headers = []

    for i, line in enumerate(lines):
        if FIRST not in line:
            continue

        while True:
            headers += re.findall(r'src/[A-Za-z0-9_]+\.h', lines[i])

            if not lines[i].rstrip().endswith('\\'):
                break

            i += 1
            if i == len(lines):
                die(f'{MAKE}: the install rule under "{FIRST}" does not end')

        break
    else:
        die(f'{MAKE}: no install rule with "{FIRST}"')

    if not headers:
        die(f'{MAKE}: the install rule names no header')

    return headers


def structs(header):
    """The names a list entry can use for the structs in one header.

    The entry must be the tag, or the name of an anonymous typedef.  An
    alias of a tagged struct does not work: pahole -C with the alias
    prints only the "typedef struct tag alias;" line, with no layout, so
    layout-check.sh would report it "same" after any change.  The alias is
    also only known in the header that declares it, so the defining header
    still asks for the tag.
    """

    return definitions(Path(header).read_text())


def definitions(text):
    """The entry names for the struct and union definitions in text.

    Only top-level definitions count: a struct or union defined inside
    another one is part of its layout.  An anonymous struct that is not a
    typedef has no name pahole can find, thus it is skipped.
    """

    text = COMMENT.sub(' ', text)
    names = {}
    pos = 0

    while True:
        m = OPEN.search(text, pos)
        if not m:
            break

        depth = 0
        for end in range(m.end() - 1, len(text)):
            if text[end] == '{':
                depth += 1
            elif text[end] == '}':
                depth -= 1
                if depth == 0:
                    break
        else:
            die(f'unbalanced braces after "{m.group(0)}"')

        pos = end + 1
        typedef, tag = m.group(1), m.group(3)

        if tag:
            names.setdefault(tag, {tag})
        elif typedef:
            n = NAME.match(text, pos)
            if n:
                names.setdefault(n.group(1), {n.group(1)})

    return names


def self_test():
    """The definition shapes the scan accepts, pinned."""

    cases = [
        ('struct a_s { int x; };', {'a_s'}),
        ('union u_u { int x; char c; };', {'u_u'}),
        ('typedef struct t_s { int x; } t_t;', {'t_s'}),
        ('typedef struct { int x; } anon_t;', {'anon_t'}),
        ('typedef struct { struct { int a; } inner; } nest_t;', {'nest_t'}),
        ('typedef union { int a; } anon_u_t;', {'anon_u_t'}),
        ('struct o_s { union { int a; } u; struct i_s { int b; } i; };',
         {'o_s'}),
        ('typedef struct a_s a_t;\nstruct a_s *p;', set()),
        ('/* struct c_s { int x; }; */', set()),
    ]

    bad = 0
    for text, want in cases:
        got = set(definitions(text))
        if got != want:
            print(f'self-test: {text!r}: got {sorted(got)}, '
                  f'want {sorted(want)}')
            bad += 1

    if bad:
        sys.exit(1)

    print(f'ok: {len(cases)} self-test cases')


def listed():
    if not LIST.exists():
        die(f'no {LIST}')

    names = set()

    for n, line in enumerate(LIST.read_text().splitlines(), 1):
        line = line.split('#')[0].strip()
        if not line:
            continue

        parts = line.split()
        if len(parts) != 2 or parts[0] not in ('shm', 'local', 'const'):
            die(f'{LIST}:{n}: bad line: {line}')

        names.add(parts[1])

    return names


def main():
    if sys.argv[1:] == ['--self-test']:
        self_test()
        return

    have = listed()
    missing = []

    for header in installed_headers():
        for name, names in sorted(structs(header).items()):
            if not names & have:
                missing.append((header, name, sorted(names)))

    if missing:
        print('these structs are in the installed headers and not in the '
              f'{LIST}:')
        for header, name, names in missing:
            print(f'  {name:32} {header}'
                  + (f'  (also {", ".join(names)})' if names != [name] else ''))
        print()
        print('Add each as "local", or as "shm" when both processes read it '
              'through shared memory.')
        sys.exit(1)

    print(f'ok: every struct in the installed headers is in the {LIST}')


main()
