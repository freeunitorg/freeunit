"""A worker that starts but never announces itself must not strand the start.

An application process is only ever announced by the process itself, from
nxt_unit_init() -> PROCESS_READY.  A `"type": "external"` binary that never
gets that far -- /bin/sleep is the shortest example, but a wrapper script or a
runtime blocked in its own init reaches the same state -- answers nothing, and
because it stays alive nothing kills it either, so no REMOVE_PID converts the
wait into an error.

Without a "limits": {"start_timeout"} that leaves the router's START_PROCESS
RPC armed for good.  With the default "processes" that RPC is the only
continuation of nxt_router_conf_apply(), so:

  * the configuration PUT never returned, and
  * the controller parks the in-flight request at the head of its queue, so
    nxt_controller_check_postpone_request() queued every later control request
    behind it -- GET /status included.

The whole control plane was unavailable for as long as that process lived.

The bound is opt-in: NXT_APP_START_TIMEOUT is 0, because the window it
measures is the application's own startup and a default would fail a slow but
honest one (see the comment on that macro).  So these tests set it explicitly,
and test_app_start_timeout_default_is_unbounded() pins the default.

Every test here would hang rather than fail on a build without the deadline,
which is why the module is worth running under `timeout` when comparing.
"""

import re
import subprocess
import time

import pytest

from unit.control import Control
from unit.log import Log
from unit.option import option

prerequisites = {}

client = Control()


# Seconds, like every other "limits" member (NXT_CONF_MAP_MSEC).  Long enough
# that a slow-but-honest start is not mistaken for a wedged one, short enough
# to keep the module quick.  There is no product default to inherit:
# NXT_APP_START_TIMEOUT is 0, meaning unbounded.
START_TIMEOUT = 3

STUCK_ARG = 'unit-test-app-start-timeout'


def _stuck_conf(start_timeout=START_TIMEOUT):
    app = {
        'type': 'external',
        # `sh -c CMD NAME` runs CMD with $0 = NAME, which puts a marker in the
        # process title that _stuck_pids() can grep for without matching a
        # sleep that belongs to something else on the box.  The trailing `:`
        # keeps sh from exec'ing the sleep over itself, which would drop the
        # marker again.
        'executable': '/bin/sh',
        'arguments': ['-c', 'sleep 600; :', STUCK_ARG],
        'limits': {'start_timeout': start_timeout},
    }

    return {
        'listeners': {'*:8080': {'pass': 'applications/stuck'}},
        'applications': {'stuck': app},
    }


def _serving_conf():
    return {
        'listeners': {'*:8080': {'pass': 'routes'}},
        'routes': [{'action': {'return': 200}}],
    }


def _stuck_pids():
    """Pids of the workers this module started, if any are still alive."""

    res = subprocess.run(
        ['pgrep', '-f', STUCK_ARG],
        capture_output=True,
        text=True,
        check=False,
    )

    return [int(p) for p in res.stdout.split()]


def _stuck_protos():
    """Pids of the prototypes started for the wedging application.

    A prototype cannot exit while a child of it is alive, so it is half of
    what a leaked start leaves behind and has to be counted separately: the
    marker in _stuck_pids() is the worker's, not the prototype's.
    """

    res = subprocess.run(
        ['pgrep', '-f', 'unit: "stuck" prototype'],
        capture_output=True,
        text=True,
        check=False,
    )

    return [int(p) for p in res.stdout.split()]


def _wait_gone(seconds=10):
    """Wait for every worker and prototype of the wedging app to be gone."""

    for _ in range(int(seconds * 10)):
        if _stuck_pids() == [] and _stuck_protos() == []:
            break

        time.sleep(0.1)

    return _stuck_pids(), _stuck_protos()


def _children(pid):
    """Pids the kernel currently lists as children of pid."""

    try:
        with open(f'/proc/{pid}/task/{pid}/children', encoding='utf-8') as f:
            return [int(p) for p in f.read().split()]

    except OSError:
        return []


def _reap_stuck():
    """Kill the silent workers, and the `sleep` each of them forked.

    The marker is the shell's $0, so _stuck_pids() finds the shell and never
    the `sleep 600` it forked -- and killing only the shell reparents that
    sleep to the nearest subreaper, where it keeps running.  Nothing in this
    module notices it afterwards: the pid no longer carries the marker and no
    longer has a parent we know.  Unit's own process group is the backstop
    (conftest reaps it in unit_stop), but Unit is started once per session, so
    until then one sleep per test survives into every module that follows.

    Collect the children before killing the shell: afterwards they are not its
    children any more.
    """

    for pid in _stuck_pids():
        children = _children(pid)

        subprocess.run(['kill', '-9', str(pid)], check=False)

        for child in children:
            subprocess.run(['kill', '-9', str(child)], check=False)


@pytest.fixture(autouse=True)
def _stuck_app(skip_alert):
    """Every test here deliberately produces alerts; name them all once.

    * the deadline's own alert;
    * "failed to apply new conf", which nxt_router_conf_error() logs for the
      PUT the deadline makes fail;
    * "exited on signal", which the prototype logs for a silent worker: the
      router has no pid for one and cannot kill it, so either the test reaps
      it or the prototype does, one "start_timeout" after it is told to quit
      (see nxt_proto_quit_children(), and the three tests that pin it below).
    """

    skip_alert(
        r'did not become ready in time',
        r'failed to apply new conf',
        r'exited on signal',
    )

    yield

    # A silent worker outlives the start that asked for it, and while its
    # application is live nothing collects it -- the router never learns its
    # pid.  Clean up regardless, so a failure here does not leave sleeps
    # behind for the next run.
    _reap_stuck()


def test_app_start_timeout_put_fails():
    assert 'success' in client.conf(_serving_conf()), 'baseline configured'

    started = time.monotonic()

    resp = client.conf(_stuck_conf())

    elapsed = time.monotonic() - started

    # The PUT must fail, not hang and not silently succeed.
    assert 'error' in resp, f'stuck app rejected, got {resp}'
    assert 'Failed to apply new configuration' in resp['error'], resp

    # Bounded by the deadline, with room for the fork and the round trips.
    assert elapsed < START_TIMEOUT + 10, f'PUT returned in {elapsed:.1f}s'

    # And not *before* the deadline: a start that is merely slow must not be
    # failed early, so the bound has to be the configured one.
    assert elapsed >= START_TIMEOUT - 1, f'PUT returned in {elapsed:.1f}s'


def test_app_start_timeout_control_plane_survives():
    assert 'success' in client.conf(_serving_conf()), 'baseline configured'

    assert 'error' in client.conf(_stuck_conf()), 'stuck app rejected'

    # The symptom that made this severe: with the request parked at the head
    # of the controller's queue, every later control request queued behind it.
    started = time.monotonic()

    status = client.conf_get('/status')

    assert time.monotonic() - started < 10, 'GET /status answers promptly'
    assert 'connections' in status, status

    # The failed PUT must not have installed anything.
    assert 'stuck' not in status['applications'], status['applications']

    # And the previous configuration is still the live one, still serving.
    assert client.conf_get() == _serving_conf(), 'previous config retained'

    resp = client.get(url='/')

    assert resp['status'] == 200, 'previous config still serves'


def test_app_start_timeout_alert(wait_for_record):
    assert 'success' in client.conf(_serving_conf()), 'baseline configured'
    assert 'error' in client.conf(_stuck_conf()), 'stuck app rejected'

    assert (
        wait_for_record(
            r'app "stuck" process did not become ready in time', wait=50
        )
        is not None
    ), 'the alert names the application'

    assert re.search(
        r'start_timeout', Log.read()
    ), 'the alert points at the knob that raises the bound'


def test_app_start_timeout_knob_validated():
    """The knob is a real "limits" member, not something the parser drops.

    Asserted against an application that is actually in the configuration: a
    PUT to /config/applications/<name>/limits on an app that was never applied
    is rejected with "Value doesn't exist.", which would satisfy an assertion
    that only looks for an error.  /bin/true announces nothing either, but it
    exits at once, and a start that fails by the worker dying is the
    pre-existing path -- it needs no deadline and leaves nothing behind.
    """

    conf = {
        'listeners': {'*:8080': {'pass': 'routes'}},
        'routes': [{'action': {'return': 200}}],
        'applications': {
            'stuck': {
                'type': 'external',
                'executable': '/bin/true',
                'processes': {'spare': 0},
            }
        },
    }

    assert 'success' in client.conf(conf), 'application configured'

    path = 'applications/stuck/limits'

    resp = client.conf('{"start_timeout": "soon"}', path)

    assert 'error' in resp, 'a non-integer start_timeout is rejected'

    # Rejected for the right reason.  A build without the knob rejects this
    # with "Unknown parameter", and an application that is not in the
    # configuration is rejected with "Value doesn't exist." -- either would
    # satisfy the assertion above for nothing.
    detail = resp.get('detail', '')

    assert 'Unknown parameter' not in detail, resp
    assert "doesn't exist" not in detail, resp
    assert 'integer' in detail, resp

    # And 0 -- the default, meaning unbounded -- is accepted and survives.
    assert 'success' in client.conf(
        '{"start_timeout": 0}', path
    ), 'zero accepted'

    assert client.conf_get(f'{path}/start_timeout') == 0, 'zero round-trips'


def test_app_start_timeout_range_validated():
    """The knob is bounded by what a 32-bit millisecond clock can express.

    The value reaches the router through NXT_CONF_MAP_MSEC, which computes
    (nxt_msec_t) seconds * 1000 and checks nothing.  Above
    NXT_INT32_T_MAX / 1000 the product lands past the sign bit, where
    nxt_msec_diff() reads the deadline as already past -- so a configuration
    asking for ~24.9 days would instead have fired immediately, silently
    turning a bound meant to be generous into one that fails every start.
    Negative seconds are an out-of-range conversion to an unsigned type.
    """

    conf = {
        'listeners': {'*:8080': {'pass': 'routes'}},
        'routes': [{'action': {'return': 200}}],
        'applications': {
            'stuck': {
                'type': 'external',
                'executable': '/bin/true',
                'processes': {'spare': 0},
            }
        },
    }

    assert 'success' in client.conf(conf), 'application configured'

    path = 'applications/stuck/limits'

    # Negative.
    resp = client.conf('{"start_timeout": -1}', path)

    assert 'error' in resp, f'a negative start_timeout is rejected, got {resp}'

    detail = resp.get('detail', '')

    assert 'start_timeout' in detail, resp
    assert 'negative' in detail, resp

    # Above NXT_INT32_T_MAX / 1000 == 2147483.
    resp = client.conf('{"start_timeout": 2147484}', path)

    assert 'error' in resp, f'an out-of-range start_timeout is rejected, {resp}'

    detail = resp.get('detail', '')

    assert 'start_timeout' in detail, resp
    assert '2147483' in detail, resp

    # And the bound itself is not off by one: the largest value the clock can
    # express is still accepted.
    assert 'success' in client.conf(
        '{"start_timeout": 2147483}', path
    ), 'the maximum is accepted'

    assert client.conf_get(f'{path}/start_timeout') == 2147483


def test_app_start_timeout_retry_leaks_nothing():
    """Rejecting the same configuration N times must cost N times nothing.

    The deadline fails the PUT, and nxt_router_conf_error() discards the
    temporary configuration -- with the nxt_app_t that
    app->unaccounted_processes lives on.  So there is no counter left to bound
    the config-apply path with, and each retry forks a fresh prototype and a
    fresh worker: ordinary control-plane retry automation exhausts host pids.

    What bounds it is not accounting but draining.  Discarding the application
    sends the prototype a QUIT, and the prototype is then able to act on it,
    because nxt_proto_quit_children() signals a child that has never announced
    itself instead of writing a port message nothing there reads.  Before
    that, both processes survived every retry -- and unitd's own exit.
    """

    retries = 5

    assert 'success' in client.conf(_serving_conf()), 'baseline configured'

    for i in range(retries):
        assert 'error' in client.conf(_stuck_conf()), f'PUT {i} rejected'

    workers, protos = _wait_gone()

    assert workers == [], (
        f'{len(workers)} silent workers survive {retries} rejected PUTs; '
        'each retry is leaking the process it forked'
    )

    assert protos == [], (
        f'{len(protos)} prototypes survive {retries} rejected PUTs; a '
        'prototype cannot exit while a child of it is alive'
    )

    # And none of it cost the control plane, which is the point of the bound.
    assert 'connections' in client.conf_get('/status')

    assert client.conf_get() == _serving_conf(), 'previous config retained'


def test_app_start_timeout_default_is_unbounded(findall):
    """With no "limits", a worker that is merely slow must still be waited for.

    NXT_APP_START_TIMEOUT is 0, so nothing is armed at all: the start is as
    unbounded as it was before this change.  Exercised with a worker that
    really does take its time before announcing itself -- the libunit sample
    app behind a `sleep`, which is the shape of every module's startup (user
    code first, nxt_unit_init() after) -- so a default bound short enough to
    matter would show up here as a failed PUT rather than as a served request.
    """

    delay = 4

    conf = {
        'listeners': {'*:8080': {'pass': 'applications/slow'}},
        'applications': {
            'slow': {
                'type': 'external',
                'working_directory': option.temp_dir,
                # `sh -c 'sleep N; exec APP' -` announces only after the sleep;
                # exec keeps the app in the process the prototype forked, and
                # NXT_UNIT_INIT is inherited across it.
                'executable': '/bin/sh',
                'arguments': [
                    '-c',
                    f'sleep {delay}; exec "$0"',
                    f'{option.current_dir}/build/unit_app_test',
                ],
            }
        },
    }

    started = time.monotonic()

    assert 'success' in client.conf(conf), 'a slow start is still waited for'

    elapsed = time.monotonic() - started

    assert elapsed >= delay, f'the worker announced early ({elapsed:.1f}s)'

    assert client.get(url='/')['status'] == 200, 'the slow app serves'

    assert findall(r'did not become ready in time') == [], (
        'no deadline fired without an explicit start_timeout'
    )


def _stuck_on_demand_conf(max_processes, start_timeout=START_TIMEOUT):
    """The same silent worker, started by a request rather than by the PUT.

    "spare": 0 is what moves the start off the configuration-apply path: with
    nothing to prefork the PUT returns at once, and the first request is what
    asks for a process.  That is the shape in which the deadline can fire more
    than once for one application, which is what these tests are about.
    """

    conf = _stuck_conf(start_timeout)
    conf['applications']['stuck']['processes'] = {
        'max': max_processes,
        'spare': 0,
    }

    return conf


def _app_processes(name='stuck'):
    return client.conf_get(f'/status/applications/{name}/processes')


def test_app_start_timeout_bounds_orphans():
    """"processes": {"max"} must bound the processes the deadline gives up on.

    The deadline fails the start, and the worker it gave up on keeps running:
    the router has no pid for it and cannot kill it, and its application is
    live, so nothing quits the prototype that could.  Such a worker is in
    neither app->processes, which only PROCESS_READY fills, nor
    app->pending_processes, which the failure gives back -- so unless the
    router counts it somewhere, every request forks another one and "max"
    bounds nothing.  Five requests against "max": 2 produced five `sleep`
    processes.

    app->unaccounted_processes is that somewhere.  Past the bound the
    application stops forking, and a request that arrives then is answered
    rather than parked: only a start can take a request back out of
    ack_waiting_req, and no start is coming.
    """

    max_processes = 2
    requests = max_processes + 3

    assert 'success' in client.conf(
        _stuck_on_demand_conf(max_processes)
    ), 'the on-demand config applies at once'

    assert _stuck_pids() == [], 'nothing forked before the first request'

    for i in range(requests):
        started = time.monotonic()

        assert client.get(url='/')['status'] == 503, f'request {i} failed'

        elapsed = time.monotonic() - started

        if i < max_processes:
            # A start really was attempted, so the deadline is what answered.
            assert elapsed >= START_TIMEOUT - 1, (
                f'request {i} was failed before the deadline ({elapsed:.1f}s)'
            )

        else:
            # Nothing left to start: the request must not wait for a process
            # that will never be asked for, nor sit in ack_waiting_req until
            # it times out.
            assert elapsed < START_TIMEOUT, (
                f'request {i} waited {elapsed:.1f}s for a start that cannot '
                'happen'
            )

    pids = _stuck_pids()

    assert len(pids) == max_processes, (
        f'{len(pids)} silent workers survive "max": {max_processes}; the '
        'deadline is forking one per request again'
    )

    procs = _app_processes()

    assert procs['unaccounted'] == max_processes, procs
    assert procs['running'] == 0 and procs['starting'] == 0, procs


def test_app_start_timeout_orphan_death_frees_the_slot():
    """The bound is a bound, not a one-way ratchet.

    The router does hear about the process it gave up on, once: the prototype
    that forked it reaps it and notifies with the start's own stream still
    attached, and nxt_router_remove_pid_handler() retypes that REMOVE_PID into
    an RPC error on the handler the expiry left registered.  So a slot comes
    back when the process behind it dies, and the application can start again.
    """

    assert 'success' in client.conf(_stuck_on_demand_conf(1))

    assert client.get(url='/')['status'] == 503, 'the start is bounded'

    pids = _stuck_pids()

    assert len(pids) == 1, pids
    assert _app_processes()['unaccounted'] == 1

    _reap_stuck()

    for _ in range(50):
        if _app_processes()['unaccounted'] == 0:
            break

        time.sleep(0.1)

    procs = _app_processes()

    assert procs['unaccounted'] == 0, (
        f'the dead process still holds its slot: {procs}'
    )

    # And the slot is usable again: this forks a second silent worker.
    assert client.get(url='/')['status'] == 503, 'a start is possible again'

    assert len(_stuck_pids()) == 1, 'the freed slot started a process'


def test_app_start_timeout_slow_worker_adopted():
    """A worker that only just missed the deadline is kept, not thrown away.

    It announces itself on the stream the expiry left registered, and the
    router moves its slot from unaccounted_processes to processes and adopts
    the port.  Discarding it instead would be the worse trade by far: the next
    request would fork another worker that is equally slow, be failed by the
    same deadline, and the application would never serve anything.
    """

    delay = START_TIMEOUT + 3

    conf = {
        'listeners': {'*:8080': {'pass': 'applications/slow'}},
        'applications': {
            'slow': {
                'type': 'external',
                'working_directory': option.temp_dir,
                'processes': {'max': 1, 'spare': 0},
                'limits': {'start_timeout': START_TIMEOUT},
                'executable': '/bin/sh',
                'arguments': [
                    '-c',
                    f'sleep {delay}; exec "$0"',
                    f'{option.current_dir}/build/unit_app_test',
                ],
            }
        },
    }

    assert 'success' in client.conf(conf), 'the slow app is configured'

    # The first request pays for the deadline: the worker is still sleeping.
    assert client.get(url='/')['status'] == 503, 'the deadline fired'

    assert _app_processes('slow')['unaccounted'] == 1, _app_processes('slow')

    # It announces itself a few seconds later, and is taken on.
    for _ in range(100):
        if _app_processes('slow')['running'] == 1:
            break

        time.sleep(0.2)

    assert _app_processes('slow') == {
        'running': 1,
        'starting': 0,
        'unaccounted': 0,
        'idle': 1,
    }

    assert client.get(url='/')['status'] == 200, 'the adopted worker serves'


def test_app_start_timeout_slow_worker_survives_a_quit(findall):
    """A worker that is merely slow must ride out a quit, not be signalled.

    The prototype's deadline is for a worker that will never read the QUIT it
    is sent (nxt_proto_quit_children()), and until one announces itself that
    worker is indistinguishable from one that is simply still starting.  The
    deadline is how they are told apart: a slow one reads the QUIT out of its
    port the moment nxt_unit_init() starts reading, which is well inside the
    second "start_timeout" it is given here.

    A restart is the shape that catches it, and is what caught it: serving a
    request replenishes the spare, so the application has a worker in the
    middle of its start exactly when the old prototype is told to quit.
    """

    delay = 2

    conf = {
        'listeners': {'*:8080': {'pass': 'applications/slow'}},
        'applications': {
            'slow': {
                'type': 'external',
                'working_directory': option.temp_dir,
                'processes': {'max': 2, 'spare': 1, 'idle_timeout': 60},
                # Comfortably longer than the start below, so nothing here
                # turns on a race between the two.
                'limits': {'start_timeout': delay * 10},
                'executable': '/bin/sh',
                'arguments': [
                    '-c',
                    f'sleep {delay}; exec "$0"',
                    f'{option.current_dir}/build/unit_app_test',
                ],
            }
        },
    }

    assert 'success' in client.conf(conf), 'the slow app is configured'

    assert client.get(url='/')['status'] == 200, 'the slow app serves'

    # Serving that request is what asks for the second process, and it is
    # still sleeping when the restart below quits the prototype that forked
    # it.  Nothing waits for it: waiting is what this test must not do.
    assert 'success' in client.conf_get('/control/applications/slow/restart')

    # Long enough for both starts to finish and for a deadline that should
    # not be armed at all to have fired if it were.
    time.sleep(delay * 3)

    assert findall(r'killing it') == [], 'a slow worker was signalled'

    assert findall(r'exited on signal') == [], 'a slow worker was killed'

    assert client.get(url='/')['status'] == 200, 'the restarted app serves'


def test_app_start_timeout_reconfigure_collects_orphans():
    """Replacing the application must collect what its deadlines gave up on.

    This is the recovery the bound relies on: a worker wedged for good holds
    its "processes" slot for the life of the application, and a
    reconfiguration is what ends that life.  It only works if the processes
    actually die.  They are children of the prototype, which is the one
    process that knows their pids, and nxt_router_free_app() sends that
    prototype a QUIT -- but a QUIT it cannot act on left the prototype waiting
    on children that answer nothing, so the whole family outlived unitd.
    """

    max_processes = 2

    assert 'success' in client.conf(_stuck_on_demand_conf(max_processes))

    for i in range(max_processes):
        assert client.get(url='/')['status'] == 503, f'request {i} failed'

    assert len(_stuck_pids()) == max_processes, 'the workers are there'
    assert len(_stuck_protos()) == 1, 'and the prototype that forked them'

    assert _app_processes()['unaccounted'] == max_processes

    assert 'success' in client.conf(_serving_conf()), 'the app is replaced'

    workers, protos = _wait_gone()

    assert workers == [], f'{len(workers)} workers survive the reconfigure'
    assert protos == [], f'{len(protos)} prototypes survive the reconfigure'

    assert client.get(url='/')['status'] == 200, 'the new config serves'
