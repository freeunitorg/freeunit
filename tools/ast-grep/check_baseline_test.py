#!/usr/bin/env python3
"""Tests for check_baseline.new_violations(): run with python3 or pytest."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_baseline import new_violations, stale_entries  # noqa: E402


def entry(line, text='memcpy(a, b, n)', rule='memcpy-computed-size'):
    return {'ruleId': rule, 'file': 'src/x.c', 'line': line, 'text': text}


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


if __name__ == '__main__':
    for name, fn in sorted(globals().items()):
        if name.startswith('test_'):
            fn()
            print(f'{name}: ok')
