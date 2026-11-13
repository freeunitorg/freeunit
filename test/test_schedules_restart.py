"""The "schedules" configuration object when the router or unitd starts
again (./configure --scheduler); see test_schedules.py.

The main process keeps the record of each schedule, with the run that is
in progress.  So a new router does not get a free slot for a schedule
whose run the old router asked for (R3 of src/nxt_schedule.h), and it
shows the counters of before.  When unitd stops, main kills the group of
each run and waits for it.  A unitd that starts with a stored
configuration runs its schedules at the same slots as the one before:
slots are multiples of the interval since the Epoch.

The last two tests stop unitd, so they need --restart.
"""

import re
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

import conftest
from conftest import unit_run, unit_stop
from test_schedules import (
    alive,
    cleanup,
    client,
    lines,
    put,
    recorded,
    schedule,
    script,
    setup_method_fixture,  # noqa: F401, the fixture of this module too
    wait_for,
    wait_status,
)
from unit.log import Log

PATTERN_ROUTER = 'unit: router'


def pid_by_name(name, ppid):
    output = subprocess.check_output(['ps', 'ax', '-O', 'ppid']).decode()
    m = re.search(fr'\s*(\d+)\s*{ppid}.*{name}', output)
    return None if m is None else int(m.group(1))


def run_with_stored_conf(statedir):
    """unit_run() ends with a check that a new unitd has no application.
    One that starts with a stored configuration of a schedule has the
    application of that schedule, and nothing else is wrong with it."""

    try:
        unit_run(state_dir=str(statedir))
    except AssertionError:
        pass

    status = client.conf_get('/status')

    assert list(status['applications']) == ['app']
    assert status['requests'] == {'total': 0}

    return conftest.unit_instance['temp_dir']


def test_schedules_router_restart(
    temp_dir, skip_alert, skip_fds_check, unit_pid
):
    """R3 with a new router: the run that the old router asked for holds
    the slot, and the counters are the ones of before."""

    skip_fds_check(router=True)

    path = script(
        temp_dir,
        'job.sh',
        'mkdir $d/lock 2>/dev/null || echo overlap >> $d/overlap\n'
        'sleep 3.6\n'
        'echo done >> $d/out\n'
        'rmdir $d/lock\n',
    )

    assert 'success' in put(temp_dir, {"tick": schedule(path)})

    before = wait_status(lambda s: s['running'] == 1)

    router = pid_by_name(PATTERN_ROUTER, unit_pid)
    assert router is not None

    skip_alert(fr'process {router} exited on signal 9')
    subprocess.call(['kill', '-9', str(router)])

    assert wait_for(
        lambda: pid_by_name(PATTERN_ROUTER, unit_pid) not in (None, router)
    ), 'a new router'

    # The new router has the configuration again and asks for a run at its
    # first slot.  Main refuses: the run of before is not reaped.
    st = wait_status(lambda s: s['skipped'] > before['skipped'], timeout=15)

    assert st['running'] == 1
    assert st['pid'] == before['pid']
    assert st['runs'] == before['runs']
    assert st['last_start'] == before['last_start']

    assert lines(temp_dir, 'out') == [], 'the run of before is in progress'

    assert wait_for(lambda: lines(temp_dir, 'out') == ['done'], 10)

    # And the new router gets the slot when that run is reaped.
    wait_status(lambda s: s['runs'] > before['runs'])

    assert not Path(temp_dir, 'overlap').exists(), 'two runs at once'


def test_schedules_stop_kills_group(requires_restart, temp_dir):
    """When unitd stops, the process group of a run gets SIGKILL, and
    main waits for the run before it exits."""

    path = script(
        temp_dir,
        'job.sh',
        "trap '' TERM\n"
        'sleep 300 &\n'
        'echo $! >> $d/members\n'
        'sleep 300\n',
    )

    assert 'success' in put(temp_dir, {"tick": schedule(path)})

    st = wait_status(lambda s: s['running'] == 1)

    assert wait_for(lambda: recorded(temp_dir, 'members'))

    leader = st['pid']
    member = recorded(temp_dir, 'members')[0]

    assert alive(leader) and alive(member)

    assert unit_stop() is None, 'unitd stopped in time, with status 0'

    assert not alive(leader), 'a run after unitd has stopped'
    assert wait_for(lambda: not alive(member), 3), 'the group of that run'


def test_schedules_unit_restart(requires_restart):
    """A unitd that starts with a stored configuration runs its schedules,
    at the same slots as the unitd before it."""

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    dirs = []

    try:
        first = unit_run(state_dir=str(statedir))['temp_dir']
        dirs.append(first)

        path = script(first, 'job.sh', 'date +%s >> $d/starts\n')

        assert 'success' in put(first, {"tick": schedule(path, interval=2)})

        wait_status(lambda s: s['runs'] >= 1 and not s['running'])

        log = Path(first, 'unit.log').read_text(encoding='utf-8')

        assert unit_stop() is None

        # The teardown checks only the log of the last unitd.
        Log.check_alerts(log=log)

        count = len(lines(first, 'starts'))
        stopped = time.time()

        dirs.append(run_with_stored_conf(statedir))

        # The counters are those of the new main process.
        st = wait_status(lambda s: s['runs'] >= 2 and not s['running'])

        assert st['runs'] < 10

        starts = [int(x) for x in lines(first, 'starts')]

        assert len(starts) >= count + 2
        assert all(s % 2 == 0 for s in starts), starts
        assert starts == sorted(set(starts)), 'one run in a slot'
        assert starts[count] - stopped <= 4, 'the first slot after the start'

        cleanup(first)

    finally:
        unit_stop()

        shutil.rmtree(statedir, ignore_errors=True)

        for path in dirs[:-1]:
            shutil.rmtree(path, ignore_errors=True)
