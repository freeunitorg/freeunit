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

Each rule in rules/, and in any subdirectory of it, must be run by the
scan, or the check exits with 2.  A rule with "severity: off" counts as
not run.  A rule that runs but matches nothing is caught by its own test
("ast-grep test").  --update does not drop all the entries of a rule: if a
rule had entries and matches nothing now, it stops.  Check the rule, then
remove its entries by hand.

The --update option writes the baseline again.  The baseline does not
keep the line numbers: they are not part of the key, and they change
with each edit above a match.

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


# "id:" in column 0 starts a rule.  One file can hold more than one rule,
# as YAML documents separated by "---".  A key inside a rule, such as one
# under "utils:", is indented and does not match.
_RULE_ID = re.compile(r'^id:[ \t]*(\S+)', re.M)

# "--inspect entity" writes one line for each rule that the scan ran.
_ENTITY_RULE = re.compile(r'^sg: entity\|rule\|(.+?): ', re.M)
_SKIPPED_RULES = re.compile(r'skippedRuleCount=(\d+)')


def rule_ids(rules_dir):
    """Return the set of rule ids in the files under rules_dir.

    ast-grep reads a ruleDirs entry down the whole tree, so a rule in a
    subdirectory of rules/ counts too.  rglob finds it; glob would not.
    """
    ids = set()

    for path in Path(rules_dir).rglob('*.yml'):
        ids.update(_RULE_ID.findall(path.read_text()))

    return ids


def unloaded_rules(stderr, expected):
    """Return the reason the scan did not run every rule, or ''.

    A count of the rules is not enough.  A rule with "severity: off" is
    skipped, and a second rule in another file keeps the count the same.
    So the ids are compared, and a skipped rule fails the check as well.
    """
    loaded = set(_ENTITY_RULE.findall(stderr))
    missing = sorted(expected - loaded)

    if missing:
        return 'ast-grep did not run ' + ', '.join(missing)

    skipped = _SKIPPED_RULES.search(stderr)

    if not skipped:
        return 'no skippedRuleCount in the ast-grep output'

    if skipped.group(1) != '0':
        return f'ast-grep skipped {skipped.group(1)} rule(s)'

    return ''


def scan():
    # The C tests in src/test/ are not scanned.  They build bad input on
    # purpose, and each edit of a matched test line would change the
    # baseline.
    # --inspect entity names each rule that the scan ran, so the ids can
    # be compared.  Its summary line holds the skippedRuleCount.
    cmd = ['ast-grep', 'scan', '-c', str(HERE / 'sgconfig.yml'), '--json',
           '--inspect', 'entity', '--globs', '!src/test/**']
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

        # A wrong ruleDirs or a rule file that does not load is not an
        # error for ast-grep.  Its matches would then look fixed.
        bad = unloaded_rules(res.stderr, rule_ids(HERE / 'rules'))

        if bad:
            raise ValueError(bad)
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
# name on its own line or after the type.  A "}" in column 0 closes the definition.  A prototype also
# starts in column 0, but it ends with ";".
_DEFINITION = re.compile(r'(?:[A-Za-z_][A-Za-z0-9_ *]*[ *])?'
                         r'([A-Za-z_][A-Za-z0-9_]*)\(')


def is_prototype(lines, start):
    """Return True if the declaration at lines[start] ends with ";".

    A prototype can continue on the next lines.  The search stops at the
    first line that ends with ";" (a prototype) or "{" (a definition).
    """
    for text in lines[start:]:
        text = text.rstrip()

        if text.endswith(';'):
            return True

        if text.endswith('{'):
            return False

    return False


def enclosing_function(source, line):
    """Return the name of the function that contains the line.

    Return '' for a line outside of a function.  Lines count from 1.
    """
    lines = source.splitlines()

    for i in range(line - 2, -1, -1):
        text = lines[i]

        if text.startswith('}'):
            return ''

        m = _DEFINITION.match(text)
        if m and not is_prototype(lines, i):
            return m.group(1)

    return ''


def key(e):
    # The text of a match can have more than one line.  Whitespace is
    # normalized, so a change of the indent does not change the key.
    return (e['ruleId'], e['file'], e.get('function', ''),
            ' '.join(e['text'].split()))


def unmatched(items, against):
    """Return the items that no entry of "against" covers.

    One entry covers one item.
    """
    left = Counter(key(e) for e in against)
    out = []

    for item in items:
        if left[key(item)] > 0:
            left[key(item)] -= 1
        else:
            out.append(item)

    return out


def new_violations(matches, baseline):
    """Return the matches that the baseline does not cover."""
    return unmatched(matches, baseline)


def stale_entries(matches, baseline):
    """Return the baseline entries that no match covers.

    Such an entry is the allowance for a violation that is fixed.
    """
    return unmatched(baseline, matches)


def emptied_rules(matches, baseline):
    """Return the rules that have baseline entries and no match."""
    return sorted({e['ruleId'] for e in baseline}
                  - {m['ruleId'] for m in matches})


def main():
    args = sys.argv[1:]

    if args not in ([], ['--update']):
        print('usage: check_baseline.py [--update]')
        return 2

    matches = scan()

    if args == ['--update']:
        emptied = emptied_rules(matches, json.loads(BASELINE.read_text()))

        if emptied:
            print('ast-grep: no match left for ' + ', '.join(emptied) + '. '
                  'If the rule still works and each match is fixed, remove '
                  'its entries from baseline.json by hand.')
            return 1

        entries = [{k: v for k, v in m.items() if k != 'line'}
                   for m in matches]
        BASELINE.write_text(json.dumps(entries, indent=2) + '\n')
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
