#!/usr/bin/env python3
"""Fail on ast-grep violations in src/ that are not in baseline.json.

The scan skips src/test/.  Line numbers count from 1.

A violation is identified by its rule, its file, the function that
contains it and its text.  The line number is not part of the key, so an
edit above a match does not change the key.  Each key is counted.  The
baseline holds one entry for each violation.  Thus a second copy of a
known violation is new.  A baseline entry that no violation matches any
more is stale.  A stale entry fails the check too.

A limit: when one copy of a violation is fixed and an identical copy is
added in the same function in the same change, the counts do not change,
and the check passes.  A copy in a different function is new.

The --update option writes the baseline again.

Exit codes: 0 when the baseline and the scan agree, 1 for new or stale
violations, 2 when the scan failed or for a bad argument.
"""
import json
from collections import Counter
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BASELINE = HERE / 'baseline.json'


def scan():
    # The C tests in src/test/ are not scanned.  They build bad input on
    # purpose, and each edit of a matched test line would change the
    # baseline.
    cmd = ['ast-grep', 'scan', '-c', str(HERE / 'sgconfig.yml'), '--json',
           '--globs', '!src/test/**']
    try:
        res = subprocess.run(
            cmd + ['src'], cwd=ROOT, capture_output=True, text=True
        )
    except OSError as e:
        print(f'ast-grep scan failed: {e}')
        sys.exit(2)

    try:
        if res.returncode not in (0, 1):
            raise ValueError(f'exit {res.returncode}')
        matches = json.loads(res.stdout)
    except ValueError as e:
        sys.stderr.write(res.stdout + res.stderr)
        print(f'ast-grep scan failed: {e}')
        sys.exit(2)

    entries = []
    sources = {}

    for m in matches:
        path = match_path(m['file'], ROOT)

        if path not in sources:
            sources[path] = (ROOT / path).read_text(errors='replace')

        # ast-grep counts lines from 0.  Editors count from 1.
        line = m['range']['start']['line'] + 1

        entries.append({
            'ruleId': m['ruleId'],
            'file': path,
            'function': enclosing_function(sources[path], line),
            'line': line,
            'text': m['text'].strip(),
        })

    return sorted(entries, key=lambda e: (e['ruleId'], e['file'], e['line']))


def match_path(file, root):
    """Return the path of a match relative to root, as a string.

    ast-grep runs in root, so a relative path is relative to root, not to
    the directory this script was started from.
    """
    path = Path(file)

    if not path.is_absolute():
        path = root / path

    try:
        return str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        return str(path)


# A function definition starts in column 0 in the Unit style, with the
# name on its own line or after the type, as G6 in tools/gates/run_gates.sh
# accepts it.  A "}" in column 0 closes the definition.
_DEFINITION = re.compile(r'(?:[A-Za-z_][A-Za-z0-9_ *]*[ *])?'
                         r'([A-Za-z_][A-Za-z0-9_]*)\(')


def enclosing_function(source, line):
    """Return the name of the function that contains the line.

    Return '' for a line outside of a function.  Lines count from 1.
    """
    lines = source.splitlines()

    for text in reversed(lines[:line - 1]):
        if text.startswith('}'):
            return ''

        m = _DEFINITION.match(text)
        if m:
            return m.group(1)

    return ''


def key(e):
    return (e['ruleId'], e['file'], e.get('function', ''), e['text'])


def new_violations(matches, baseline):
    """Return the matches that the baseline does not cover.

    One baseline entry covers one match.
    """
    known = Counter(key(e) for e in baseline)
    new = []

    for m in matches:
        if known[key(m)] > 0:
            known[key(m)] -= 1
        else:
            new.append(m)

    return new


def stale_entries(matches, baseline):
    """Return the baseline entries that no match covers.

    Such an entry is the allowance for a violation that is fixed.
    """
    seen = Counter(key(m) for m in matches)
    stale = []

    for e in baseline:
        if seen[key(e)] > 0:
            seen[key(e)] -= 1
        else:
            stale.append(e)

    return stale


def main():
    args = sys.argv[1:]

    if args not in ([], ['--update']):
        print('usage: check_baseline.py [--update]')
        return 2

    matches = scan()

    if args == ['--update']:
        BASELINE.write_text(json.dumps(matches, indent=2) + '\n')
        print(f'wrote {len(matches)} entries to {BASELINE}')
        return 0

    baseline = json.loads(BASELINE.read_text())
    new = new_violations(matches, baseline)
    stale = stale_entries(matches, baseline)

    if not new and not stale:
        print(f'ast-grep: {len(matches)} violation(s), all in baseline. OK.')
        return 0

    if new:
        print(f'ast-grep: {len(new)} NEW violation(s) not in baseline.json:')
        for m in new:
            print(f"  {m['file']}:{m['line']}: {m['function']}(): "
                  f"[{m['ruleId']}] {m['text']}")

    if stale:
        print(f'ast-grep: {len(stale)} STALE baseline.json entry(ies) with '
              'no violation left (fixed? then drop them):')
        for m in stale:
            print(f"  {m['file']}: {m.get('function', '')}(): "
                  f"[{m['ruleId']}] {m['text']}")

    print('\nIf reviewed and intended, run '
          "'python3 tools/ast-grep/check_baseline.py --update' and commit "
          'baseline.json with the change.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
