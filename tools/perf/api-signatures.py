#!/usr/bin/env python3

# Copyright (C) FreeUnit contributors.
#
# Checks that the declarations of the public functions did not change between
# two commits.
#
# A module or an embedder compiles the declarations of the installed headers
# into its own object, so a change to a return type or to a parameter type
# breaks it.  The tree builds and links without a word.
# tools/perf/layout-check.sh covers a public function that disappears; this
# covers the one that stays and changes shape.
#
#   tools/perf/api-signatures.py BASE [HEAD]
#
# The comparison is textual.  Comments are removed, whitespace is collapsed
# and parameter names are dropped, so a reformat or a parameter rename does
# not report.  Types, in return position and in the parameter list, do.
# A macro that stands for a type is compared as written.
#
# Run it from the repository root.
#
# Exit status: 0 when every declaration is unchanged, 1 when one changed or
# left the headers, 2 for an error of the check itself.

import re
import subprocess
import sys

MAKE = 'auto/make'
FIRST = 'install -p -m u=rw,go=r src/nxt_unit.h'
COMMENT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
FUNC = re.compile(r'\b(nxt_unit_[a-z0-9_]+)\s*\(')
# A trailing word of a parameter is a name unless it is one of these.  The
# list keeps "unsigned int" and friends whole when the parameter is unnamed.
TYPES = {
    'void', 'char', 'short', 'int', 'long', 'float', 'double', 'signed',
    'unsigned', 'const', 'volatile', 'restrict', 'struct', 'union', 'enum',
    '_Bool', 'size_t', 'ssize_t', 'ptrdiff_t', 'intptr_t', 'uintptr_t',
}


def die(message):
    print(f'api-signatures: {message}', file=sys.stderr)
    sys.exit(2)


def show(commit, path):
    out = subprocess.run(['git', 'show', f'{commit}:{path}'],
                         capture_output=True, text=True)

    return out.stdout if out.returncode == 0 else None


def headers(commit):
    """The headers the install rule of COMMIT copies."""

    make = show(commit, MAKE)
    if make is None:
        die(f'{commit} has no {MAKE}')

    lines = make.splitlines()
    names = []

    for i, line in enumerate(lines):
        if FIRST not in line:
            continue

        while True:
            names += re.findall(r'src/[A-Za-z0-9_]+\.h', lines[i])

            if not lines[i].rstrip().endswith('\\'):
                break

            i += 1
            if i == len(lines):
                die(f'{MAKE} of {commit}: the install rule does not end')

        break
    else:
        die(f'{MAKE} of {commit}: no install rule with "{FIRST}"')

    return names


def parameters(text):
    """The parameter list of "text", split on the top level commas."""

    depth = 0
    current = ''
    out = []

    for ch in text:
        if ch == '(' or ch == '[':
            depth += 1
        elif ch == ')' or ch == ']':
            depth -= 1

        if ch == ',' and depth == 0:
            out.append(current)
            current = ''
        else:
            current += ch

    if current.strip():
        out.append(current)

    return out


def drop_name(parameter):
    """A parameter without its name: "nxt_unit_ctx_t *ctx" -> "nxt_unit_ctx_t *"."""

    parameter = parameter.strip()
    m = re.match(r'^(.*?)([A-Za-z_][A-Za-z0-9_]*)$', parameter)

    if not m or not m.group(1).strip():
        return parameter

    if m.group(2) in TYPES:
        return parameter

    return m.group(1).strip()


def declarations(commit):
    """name -> the normalized declaration, for one commit."""

    found = {}

    for header in headers(commit):
        text = show(commit, header)
        if text is None:
            continue

        text = COMMENT.sub(' ', text)
        text = re.sub(r'\s+', ' ', text)

        for piece in text.split(';'):
            m = FUNC.search(piece)
            if not m:
                continue

            name = m.group(1)
            start = m.end()
            depth = 1
            i = start

            while i < len(piece) and depth:
                if piece[i] == '(':
                    depth += 1
                elif piece[i] == ')':
                    depth -= 1
                i += 1

            if depth:
                continue

            ret = piece[:m.start()].strip()
            # Keep the pointer stars with the return type, not the name.
            ret = re.sub(r'\s*\*\s*$', ' *', ret) if ret.endswith('*') else ret
            args = ', '.join(drop_name(a) for a in parameters(piece[start:i - 1]))
            found[name] = f'{ret} {name}({args})'

    return found


def main():
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        die('usage: api-signatures.py BASE [HEAD]')

    base = subprocess.run(['git', 'rev-parse', '--verify', sys.argv[1]],
                          capture_output=True, text=True)
    if base.returncode != 0:
        die(f'no such revision: {sys.argv[1]}')

    head = subprocess.run(['git', 'rev-parse', '--verify',
                           sys.argv[2] if len(sys.argv) == 3 else 'HEAD'],
                          capture_output=True, text=True)
    if head.returncode != 0:
        die(f'no such revision: {sys.argv[2] if len(sys.argv) == 3 else "HEAD"}')

    base = base.stdout.strip()
    head = head.stdout.strip()

    old = declarations(base)
    new = declarations(head)
    changed = []

    for name, was in sorted(old.items()):
        now = new.get(name)

        if now is None:
            changed.append((name, was, '(not declared any more)'))
        elif now != was:
            changed.append((name, was, now))

    print(f'base: {base[:9]}  head: {head[:9]}  '
          f'declarations: {len(old)} base, {len(new)} head')

    if changed:
        for name, was, now in changed:
            print(f'::error::{name}: the declaration changed')
            print(f'  base: {was}')
            print(f'  head: {now}')

        print(f'::error::api-signatures: {len(changed)} declaration(s) changed '
              '(see tools/perf/api-signatures.py)')
        sys.exit(1)

    print('ok: every public declaration is unchanged')


main()
