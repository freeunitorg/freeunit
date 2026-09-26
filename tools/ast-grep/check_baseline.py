#!/usr/bin/env python3
"""Fail on ast-grep violations in src/ not in baseline.json, keyed by rule,
file and text rather than line.  A key is counted: the baseline holds one
entry per violation, so a second copy of a known one is new.  A baseline
entry no violation matches any more is stale and fails too, so a fixed
violation's allowance cannot be reused later.  --update rewrites the
baseline.

Exit codes: 0 baseline and scan agree, 1 new or stale violations, 2 the
scan failed.
"""
import json
from collections import Counter
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BASELINE = HERE / 'baseline.json'


def scan():
    cmd = ['ast-grep', 'scan', '-c', str(HERE / 'sgconfig.yml'), '--json']
    res = subprocess.run(
        cmd + ['src'], cwd=ROOT, capture_output=True, text=True
    )

    try:
        if res.returncode not in (0, 1):
            raise ValueError(f'exit {res.returncode}')
        matches = json.loads(res.stdout)
    except ValueError as e:
        sys.stderr.write(res.stdout + res.stderr)
        print(f'ast-grep scan failed: {e}')
        sys.exit(2)

    entries = []

    for m in matches:
        path = Path(m['file'])
        try:
            path = path.resolve().relative_to(ROOT)
        except ValueError:
            pass

        entries.append({
            'ruleId': m['ruleId'],
            'file': str(path),
            'line': m['range']['start']['line'],
            'text': m['text'].strip(),
        })

    return sorted(entries, key=lambda e: (e['ruleId'], e['file'], e['line']))


def key(e):
    return (e['ruleId'], e['file'], e['text'])


def new_violations(matches, baseline):
    """The matches the baseline does not cover, one baseline entry each."""
    known = Counter(key(e) for e in baseline)
    new = []

    for m in matches:
        if known[key(m)] > 0:
            known[key(m)] -= 1
        else:
            new.append(m)

    return new


def stale_entries(matches, baseline):
    """The baseline entries no match covers: a fixed violation's allowance."""
    seen = Counter(key(m) for m in matches)
    stale = []

    for e in baseline:
        if seen[key(e)] > 0:
            seen[key(e)] -= 1
        else:
            stale.append(e)

    return stale


def main():
    matches = scan()

    if sys.argv[1:] == ['--update']:
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
            print(f"  {m['file']}:{m['line']}: [{m['ruleId']}] {m['text']}")

    if stale:
        print(f'ast-grep: {len(stale)} STALE baseline.json entry(ies) with '
              'no violation left (fixed? then drop them):')
        for m in stale:
            print(f"  {m['file']}: [{m['ruleId']}] {m['text']}")

    print('\nIf reviewed and intended, run '
          "'python3 tools/ast-grep/check_baseline.py --update' and commit "
          'baseline.json with the change.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
