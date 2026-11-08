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


def _app_procs():
    """Answers the prototypes and the workers of this test's unitd.

    The prototypes are a set of pids; the workers a dict of worker pid to
    the pid of its prototype.  The title alone is not enough: another unitd
    on the host can run an application of the same name.  A worker is a
    child of a prototype of this unitd.
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

    workers = {
        pid: ppid
        for pid, ppid, cmd in procs
        if ppid in protos and f'unit: "{APP}" application' in cmd
    }

    return protos, workers


def _app_pids():
    """Answers the pids of the workers of this test's unitd."""

    return set(_app_procs()[1])


def _adopted():
    processes = client.conf_get(f'/status/applications/{APP}/processes')

    return processes['running'] == 1 and processes['starting'] == 0


def _wait_new(old, timeout=10):
    """Waits for a process that is not in "old"; answers its pid or None.

    The time the process first shows in ps goes to _unit['seen'].  Then the
    wait goes on until the router has adopted the process.  The process
    shows in ps before it sends PROCESS_READY, and a kill before READY is
    a failed start, not an exit: the router gives the slot back and starts
    no replacement for a spare process until a request comes.
    """

    deadline = time.monotonic() + timeout
    pid = None

    while time.monotonic() < deadline:
        if pid is None:
            new = _app_pids() - old

            if new:
                pid = new.pop()
                _unit['seen'] = time.monotonic()

        if pid is not None and _adopted():
            return pid

        time.sleep(0.01)

    return None


def _kill_and_time(pid, seen):
    """Kills pid and answers the replacement's pid and the gap in seconds."""

    os.kill(pid, signal.SIGKILL)
    killed = time.monotonic()

    seen.add(pid)

    new = _wait_new(seen)
    assert new is not None, f'no replacement for {pid}'

    return new, _unit['seen'] - killed


def _setup(skip_alert, skip_fds_check, script=APP, **kwargs):
    skip_fds_check(main=True, router=True, controller=True)
    skip_alert(r'app process \d+ exited on signal 9', ALERT)

    client.load(
        script,
        name=APP,
        processes={"spare": 1, "max": 1, "idle_timeout": 30},
        **kwargs,
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
    assert len(findall(ALERT + r'; .*, now 100 ms, at most 5000 ms')) == 1, (
        'the alert states the bound'
    )
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


@pytest.mark.parametrize('fail', [False, True])
def test_python_restart_backoff_abandoned(
    fail, skip_alert, skip_fds_check, findall, wait_for_record
):
    """An answer to a request the router gave up on ends the back-off.

    The request times out on "limits": {"timeout"} and gets a 503, but the
    worker keeps running it and answers later.  That answer is the worker's
    first one, so the worker is not a failure.  An RPC_ERROR that the worker
    sends, because the application raised, is an answer too.
    """

    pid = _setup(
        skip_alert,
        skip_fds_check,
        script='requeue_late',
        limits={"timeout": 1},
    )
    seen = set()

    pid, gap = _kill_and_time(pid, seen)
    assert gap >= STEPS[0] - SLACK, f'gap {gap}'
    assert len(findall(ALERT)) == 1, 'one alert'

    resp = client.get(
        headers={
            'Host': 'localhost',
            'X-Delay': '2',
            'Connection': 'close',
            **({'X-Fail': '1'} if fail else {}),
        },
        read_timeout=LATE,
    )
    assert resp['status'] == 503, 'timeout'

    assert wait_for_record(NOTICE) is not None, 'late answer ended back-off'

    # A counted kill would log a second alert when its timer fires.

    pid, gap = _kill_and_time(pid, seen)
    time.sleep(STEPS[0] * 5)
    assert len(findall(ALERT)) == 1, f'answered worker counted, gap {gap}'

    assert client.get()['status'] == 200


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


def test_python_restart_backoff_reconfigure_waiting(
    skip_alert, skip_fds_check, findall, wait_for_record
):
    """A changed configuration while a start waits.

    The removed application starts nothing more: the start that waited is
    given back, and its timer forks no process for it.  The new application
    starts at once.  Each application has a prototype of its own, so the
    parent of a worker says which application it serves, and a prototype
    logs the exit of each worker it forked.
    """

    pid = _setup(skip_alert, skip_fds_check)
    seen = set()

    # Six rounds wait 0.1 + 0.2 + ... + 3.2 s; the seventh would wait 5 s.

    for _ in range(6):
        pid, _ = _kill_and_time(pid, seen)

    old_protos, _ = _app_procs()
    assert len(old_protos) == 1, f'prototypes {old_protos}'
    old_proto = next(iter(old_protos))

    os.kill(pid, signal.SIGKILL)
    killed = time.monotonic()
    seen.add(pid)

    # Let the router count the round, so that the start waits for the
    # timer before the configuration changes.
    time.sleep(0.5)
    assert not _app_pids() - seen, 'a process started during the delay'

    changed = time.monotonic()
    assert 'success' in client.conf(
        {"PYTHONDONTWRITEBYTECODE": "1"},
        f'applications/{APP}/environment',
    )

    # Watch until well after the removed application's timer would have
    # fired, 5 s after the kill.

    deadline = killed + 5 + 1
    new_pid = None
    started = None
    old_children = set()

    while time.monotonic() < deadline:
        protos, workers = _app_procs()

        for worker, proto in workers.items():
            if proto in old_protos:
                old_children.add(worker)

            elif new_pid is None:
                new_pid = worker
                started = time.monotonic()

        time.sleep(0.01)

    assert not old_children - seen, (
        f'the removed application started {old_children - seen}'
    )

    # A process that the removed application forks after the change can
    # exit within milliseconds, between two looks at ps: the router frees
    # the application right after that start and QUITs its prototype.  The
    # prototype reaps the process and logs its exit, so the log says what
    # ps can miss.  The killed workers are the only exits it may report.

    exits = {
        int(exited)
        for exited in findall(
            fr'{old_proto}#{old_proto} app process (\d+) exited'
        )
    }
    assert exits <= seen, f'the removed application forked {exits - seen}'

    assert new_pid is not None, 'no process for the new application'

    # Without the cancel the old start would come about 4.5 s after the
    # change; the new application does not wait for it.
    waited = started - changed
    assert waited < 3, f'the new application waited {waited} s'

    assert not (old_protos & protos), 'the old prototype is still running'

    # The removal ended the back-off of the old application.
    assert wait_for_record(NOTICE) is not None, 'back-off ended'

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
    assert pid is not None, 'no process after the restart'
    waited = _unit['seen'] - restarted

    # Without the restart the start would wait about 4.5 s more.
    assert waited < 3, f'waited {waited} s after the restart'

    assert client.get()['status'] == 200


def test_python_restart_backoff_slow_ready(skip_alert, skip_fds_check):
    """A worker shows in ps before it is ready; a kill waits for READY.

    A kill before PROCESS_READY is a failed start, not an exit, and the
    router starts no spare process for it.  Each worker here takes 0.3 s
    from its first line in ps to READY, so a kill that is timed on ps alone
    always lands in that window, and no replacement comes.
    """

    pid = _setup(
        skip_alert,
        skip_fds_check,
        script='slow_start',
        environment={'UNIT_SLOW_START': '0.3'},
    )
    seen = set()

    for step in STEPS[:2]:
        pid, gap = _kill_and_time(pid, seen)
        assert gap >= step - SLACK, f'gap {gap}'

    assert client.get()['status'] == 200
