"""The "schedules" configuration object: a command that runs at an interval
in the context of an application (./configure --scheduler).

A schedule names an application, an executable with its arguments, an
interval in seconds and an optional timeout.  The router keeps the clock:
slots are multiples of the interval since the Epoch.  The main process
forks each run with the isolation, the user, the environment and the
working directory of the application, kills its process group at the
timeout and reaps it.  /status/schedules shows the state that main keeps.

The rules that the tests with R1, R2 and R3 in their docstring check are
in src/nxt_schedule.h:

  R1  a slot is busy until main's waitpid() returns the process of the
      run, and no longer;
  R2  a timeout means SIGKILL to the process group of the run;
  R3  for one schedule, main never starts a run while the process of the
      previous run is not reaped.  A process that left the group, or that
      outlives a run which ended by itself, is out of scope.

The application here is an "external" one with no spare process: nothing
starts it, so the tests need no language module.  A run takes the context
of the application, not its code.

The commands are shell scripts.  A script appends its pid to "leaders" in
the temporary directory and the pid of each process it leaves behind to
"members", so that the teardown can wait for the first and kill the second.
The tests never signal the process of a run: main would report that as an
alert.

A build without --scheduler refuses "schedules"; the one test for that has
"unsupported" in its name and runs only there.
"""

import os
import shutil
import signal
import time
from pathlib import Path

import pytest

from unit.applications.proto import ApplicationProto
from unit.option import option

client = ApplicationProto()

STATUS_KEYS = {
    'runs',
    'skipped',
    'failed',
    'timed_out',
    'running',
    'pid',
    'last_start',
    'last_exit',
    'last_signal',
    'last_duration_ms',
}


@pytest.fixture(autouse=True)
def setup_method_fixture(request, temp_dir):
    built = option.available['features']['scheduler']
    unsupported = 'unsupported' in request.node.name

    if unsupported and built:
        pytest.skip('unitd is built with --scheduler')

    if not unsupported and not built:
        pytest.skip('unitd is built without --scheduler')

    yield

    cleanup(temp_dir)


def cleanup(temp_dir):
    # No new runs, then wait for the ones that started.  A leader ends by
    # itself or by its timeout; what a run left behind is killed here.

    # Not when the test has stopped unitd: main removes the socket then.
    if Path(temp_dir, 'control.unit.sock').exists():
        client.conf({"listeners": {}, "applications": {}}, '/config')

    for pid in recorded(temp_dir, 'leaders'):
        wait_for(lambda: not alive(pid), 20)

    for pid in recorded(temp_dir, 'members'):
        kill_quietly(pid)


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True

    # A zombie has a pid too, and it is not running anything.
    try:
        state = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return state[0] != 'Z'
    except (OSError, IndexError):
        return True


def kill_quietly(pid):
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass


def recorded(temp_dir, name):
    try:
        text = Path(temp_dir, name).read_text()
    except OSError:
        return []

    return [int(x) for x in text.split()]


def wait_for(cond, timeout=10, step=0.05):
    end = time.monotonic() + timeout

    while True:
        value = cond()

        if value or time.monotonic() >= end:
            return value

        time.sleep(step)


def app(temp_dir, **kwargs):
    conf = {
        "type": "external",
        "executable": "/bin/true",
        "processes": {"max": 1, "spare": 0},
        "working_directory": temp_dir,
    }
    conf.update(kwargs)

    return conf


def script(temp_dir, name, body):
    """A shell script that records its pid first."""

    path = Path(temp_dir, name)
    path.write_text(f'#!/bin/sh\nd={temp_dir}\necho $$ >> $d/leaders\n{body}')
    path.chmod(0o755)

    return str(path)


def schedule(path, interval=1, **kwargs):
    conf = {
        "application": "app",
        "executable": "/bin/sh",
        "arguments": [path],
        "interval": interval,
    }
    conf.update(kwargs)

    return conf


def put(temp_dir, schedules, **app_kwargs):
    return client.conf(
        {
            "listeners": {},
            "applications": {"app": app(temp_dir, **app_kwargs)},
            "schedules": schedules,
        },
        '/config',
    )


def status(name='tick'):
    resp = client.conf_get('/status/schedules')

    return resp.get(name) if isinstance(resp, dict) else None


def wait_status(cond, name='tick', timeout=10):
    def check():
        st = status(name)
        return st if st is not None and cond(st) else None

    st = wait_for(check, timeout)
    assert st is not None, f'schedule state: {status(name)}'

    return st


def lines(temp_dir, name):
    try:
        return Path(temp_dir, name).read_text().splitlines()
    except OSError:
        return []


# Validation.


def test_schedules_unsupported(temp_dir):
    """A build without --scheduler names the build, not a typing error."""

    resp = put(temp_dir, {"tick": schedule("/bin/true")})

    assert 'error' in resp
    assert 'built without' in resp['detail']
    assert '"schedules"' in resp['detail']

    assert 'error' in client.conf({}, '/config/schedules')
    assert 'schedules' not in client.conf_get('/status')


def test_schedules_valid(temp_dir):
    """The limits themselves are valid."""

    assert 'success' in put(
        temp_dir,
        {
            # 64 characters, with all the kinds that a name can have.
            "A" + "b0._-" * 12 + "xyz": {
                "application": "app",
                "executable": "/bin/true",
                "arguments": ["x" * 127] * 64,
                "interval": 2147483,
                "timeout": 2147483,
            },
            "0": {
                "application": "app",
                "executable": "relative/path",
                "arguments": [],
                "interval": 1,
            },
        },
    )

    assert 'success' in client.conf({}, '/config/schedules')


@pytest.mark.parametrize(
    'name',
    ['', '-a', '.a', '_a', 'a b', 'a/b', 'a"b', 'é', 'a\n', 'x' * 65],
)
def test_schedules_invalid_name(temp_dir, name):
    resp = put(temp_dir, {name: schedule("/bin/true")})

    assert 'error' in resp, name
    assert 'schedule name' in resp['detail'], name


@pytest.mark.parametrize(
    'change, reason',
    [
        ({"application": None}, 'Required parameter "application"'),
        ({"application": "other"}, 'is not an application'),
        ({"application": 1}, '"application"'),
        ({"executable": None}, 'Required parameter "executable"'),
        ({"executable": ""}, '"executable" must not be empty'),
        ({"executable": "a\u0000b"}, 'null character'),
        ({"executable": ["/bin/true"]}, '"executable"'),
        ({"arguments": "x"}, '"arguments"'),
        ({"arguments": ["x"] * 65}, 'more than 64 elements'),
        ({"arguments": ["x", 1]}, 'only string values'),
        ({"arguments": ["a\u0000b"]}, 'null character'),
        # 8192 bytes with the NUL of each argument are valid, one more
        # is not.
        ({"arguments": ["x" * 4095, "x" * 4096]}, 'more than 8192 bytes'),
        ({"interval": None}, 'Required parameter "interval"'),
        ({"interval": 0}, '"interval" must be from 1 to 2147483'),
        ({"interval": -1}, '"interval" must be from 1 to 2147483'),
        ({"interval": 2147484}, '"interval" must be from 1 to 2147483'),
        ({"interval": 1.5}, '"interval"'),
        ({"interval": "1"}, '"interval"'),
        ({"timeout": 0}, '"timeout" must be from 1 to 2147483'),
        ({"timeout": 2147484}, '"timeout" must be from 1 to 2147483'),
        ({"timeout": 0.5}, '"timeout"'),
        ({"jitter": 1}, 'Unknown parameter "jitter"'),
        ({"uri": "/"}, 'Unknown parameter "uri"'),
    ],
)
def test_schedules_invalid(temp_dir, change, reason):
    conf = schedule("/bin/true")

    for key, value in change.items():
        if value is None:
            del conf[key]
        else:
            conf[key] = value

    resp = put(temp_dir, {"tick": conf})

    assert 'error' in resp, change
    assert reason in resp['detail'], change


def test_schedules_invalid_container(temp_dir):
    assert 'error' in put(temp_dir, [])
    assert 'error' in put(temp_dir, {"tick": []})

    # The largest valid size of the arguments.
    assert 'success' in put(
        temp_dir,
        {"tick": schedule("/bin/true", arguments=["x" * 4095, "x" * 4095])},
    )

    # An application that a schedule names cannot be deleted under it.
    assert 'error' in client.conf_delete('/config/applications/app')
    assert 'success' in client.conf_delete('/config/schedules/tick')
    assert 'success' in client.conf_delete('/config/applications/app')


# Runs.


def test_schedules_run_and_status(temp_dir):
    """A schedule runs at each slot, and the status counts the runs."""

    assert 'schedules' not in client.conf_get('/status')

    path = script(temp_dir, 'job.sh', 'echo run >> $d/out\n')

    assert 'success' in put(
        temp_dir,
        {"tick": schedule(path), "idle": schedule(path, interval=2147483)},
    )

    # Known at once, before its first slot.
    assert status('idle') == {
        'runs': 0,
        'skipped': 0,
        'failed': 0,
        'timed_out': 0,
        'running': 0,
        'pid': 0,
        'last_start': 0,
        'last_exit': -1,
        'last_signal': 0,
        'last_duration_ms': 0,
    }

    before = time.time()

    st = wait_status(lambda s: s['runs'] >= 2 and not s['running'])

    assert set(st) == STATUS_KEYS
    assert st['skipped'] == 0
    assert st['failed'] == 0
    assert st['timed_out'] == 0
    assert st['pid'] == 0
    assert st['last_exit'] == 0
    assert st['last_signal'] == 0
    assert 0 <= st['last_duration_ms'] < 2000
    assert before - 1 <= st['last_start'] <= time.time()

    assert len(lines(temp_dir, 'out')) >= 2

    # The schedules go, their state goes, and nothing runs any more.
    assert 'success' in client.conf_delete('/config/schedules')
    assert 'schedules' not in client.conf_get('/status')

    time.sleep(0.3)
    count = len(lines(temp_dir, 'out'))
    time.sleep(2.2)
    assert len(lines(temp_dir, 'out')) == count


def test_schedules_context(temp_dir):
    """A run has the environment and the working directory of the
    application, the arguments of the schedule, the standard descriptors
    only, no blocked and no ignored signal, and a process group of its
    own."""

    work = Path(temp_dir, 'work dir')
    work.mkdir()

    # "ls" and "grep" are children of the command, so they show what it
    # got: its descriptors with the one more that "ls" reads the directory
    # through, and the signals that are blocked or ignored at its start.
    command = (
        'echo $$ >> "$0/leaders"; '
        'echo "env=$SCHED_VAR" > "$0/context"; '
        'echo "cwd=$PWD" >> "$0/context"; '
        'echo "args=$#:$1:$2:$3" >> "$0/context"; '
        'echo "pgid=$(ps -o pgid= -p $$ | tr -d " ")=$$" >> "$0/context"; '
        'ls /proc/self/fd > "$0/fds"; '
        'grep -E "^Sig(Blk|Ign):" /proc/self/status > "$0/signals"'
    )

    assert 'success' in put(
        temp_dir,
        {
            "tick": {
                "application": "app",
                "executable": "/bin/sh",
                "arguments": ["-c", command, temp_dir, "one", "", "two words"],
                "interval": 1,
            }
        },
        working_directory=str(work),
        environment={"SCHED_VAR": "from the application"},
    )

    wait_status(lambda s: s['runs'] >= 1 and not s['running'])

    assert 'success' in client.conf_delete('/config/schedules/tick')

    context = dict(line.split('=', 1) for line in lines(temp_dir, 'context'))

    assert context['env'] == 'from the application'
    assert context['cwd'] == str(work)
    assert context['args'] == '3:one::two words'

    pgid, pid = context['pgid'].split('=')
    assert pgid == pid, 'the run leads its own process group'

    if Path('/proc/self/fd').is_dir():
        assert lines(temp_dir, 'fds') == ['0', '1', '2', '3']

        masks = dict(line.split(':') for line in lines(temp_dir, 'signals'))

        assert int(masks['SigBlk'], 16) == 0, 'no blocked signal'
        assert int(masks['SigIgn'], 16) == 0, 'no ignored signal'


def test_schedules_slots_aligned(temp_dir):
    """Slots are multiples of the interval since the Epoch: a schedule of
    3 s starts at seconds that 3 divides, whenever it was configured."""

    path = script(temp_dir, 'job.sh', 'date +%s >> $d/starts\n')

    # Not at a slot: the first run is at the next multiple, not now.
    while time.time() % 3 > 2.5 or time.time() % 3 < 0.3:
        time.sleep(0.1)

    applied = time.time()

    assert 'success' in put(temp_dir, {"tick": schedule(path, interval=3)})

    st = wait_status(lambda s: s['runs'] >= 2 and not s['running'])

    starts = [int(x) for x in lines(temp_dir, 'starts')]

    assert len(starts) >= 2
    assert all(s % 3 == 0 for s in starts), starts
    assert starts[0] - applied <= 3, 'the first slot after the configuration'
    assert starts[1] - starts[0] == 3
    assert st['last_start'] % 3 == 0


def test_schedules_overlap_skipped(temp_dir):
    """R3: a slot that comes while the previous run is not reaped is
    skipped, and two runs of one schedule never overlap."""

    path = script(
        temp_dir,
        'job.sh',
        'mkdir $d/lock 2>/dev/null || echo overlap >> $d/overlap\n'
        'echo "start $(date +%s.%N)" >> $d/log\n'
        'sleep 2.4\n'
        'echo "end $(date +%s.%N)" >> $d/log\n'
        'rmdir $d/lock\n',
    )

    assert 'success' in put(temp_dir, {"tick": schedule(path)})

    st = wait_status(lambda s: s['runs'] >= 2 and s['skipped'] >= 2, timeout=12)

    assert st['failed'] == 0

    # While a run is in progress the status names its process.
    st = wait_status(lambda s: s['running'] == 1)
    assert st['pid'] in recorded(temp_dir, 'leaders')

    assert 'success' in client.conf_delete('/config/schedules/tick')

    for pid in recorded(temp_dir, 'leaders'):
        assert wait_for(lambda: not alive(pid), 10)

    assert not Path(temp_dir, 'overlap').exists(), 'two runs at once'

    log = [line.split() for line in lines(temp_dir, 'log')]

    assert [kind for kind, _ in log] == ['start', 'end'] * (len(log) // 2)


def test_schedules_other_schedule_not_blocked(temp_dir):
    """R3 is for one schedule: a run of one does not hold another."""

    slow = script(temp_dir, 'slow.sh', 'sleep 2.4\n')
    fast = script(temp_dir, 'fast.sh', 'echo run >> $d/fast\n')

    assert 'success' in put(
        temp_dir, {"slow": schedule(slow), "fast": schedule(fast)}
    )

    wait_status(lambda s: s['skipped'] >= 1, 'slow')

    st = status('fast')
    assert st['runs'] >= 2
    assert st['skipped'] == 0


def test_schedules_timeout_kills_group(temp_dir):
    """R2: at the timeout the process group of the run gets SIGKILL: the
    run, which ignores SIGTERM, and a process that it started and does not
    wait for."""

    path = script(
        temp_dir,
        'job.sh',
        "trap '' TERM\n"
        'sleep 300 &\n'
        'echo $! >> $d/members\n'
        'sleep 300\n',
    )

    assert 'success' in put(temp_dir, {"tick": schedule(path, timeout=1)})

    st = wait_status(lambda s: s['timed_out'] >= 1 and not s['running'])

    assert st['failed'] == 0, 'a timeout is not a failure'
    assert st['last_exit'] == -1
    assert st['last_signal'] == signal.SIGKILL
    assert 900 <= st['last_duration_ms'] < 3000

    assert 'success' in client.conf_delete('/config/schedules/tick')

    leaders = recorded(temp_dir, 'leaders')
    members = recorded(temp_dir, 'members')

    assert leaders and members

    for pid in leaders:
        assert wait_for(lambda: not alive(pid), 10), 'a run after its timeout'

    left = [pid for pid in members if not wait_for(lambda: not alive(pid), 3)]

    assert not left, 'a process of the group of a run outlived the timeout'


def test_schedules_timeout_not_reached(temp_dir):
    """A run that ends before its timeout is not killed, and the timeout
    of one run does not reach the next."""

    path = script(temp_dir, 'job.sh', 'sleep 0.3\necho done >> $d/out\n')

    assert 'success' in put(temp_dir, {"tick": schedule(path, timeout=2)})

    st = wait_status(lambda s: s['runs'] >= 4 and not s['running'])

    assert st['timed_out'] == 0
    assert st['failed'] == 0
    assert len(lines(temp_dir, 'out')) >= 4


@pytest.mark.skipif(shutil.which('setsid') is None, reason='no setsid')
def test_schedules_timeout_scope(temp_dir):
    """The scope of R2 and R3: a process that left the group of the run is
    not killed at the timeout, and it does not hold the slot."""

    path = script(
        temp_dir,
        'job.sh',
        'setsid sleep 300 &\n'
        'echo $! >> $d/members\n'
        'sleep 300\n',
    )

    assert 'success' in put(temp_dir, {"tick": schedule(path, timeout=1)})

    wait_status(lambda s: s['timed_out'] >= 1)
    wait_status(lambda s: s['runs'] >= 2)

    members = recorded(temp_dir, 'members')

    assert alive(members[0]), 'a process in a session of its own'


def test_schedules_slot_free_after_exit(temp_dir):
    """R1, "and no longer": the slot is free when the process of the run
    is reaped, also when a process that it started lives on."""

    path = script(
        temp_dir,
        'job.sh',
        'sleep 300 &\n'
        'echo $! >> $d/members\n'
        'echo run >> $d/out\n',
    )

    assert 'success' in put(temp_dir, {"tick": schedule(path)})

    st = wait_status(lambda s: s['runs'] >= 3 and not s['running'])

    assert st['skipped'] == 0
    assert st['failed'] == 0
    assert st['last_exit'] == 0

    members = recorded(temp_dir, 'members')

    assert len(members) >= 3
    assert alive(members[0]), 'main does not track what a run left behind'


def test_schedules_failed(temp_dir):
    """A status other than zero, and a command that cannot be executed,
    count as failed."""

    path = script(temp_dir, 'job.sh', 'exit 3\n')

    assert 'success' in put(temp_dir, {"tick": schedule(path)})

    st = wait_status(lambda s: s['failed'] >= 1 and not s['running'])

    assert st['failed'] == st['runs']
    assert st['last_exit'] == 3
    assert st['last_signal'] == 0
    assert st['timed_out'] == 0

    missing = f'{temp_dir}/no such file'

    assert 'success' in put(
        temp_dir,
        {
            "missing": {
                "application": "app",
                "executable": missing,
                "interval": 1,
            }
        },
    )

    st = wait_status(lambda s: s['failed'] >= 1 and not s['running'], 'missing')

    # As a shell reports a command that it did not find.
    assert st['last_exit'] == 127
    assert st['failed'] == st['runs']


def test_schedules_signal(temp_dir, skip_alert):
    """A run that a signal ends, other than the kill of its timeout, is a
    failure, and main reports the signal as for any of its children."""

    skip_alert(r'process \d+ exited on signal 15')

    path = script(temp_dir, 'job.sh', 'kill -TERM $$\nsleep 5\n')

    assert 'success' in put(temp_dir, {"tick": schedule(path)})

    st = wait_status(lambda s: s['failed'] >= 1 and not s['running'])

    assert st['last_exit'] == -1
    assert st['last_signal'] == signal.SIGTERM
    assert st['timed_out'] == 0


def test_schedules_application_changes(temp_dir):
    """A run has the application of the configuration in effect."""

    path = script(temp_dir, 'job.sh', 'echo "$SCHED_VAR" >> $d/out\n')

    assert 'success' in put(
        temp_dir, {"tick": schedule(path)}, environment={"SCHED_VAR": "one"}
    )

    wait_for(lambda: 'one' in lines(temp_dir, 'out'))

    assert 'success' in client.conf(
        {"SCHED_VAR": "two"}, '/config/applications/app/environment'
    )

    assert wait_for(lambda: 'two' in lines(temp_dir, 'out'))

    out = lines(temp_dir, 'out')

    assert 'one' not in out[out.index('two') :], 'no run of the old one'

    # The counters are of the schedule, not of one configuration of it.
    assert status()['runs'] >= 2


def test_schedules_removed_during_run(temp_dir):
    """R3 across a change of the configuration: a schedule that is removed
    while it runs lets the run end, and when it comes back under the same
    name before that, its slot is still busy."""

    path = script(
        temp_dir,
        'job.sh',
        'mkdir $d/lock 2>/dev/null || echo overlap >> $d/overlap\n'
        'sleep 2.6\n'
        'echo done >> $d/out\n'
        'rmdir $d/lock\n',
    )

    assert 'success' in put(temp_dir, {"tick": schedule(path)})

    wait_status(lambda s: s['running'] == 1)

    assert 'success' in client.conf_delete('/config/schedules/tick')
    assert status() is None

    assert 'success' in client.conf(schedule(path), '/config/schedules/tick')

    # The record in main is the one of the run in progress.
    st = wait_status(lambda s: s['skipped'] >= 1)

    assert st['running'] == 1
    assert st['runs'] == 1

    assert wait_for(lambda: lines(temp_dir, 'out') == ['done'], 10)

    wait_status(lambda s: s['runs'] >= 2)

    assert not Path(temp_dir, 'overlap').exists(), 'two runs at once'


def test_schedules_context_fails(temp_dir):
    """A run whose context cannot be set up ends before the command and
    counts as failed.  The process ends before it is ready, so main still
    has its port then."""

    path = script(temp_dir, 'job.sh', 'echo run >> $d/out\n')

    assert 'success' in put(
        temp_dir,
        {"tick": schedule(path)},
        working_directory=f'{temp_dir}/no such directory',
    )

    st = wait_status(lambda s: s['failed'] >= 2 and not s['running'])

    assert st['failed'] == st['runs']
    assert st['last_exit'] == 1
    assert lines(temp_dir, 'out') == [], 'the command did not run'

    # The application is fixed: the next run is a good one.
    assert 'success' in client.conf(
        f'"{temp_dir}"', '/config/applications/app/working_directory'
    )

    wait_status(lambda s: s['last_exit'] == 0 and not s['running'])

    assert lines(temp_dir, 'out')


def test_schedules_isolation_pid(temp_dir, require):
    """A run has the namespaces of the application.  In a pid namespace it
    is process 1, main still reaps it, and its timeout still ends it
    (R2)."""

    require({'features': {'isolation': ['unprivileged_userns_clone', 'pid']}})

    command = 'echo $$ >> "$0/pids"; test -e "$0/hang" && sleep 300; exit 0'

    assert 'success' in put(
        temp_dir,
        {
            "tick": {
                "application": "app",
                "executable": "/bin/sh",
                "arguments": ["-c", command, temp_dir],
                "interval": 1,
                "timeout": 1,
            }
        },
        isolation={"namespaces": {"credential": True, "pid": True}},
    )

    st = wait_status(lambda s: s['runs'] >= 2 and not s['running'])

    assert st['failed'] == 0
    assert st['last_exit'] == 0
    assert set(lines(temp_dir, 'pids')) == {'1'}

    Path(temp_dir, 'hang').touch()

    st = wait_status(lambda s: s['timed_out'] >= 1 and not s['running'])

    assert st['last_signal'] == signal.SIGKILL

    Path(temp_dir, 'hang').unlink()

    assert 'success' in client.conf_delete('/config/schedules')

    # The teardown has no pid to wait for: "leaders" is not written here.
    time.sleep(1.5)
