import os
import re
import subprocess
import sys
import textwrap

# Runs the real deadline fixture and the rep_call hook from conftest.py in
# a child pytest session, so a hang can be staged in a fixture, which a
# test body of this session cannot do.  The child needs no unitd: only
# the two are imported, not the autouse run() fixture.
TEST_DIR = os.path.dirname(os.path.abspath(__file__))
CAUGHT = 'The test caught the failure.'

CONFTEST = f'''
import importlib.util
import sys

sys.path.insert(0, {TEST_DIR!r})

# The child's own conftest module takes the name "conftest", so load the
# suite's one under another name.
spec = importlib.util.spec_from_file_location(
    'unit_conftest', {TEST_DIR!r} + '/conftest.py'
)
unit_conftest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(unit_conftest)

deadline = unit_conftest.deadline
pytest_runtest_makereport = unit_conftest.pytest_runtest_makereport

from unit.option import option

option.test_timeout = 1
'''

TESTS = '''
import time

import pytest


@pytest.fixture
def hang_setup(deadline):
    time.sleep(10)
    yield


@pytest.fixture
def hang_teardown(deadline):
    yield
    time.sleep(10)


def test_setup(hang_setup):
    pass


def test_teardown(hang_teardown):
    pass


def test_caught(deadline):
    try:
        time.sleep(10)
    except BaseException:
        pass


def test_hang(deadline):
    time.sleep(10)
'''


def run_child(tmp_path, name):
    (tmp_path / 'pytest.ini').write_text('[pytest]\n')
    (tmp_path / 'conftest.py').write_text(CONFTEST)
    (tmp_path / 'test_child.py').write_text(textwrap.dedent(TESTS))

    out = subprocess.run(
        [
            sys.executable,
            '-m',
            'pytest',
            '-p',
            'no:cacheprovider',
            '-p',
            'no:timeout',
            '-q',
            f'test_child.py::test_{name}',
        ],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=60,
    ).stdout

    assert out, 'the child session printed nothing'
    summary = re.findall(r'(\d+) (passed|failed|errors?)', out.splitlines()[-1])
    counts = {kind.rstrip('s'): int(n) for n, kind in summary}

    return counts, out


def test_deadline_setup_hang_one_error(tmp_path):
    counts, out = run_child(tmp_path, 'setup')

    assert counts == {'error': 1}, out
    assert CAUGHT not in out, out


def test_deadline_teardown_hang_one_error(tmp_path):
    counts, out = run_child(tmp_path, 'teardown')

    assert counts == {'passed': 1, 'error': 1}, out
    assert 'ExceptionGroup' not in out, out
    assert CAUGHT not in out, out


def test_deadline_caught_in_body_fails(tmp_path):
    counts, out = run_child(tmp_path, 'caught')

    assert counts == {'passed': 1, 'error': 1}, out
    assert CAUGHT in out, out


def test_deadline_body_hang_one_failure(tmp_path):
    counts, out = run_child(tmp_path, 'hang')

    assert counts == {'failed': 1}, out
    assert CAUGHT not in out, out
