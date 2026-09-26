#!/usr/bin/env python3
"""Tests for check_baseline.py.  Run them with python3 or with pytest."""
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_baseline import (  # noqa: E402
    enclosing_function, match_path, new_violations, stale_entries
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
