"""The restart back-off for workers that exit before they answer (#596).

A worker that exits before the router saw any answer from it is a failure.
While failures are counted, a new process starts only after a delay: 100 ms
after the first round, doubled per round, at most 5 s.  Any answer from the
application ends the back-off.

The test kills the one process of the application with SIGKILL, before it
answered anything, and times how long the replacement takes to appear.
Without the back-off the replacement appears within a few milliseconds every
time.  Only lower bounds are asserted for the delays, so a slow box cannot
fail them; the upper bounds are generous.
"""

import os
import signal
import subprocess
import time

import pytest

from unit.applications.lang.python import ApplicationPython

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

APP = 'empty'

ALERT = r'a process exited before it answered anything'
NOTICE = r'new processes start without a delay again'

# The delay of each round in seconds, and the slack allowed below it for the
# 10 ms poll and the clock.
STEPS = [0.1, 0.2, 0.4]
SLACK = 0.02

# Generous: a loaded CI box may take long to fork and boot a worker.
LATE = 5


_unit = {}


@pytest.fixture(autouse=True)
def setup_method_fixture(unit_pid):
    _unit['pid'] = unit_pid


def _app_pids():
    """Answers the pids of the workers of this test's unitd.

    The title alone is not enough: another unitd on the host can run an
    application of the same name.  A worker is a child of a prototype of
    this unitd.
    """

    out = subprocess.check_output(
        ['ps', '-ax', '-o', 'pid=', '-o', 'ppid=', '-o', 'command=']
    ).decode()

    procs = []

    for line in out.splitlines():
        fields = line.split(None, 2)

        if len(fields) == 3:
            procs.append((int(fields[0]), int(fields[1]), fields[2]))

    protos = {
        pid
        for pid, ppid, cmd in procs
        if ppid == _unit['pid']
        and f'unit: "{APP}" prototype' in cmd
    }

    return {
        pid
        for pid, ppid, cmd in procs
        if ppid in protos and f'unit: "{APP}" application' in cmd
    }


def _wait_new(old, timeout=10):
    """Waits for a process that is not in "old"; answers its pid or None."""

    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        new = _app_pids() - old

        if new:
            return new.pop()

        time.sleep(0.01)

    return None


def _kill_and_time(pid, seen):
    """Kills pid and answers the replacement's pid and the gap in seconds."""

    os.kill(pid, signal.SIGKILL)
    killed = time.monotonic()

    seen.add(pid)

    new = _wait_new(seen)
    assert new is not None, f'no replacement for {pid}'

    return new, time.monotonic() - killed


def _setup(skip_alert, skip_fds_check):
    skip_fds_check(main=True, router=True, controller=True)
    skip_alert(r'app process \d+ exited on signal 9', ALERT)

    client.load(
        APP,
        processes={"spare": 1, "max": 1, "idle_timeout": 30},
    )

    pid = _wait_new(set())
    assert pid is not None, 'no application process'

    return pid


def test_python_restart_backoff(
    skip_alert, skip_fds_check, findall, wait_for_record
):
    pid = _setup(skip_alert, skip_fds_check)
    seen = set()

    gaps = []

    for _ in STEPS:
        pid, gap = _kill_and_time(pid, seen)
        gaps.append(gap)

    for step, gap in zip(STEPS, gaps):
        assert step - SLACK <= gap < step + LATE, f'gaps {gaps}'

    assert len(findall(ALERT)) == 1, 'one alert'
    assert findall(NOTICE) == [], 'no answer yet'

    # A request sent during the next delay (800 ms) waits for the
    # replacement and is answered.

    os.kill(pid, signal.SIGKILL)
    killed = time.monotonic()
    seen.add(pid)

    resp = client.get(read_timeout=LATE + 5)
    waited = time.monotonic() - killed

    assert resp['status'] == 200
    assert 0.8 - SLACK <= waited < 0.8 + LATE, f'waited {waited}'

    # The answer ended the back-off.  The router logs that when it releases
    # the request, which can be after the client has the response.

    assert wait_for_record(NOTICE) is not None, 'back-off ended'
    assert len(findall(NOTICE)) == 1, 'one notice'

    pid = _wait_new(seen)
    assert pid is not None, 'no application process'

    # A worker that answered is not a failure.  A counted kill would log a
    # second alert when its timer fires, 100 ms after the kill, so the
    # check reads the log and not the clock.

    pid, gap = _kill_and_time(pid, seen)
    time.sleep(STEPS[0] * 5)
    assert len(findall(ALERT)) == 1, f'answered worker counted, gap {gap}'

    # The next failure starts at the first step again.

    # Without the reset this round would wait 1.6 s.
    pid, gap = _kill_and_time(pid, seen)
    assert STEPS[0] - SLACK <= gap < 1, f'first step: gap {gap}'

    assert client.get()['status'] == 200
    assert len(findall(ALERT)) == 2, 'one alert for each back-off'


def test_python_restart_backoff_reconfigure(
    skip_alert, skip_fds_check, findall
):
    """A changed configuration is a new application: no back-off."""

    pid = _setup(skip_alert, skip_fds_check)
    seen = set()

    for step in STEPS[:2]:
        pid, gap = _kill_and_time(pid, seen)
        assert gap >= step - SLACK, f'gap {gap}'

    assert 'success' in client.conf(
        {"PYTHONDONTWRITEBYTECODE": "1"},
        f'applications/{APP}/environment',
    )

    seen.add(pid)
    pid = _wait_new(seen)
    assert pid is not None, 'no application process'

    # The old application had two rounds: this one would wait 400 ms.
    pid, gap = _kill_and_time(pid, seen)
    assert STEPS[0] - SLACK <= gap < STEPS[2] - SLACK, f'first step: gap {gap}'

    assert client.get()['status'] == 200


def test_python_restart_backoff_restart(skip_alert, skip_fds_check):
    """A /restart ends the back-off; the start that waits runs at once."""

    pid = _setup(skip_alert, skip_fds_check)
    seen = set()

    # Six rounds wait 0.1 + 0.2 + ... + 3.2 s; the seventh would wait 5 s.

    for _ in range(6):
        pid, _ = _kill_and_time(pid, seen)

    os.kill(pid, signal.SIGKILL)
    seen.add(pid)

    # Let the router count the round, so that the start waits for the
    # timer before the restart.
    time.sleep(0.5)
    assert not _app_pids() - seen, 'a process started during the delay'

    restarted = time.monotonic()
    assert 'success' in client.conf_get(f'/control/applications/{APP}/restart')

    pid = _wait_new(seen)
    waited = time.monotonic() - restarted

    assert pid is not None, 'no process after the restart'
    # Without the restart the start would wait about 4.5 s more.
    assert waited < 3, f'waited {waited} s after the restart'

    assert client.get()['status'] == 200
