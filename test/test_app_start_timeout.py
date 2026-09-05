"""A worker that starts but never announces itself must not strand the start.

An application process is only ever announced by the process itself, from
nxt_unit_init() -> PROCESS_READY.  A `"type": "external"` binary that never
gets that far -- /bin/sleep is the shortest example, but a wrapper script or a
runtime blocked in its own init reaches the same state -- answers nothing, and
because it stays alive nothing kills it either, so no REMOVE_PID converts the
wait into an error.

Without a "limits": {"start_timeout"} that leaves the router's START_PROCESS
RPC armed for good.  With the default "processes" that RPC is the only continuation
of nxt_router_conf_apply(), so:

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
# to keep the module quick.  The product default is 30 s
# (NXT_APP_START_TIMEOUT, in milliseconds).
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


def _reap_stuck():
    for pid in _stuck_pids():
        subprocess.run(['kill', '-9', str(pid)], check=False)


@pytest.fixture(autouse=True)
def _stuck_app(skip_alert):
    """Every test here deliberately produces alerts; name them all once.

    * the deadline's own alert;
    * "failed to apply new conf", which nxt_router_conf_error() logs for the
      PUT the deadline makes fail;
    * "exited on signal", from reaping the silent worker below -- the router
      cannot kill it (see test_app_start_timeout_worker_not_reaped), so the
      test has to, and the prototype reports the death.
    """

    skip_alert(
        r'did not become ready in time',
        r'failed to apply new conf',
        r'exited on signal',
    )

    yield

    # The router never learns the silent worker's pid -- it arrives only with
    # PROCESS_READY -- so the deadline cannot kill it; see the module note in
    # test_app_start_timeout_worker_not_reaped().  Clean up regardless, so a
    # failure here does not leave sleeps behind for the next run.
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
    assert 'success' in client.conf('{"start_timeout": 0}', path), 'zero accepted'

    assert client.conf_get(f'{path}/start_timeout') == 0, 'zero round-trips'


def test_app_start_timeout_worker_not_reaped():
    """Documents the boundary of this fix.

    The router only ever learns a worker's pid from PROCESS_READY, which is
    exactly the message a silent worker does not send, so the deadline has no
    pid to kill.  Reaping the process needs the *prototype* -- which does know
    the pid -- to escalate a child that will not exit, in
    nxt_proto_sigchld_handler()/nxt_process_quit(); that is the code #268
    rewrites, so it is deliberately left out of this change.

    This test asserts the state that fix will change, so that landing it turns
    this assertion red rather than letting the gap go unnoticed.
    """

    assert 'success' in client.conf(_serving_conf()), 'baseline configured'
    assert 'error' in client.conf(_stuck_conf()), 'stuck app rejected'

    # The router recovered; the process it gave up on is still there.
    assert client.conf_get('/status')['connections'] is not None

    time.sleep(1)

    assert _stuck_pids() != [], (
        'the silent worker is expected to survive the timeout; if this fails, '
        'something now reaps it -- update this test and the note above'
    )


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
