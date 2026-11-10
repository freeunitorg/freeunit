import json
import os
import re
import shutil
import signal
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import conftest
from unit import port as port_map
from unit.option import Options

# The session cases run the real conftest.py in a child pytest session:
# it starts its own unitd, and a fixture of the child stages the failure
# in teardown, which a test body of this session cannot do.  The child
# adds one fake row to the "ps" listing of teardown.  The row names the
# main pid of the unitd that ran the staged test, so a fresh unitd does
# not see it.
TEST_DIR = os.path.dirname(os.path.abspath(__file__))
PROCESS_TIMEOUT = 2
LEFTOVER = 'unit: "stuck" prototype'

CONFTEST = f'''
import importlib.util
import subprocess
import sys

sys.path.insert(0, {TEST_DIR!r})

# The child's own conftest module takes the name "conftest", so load the
# suite's one under another name.
spec = importlib.util.spec_from_file_location(
    'unit_conftest', {TEST_DIR!r} + '/conftest.py'
)
unit_conftest = importlib.util.module_from_spec(spec)
sys.modules['unit_conftest'] = unit_conftest
spec.loader.exec_module(unit_conftest)

for name, value in vars(unit_conftest).items():
    if not name.startswith('__'):
        globals().setdefault(name, value)

PS = ['ps', '-ax', '-o', 'pid', '-o', 'ppid', '-o', 'command']

# Main pids that get the fake leftover row.
leftover_pids = []

# Group ids that look alive after the real reap, as a group that
# survived SIGKILL does.
stuck = set()
stuck_mode = []


class FakeSubprocess:
    def __getattr__(self, name):
        return getattr(subprocess, name)

    @staticmethod
    def check_output(args, *a, **kw):
        out = subprocess.check_output(args, *a, **kw)
        if list(args) == PS and leftover_pids:
            out += f'99999999 {{leftover_pids[0]}} {LEFTOVER}\\n'.encode()
        return out


unit_conftest.subprocess = FakeSubprocess()

real_group_alive = unit_conftest._group_alive
real_reap_group = unit_conftest._reap_group


def group_alive(p, pgid):
    if pgid in stuck:
        return True
    return real_group_alive(p, pgid)


def reap_group(p, pgid, timeout):
    if pgid in stuck:
        return
    real_reap_group(p, pgid, timeout)
    if stuck_mode:
        stuck.add(pgid)


unit_conftest._group_alive = group_alive
unit_conftest._reap_group = reap_group
'''

TESTS = '''
import json
import os

import pytest

import unit_conftest

conftest = __import__('conftest')
unit_instance = unit_conftest.unit_instance


def record(**kw):
    with open(os.environ['RECORD'], 'a', encoding='utf-8') as f:
        f.write(json.dumps(kw) + '\\n')


@pytest.fixture
def leftover():
    yield
    conftest.leftover_pids.append(unit_instance['pid'])


@pytest.fixture
def survives(leftover):
    yield
    conftest.stuck_mode.append(True)


@pytest.fixture
def mounted(leftover):
    yield
    unit_conftest.is_findmnt = True
    unit_conftest.waitforunmount = lambda path, timeout: False


def test_first(leftover):
    record(
        pid=unit_instance['pid'],
        pgid=unit_instance['pgid'],
        temp_dir=unit_instance['temp_dir'],
    )


def test_second():
    old = conftest.leftover_pids[0]
    old_pgid = int(old)
    record(
        pid=unit_instance['pid'],
        old_alive=unit_conftest._group_alive(None, old_pgid),
    )


def test_survives(survives):
    record(pgid=unit_instance['pgid'], temp_dir=unit_instance['temp_dir'])


def test_mounted(mounted):
    record(pgid=unit_instance['pgid'], temp_dir=unit_instance['temp_dir'])


def test_base():
    record(base=int(os.environ['_UNIT_TEST_PORT_BASE']))
'''


def run_child(tmp_path, *names):
    (tmp_path / 'pytest.ini').write_text('[pytest]\n')
    (tmp_path / 'conftest.py').write_text(CONFTEST)
    (tmp_path / 'test_child.py').write_text(textwrap.dedent(TESTS))
    record = tmp_path / 'record.jsonl'

    # The child resolves its own port base from --port or UNIT_TEST_PORT
    # and else falls back to 8080.  Give it the base of this session, or
    # it binds 8080 where that port is taken.
    base = str(port_map.base())

    env = dict(os.environ)
    env['RECORD'] = str(record)
    env['UNIT_TEST_PROCESS_TIMEOUT'] = str(PROCESS_TIMEOUT)
    env['UNIT_TEST_PORT'] = base

    child = subprocess.Popen(
        [
            sys.executable,
            '-m',
            'pytest',
            '-p',
            'no:cacheprovider',
            '-p',
            'no:timeout',
            '-q',
            '--durations=0',
            '--durations-min=0',
            '--port',
            base,
            *[f'test_child.py::test_{name}' for name in names],
        ],
        cwd=tmp_path,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        stdout, stderr = child.communicate(timeout=240)

    except subprocess.TimeoutExpired:
        # SIGTERM first: the handler in conftest.py reaps the unitd of
        # the child.  SIGKILL would leave it running.
        child.terminate()
        try:
            stdout, stderr = child.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            child.kill()
            stdout, stderr = child.communicate()

        raise AssertionError(f'the child session hung:\n{stdout}{stderr}')

    text = stdout + stderr

    assert stdout, f'the child session printed nothing:\n{text}'
    summary = [l for l in stdout.splitlines() if ' in ' in l][-1]
    counts = {
        kind.rstrip('s'): int(n)
        for n, kind in re.findall(r'(\d+) (passed|failed|errors?)', summary)
    }

    lines = record.read_text().splitlines() if record.exists() else []

    return counts, [json.loads(l) for l in lines], text


def teardown_seconds(text, name):
    m = re.search(fr'([\d.]+)s teardown +test_child.py::test_{name}', text)
    assert m, text
    return float(m.group(1))


def test_recovery_one_error_then_new_unitd(tmp_path):
    counts, rec, text = run_child(tmp_path, 'first', 'second')

    assert counts == {'passed': 2, 'error': 1}, text
    assert len(re.findall(r'^E +AssertionError: ', text, re.M)) == 1, text
    assert (
        f'main, router, and controller expected after {PROCESS_TIMEOUT} s'
        in text
    ), text
    assert LEFTOVER in text, text

    # One wait of PROCESS_TIMEOUT, then the reap of the group.
    assert teardown_seconds(text, 'first') < PROCESS_TIMEOUT + 15, text

    first, second = rec
    assert second['pid'] != first['pid'], text
    assert second['old_alive'] is False, text
    assert not Path(first['temp_dir']).exists(), text


def test_recovery_survived_group_keeps_dir(tmp_path):
    counts, rec, text = run_child(tmp_path, 'survives')
    kept = Path(rec[0]['temp_dir'])
    pgid_file = kept / 'unitd.pgid'

    try:
        assert counts == {'passed': 1, 'error': 1}, text
        assert 'survived SIGKILL; kept' in text, text
        assert kept.is_dir(), f'{kept} is gone after the session:\n{text}'
        assert pgid_file.read_text() == f'{rec[0]["pgid"]}\n', text

    finally:
        shutil.rmtree(kept, ignore_errors=True)


def test_recovery_still_mounted_keeps_dir(tmp_path):
    # Simulated: findmnt is forced on and waitforunmount() reports a busy
    # mount.  A real mount needs root.
    counts, rec, text = run_child(tmp_path, 'mounted')
    kept = Path(rec[0]['temp_dir'])

    try:
        assert counts == {'passed': 1, 'error': 1}, text
        assert ': still mounted' in text, text
        assert kept.is_dir(), f'{kept} is gone after the session:\n{text}'

    finally:
        shutil.rmtree(kept, ignore_errors=True)


def fake_unit(monkeypatch, tmp_path, restart):
    # A real process group, as unit_run() starts unitd.
    p = subprocess.Popen(['sleep', '60'], start_new_session=True)
    temp_dir = tmp_path / 'unit'
    temp_dir.mkdir()

    instance = {'process': p, 'pgid': p.pid, 'temp_dir': str(temp_dir)}

    monkeypatch.setattr(conftest, 'unit_instance', instance)
    monkeypatch.setattr(conftest, '_deadline_alarm', [])
    monkeypatch.setattr(conftest, '_retained_temp_dirs', set(), raising=False)
    # Options keeps its values in a class dict; see test_conftest_fd_kinds.py.
    monkeypatch.setitem(Options._options, 'restart', restart)
    monkeypatch.setitem(Options._options, 'save_log', False)

    return p, instance, temp_dir


def test_recovery_restart_returns_early(monkeypatch, tmp_path):
    p, instance, temp_dir = fake_unit(monkeypatch, tmp_path, restart=True)

    try:
        conftest._recover_unit()

        assert p.poll() is None, 'the group is left to unit_stop()'
        assert instance.get('process') is p, 'unit_instance is kept'
        assert temp_dir.is_dir()

    finally:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()


def test_recovery_kills_group(monkeypatch, tmp_path):
    p, instance, temp_dir = fake_unit(monkeypatch, tmp_path, restart=False)

    try:
        start = time.monotonic()
        conftest._recover_unit()

        assert p.poll() == -signal.SIGTERM
        assert not conftest._group_alive(p, p.pid)
        assert instance == {}, 'unit_instance is cleared'
        assert not temp_dir.exists()
        assert time.monotonic() - start < 10

    finally:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()


def test_recovery_child_uses_parent_port_base(tmp_path):
    # Run off the default base (--port), a child that fell back to 8080
    # failed where 8080 is taken.
    counts, rec, text = run_child(tmp_path, 'base')

    assert counts == {'passed': 1}, text
    assert rec[0]['base'] == port_map.base(), text
