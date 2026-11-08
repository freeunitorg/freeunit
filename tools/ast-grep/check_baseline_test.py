#!/usr/bin/env python3
"""Tests for check_baseline.py.  Run them with python3 or with pytest."""
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_baseline import (  # noqa: E402
    emptied_rules, enclosing_function, key, match_path, new_violations,
    rule_ids, stale_entries, unloaded_rules
)


def entry(line, text='memcpy(a, b, n)', rule='memcpy-computed-size',
          function='f'):
    return {'ruleId': rule, 'file': 'src/x.c', 'function': function,
            'line': line, 'text': text}


def test_known_violation_passes():
    assert new_violations([entry(10)], [entry(12)]) == []


def test_unknown_violation_is_new():
    assert new_violations([entry(10, 'free(p)')], [entry(12)]) == [
        entry(10, 'free(p)')
    ]


def test_second_copy_of_known_violation_is_new():
    assert new_violations([entry(10), entry(20)], [entry(12)]) == [entry(20)]


def test_two_copies_need_two_entries():
    assert new_violations([entry(10), entry(20)],
                          [entry(12), entry(22)]) == []


def test_matched_entries_are_not_stale():
    assert stale_entries([entry(10)], [entry(12)]) == []


def test_fixed_violation_leaves_a_stale_entry():
    assert stale_entries([entry(10)], [entry(12), entry(22)]) == [entry(22)]


def test_update_keeps_a_rule_that_matches_nothing():
    # A rule that stops matching must not lose its entries silently.
    baseline = [entry(12), entry(14, rule='narrowing-to-proto')]

    assert emptied_rules([entry(10)], baseline) == ['narrowing-to-proto']
    assert emptied_rules(baseline, baseline) == []


def test_known_violation_in_other_function_is_new():
    # A fixed copy in f() does not cover a new copy in g().
    matches = [entry(30, function='g')]
    baseline = [entry(12)]

    assert new_violations(matches, baseline) == matches
    assert stale_entries(matches, baseline) == baseline


SOURCE = """\
static nxt_str_t  names[] = {
    nxt_string("a"),
};


static void
nxt_one(u_char *p)
{
    if (p) {
        *p = 0;
    }
}


static nxt_int_t nxt_two(u_char *p,
    size_t n)
{
    return n;
}
"""


def test_enclosing_function():
    assert enclosing_function(SOURCE, 2) == ''
    assert enclosing_function(SOURCE, 10) == 'nxt_one'
    assert enclosing_function(SOURCE, 14) == ''
    assert enclosing_function(SOURCE, 18) == 'nxt_two'


PROTOTYPES = """\
}


static void nxt_one(nxt_task_t *task,
    void *obj, void *data);
static nxt_int_t nxt_two(u_char *p);


static nxt_str_t  names[] = {
    nxt_string("a"),
};
"""


def test_enclosing_function_skips_prototypes():
    # A file-scope line below prototypes is outside of a function.
    assert enclosing_function(PROTOTYPES, 10) == ''
    assert enclosing_function(PROTOTYPES, 7) == ''


def test_key_ignores_whitespace():
    a = entry(10, 'resp->len =\n        req->len;')
    b = entry(10, 'resp->len =\n                req->len;')

    assert key(a) == key(b)
    assert new_violations([a], [b]) == []
    assert stale_entries([a], [b]) == []


def stderr(ran, skipped=0):
    lines = [f'sg: entity|rule|{r}: finalSeverity=Warning' for r in ran]
    lines.append('sg: summary|rule: '
                 f'effectiveRuleCount={len(ran)},'
                 f'skippedRuleCount={skipped}')

    return '\n'.join(lines) + '\n'


def test_unloaded_rules_passes_when_each_rule_ran():
    assert unloaded_rules(stderr(['a', 'b']), {'a', 'b'}) == ''


def test_unloaded_rules_catches_a_rule_turned_off():
    # "severity: off" skips the rule.  A second rule in another file
    # keeps the count the same, so only the ids show the loss.
    out = stderr(['a', 'extra'], skipped=1)

    assert 'effectiveRuleCount=2' in out
    assert unloaded_rules(out, {'a', 'b', 'extra'}) == \
        'ast-grep did not run b'


def test_unloaded_rules_catches_a_skipped_rule():
    # Each id of rules/ ran, and a rule was still skipped.
    assert unloaded_rules(stderr(['a', 'b'], skipped=1), {'a', 'b'}) == \
        'ast-grep skipped 1 rule(s)'


def test_unloaded_rules_needs_the_summary_line():
    assert unloaded_rules('', set()) == \
        'no skippedRuleCount in the ast-grep output'


def test_rule_ids_reads_a_subdirectory():
    # ast-grep reads a ruleDirs entry down the whole tree.  A rule in
    # rules/sub/ counts, and one file can hold more than one rule.
    with tempfile.TemporaryDirectory() as tmp:
        rules = Path(tmp)
        (rules / 'sub').mkdir()
        (rules / 'a.yml').write_text(
            'id: a\nlanguage: c\nrule:\n  pattern: f()\n'
            '---\nid: a2\nlanguage: c\nrule:\n  pattern: g()\n'
        )
        (rules / 'sub' / 'b.yml').write_text(
            'id: b\nlanguage: c\nrule:\n  pattern: h()\n'
            'utils:\n  u:\n    pattern: i()\n'
        )

        assert rule_ids(rules) == {'a', 'a2', 'b'}


def test_match_path():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        cwd = os.getcwd()

        try:
            # ast-grep ran in root.  A relative path is relative to root
            # also when this script runs in another directory.
            os.chdir('/')

            assert match_path('src/x.c', root) == 'src/x.c'
            assert match_path(str(root / 'src/x.c'), root) == 'src/x.c'
        finally:
            os.chdir(cwd)


if __name__ == '__main__':
    for name, fn in sorted(globals().items()):
        if name.startswith('test_'):
            fn()
            print(f'{name}: ok')
