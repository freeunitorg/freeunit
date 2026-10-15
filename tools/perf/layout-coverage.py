#!/usr/bin/env python3

# Copyright (C) FreeUnit contributors.
#
# Checks that every struct a language module can see has an entry in
# tools/perf/layout-structs.txt, so that tools/perf/layout-check.sh watches
# it.
#
# A module is built against the headers that "make install" copies, and it
# lays those structs out itself.  A change under a module that was built
# against the old header is silent: the module reads the wrong offsets.
# The list is what makes the layout check look, and this script is what
# keeps the list complete.
#
#   tools/perf/layout-coverage.py
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
NAMED = re.compile(r'^struct ([A-Za-z_][A-Za-z0-9_]*) \{', re.M)
ANON = re.compile(r'^typedef struct \{[^}]*\} ([A-Za-z_][A-Za-z0-9_]*);', re.M | re.S)
ALIAS = re.compile(r'^typedef struct ([A-Za-z_][A-Za-z0-9_]*) ([A-Za-z_][A-Za-z0-9_]*);', re.M)


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

    A struct has a tag, a typedef name, or both.  The list may use any of
    them, because pahole -C takes any of them.
    """

    text = Path(header).read_text()
    names = {}

    for tag in NAMED.findall(text):
        names.setdefault(tag, set()).add(tag)

    for alias, tag in ALIAS.findall(text):
        names.setdefault(tag, set()).add(alias)

    for name in ANON.findall(text):
        names.setdefault(name, set()).add(name)

    return names


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
