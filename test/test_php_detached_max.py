"""processes.max must bound live workers, detached post-response work included.

fastcgi_finish_request() reports the request done while the script keeps
running.  The router takes that as the response, so the worker is counted
idle and becomes reapable while it is in fact still executing PHP.

These tests assert the property that follows from "max" being a cap on
processes rather than on requests: however the router accounts for a worker
that is finishing detached work, the number of live PHP children of the
prototype must never exceed "max".
"""

import os
import subprocess
import threading
import time

from unit.applications.lang.php import ApplicationPHP
from unit.control import Control
from unit.option import option

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()

IDLE_TIMEOUT = 2

# Comfortably past IDLE_TIMEOUT, so "the reaper did not act" is unambiguous,
# and short enough that every test can wait for the work to finish before it
# ends.  Draining matters now: a detached worker is deliberately not reaped,
# so one left running would be counted by the next test and its port
# descriptors flagged as a leak by conftest.
SLEEP_SECONDS = 6


def worker_pids(name='detached_worker'):
    """Live worker pids, as the children of the application's prototype.

    Same derivation as test_app_lifecycle.py: the prototype's title survives
    for every runtime, and its children are the workers.  A worker that has
    been sent QUIT but has not exited is still a child, which is the whole
    point here -- it is still a live PHP process holding memory and a slot.
    """

    output = subprocess.check_output(
        ['ps', 'ax', '-o', 'pid=', '-o', 'ppid=', '-o', 'args=']
    ).decode()

    rows = [p for p in (l.split(None, 2) for l in output.splitlines())
            if len(p) == 3]

    marker = f'unit: "{name}" prototype'
    prototypes = {pid for pid, _, args in rows if marker in args}

    if not prototypes:
        return set()

    return {pid for pid, ppid, _ in rows if ppid in prototypes}


def app_processes(name='detached_worker'):
    """The application's "processes" object from /status."""

    status = Control().conf_get('/status')
    apps = status.get('applications', {})

    return apps.get(name, {}).get('processes', {})


def drain(timeout=30):
    """Wait for detached work to finish and the workers to settle.

    Polls the router's own view rather than sleeping a fixed time: the
    "detached" count is the thing under test, so waiting on it also asserts
    it reaches zero on every ordinary path.
    """

    deadline = time.time() + timeout

    while time.time() < deadline:
        if app_processes().get('detached', 0) == 0:
            return True

        time.sleep(0.2)

    return False


def marker(name):
    """An absolute path under the test temp dir, removed if it exists."""

    path = f'{option.temp_dir}/{name}'

    if os.path.exists(path):
        os.remove(path)

    return path


def test_php_detached_response_overlaps_the_script():
    """The response really does arrive while the script is still running.

    Without this the other tests could pass against a build where
    fastcgi_finish_request() did nothing, because every request would simply
    be serialised.
    """

    client.load(
        'detached_worker',
        processes={'max': 1, 'spare': 0, 'idle_timeout': IDLE_TIMEOUT},
    )

    start = time.time()
    assert client.get(url=f'/?sleep={SLEEP_SECONDS}')['status'] == 200
    elapsed = time.time() - start

    assert elapsed < SLEEP_SECONDS / 2, (
        f'the response took {elapsed:.1f}s, so it waited for the script '
        f'rather than overlapping it'
    )

    assert len(worker_pids()) == 1, 'one worker serves the detached request'

    assert drain(), 'the detached work finished and the count cleared'


def test_php_detached_work_does_not_exceed_max():
    """A parked request must not add a second live PHP child under max: 1.

    The first script detaches and sleeps well past idle_timeout.  A second
    request arrives while it runs, so it parks.  Whatever the router does
    about that parked request, "max": 1 forbids a second live worker while
    the first is still executing.
    """

    client.load(
        'detached_worker',
        processes={'max': 1, 'spare': 0, 'idle_timeout': IDLE_TIMEOUT},
    )

    assert client.get(url=f'/?sleep={SLEEP_SECONDS}')['status'] == 200

    first = worker_pids()
    assert len(first) == 1, 'one worker after the detached request'

    # Park a second request against the still-busy worker.  It is issued from
    # a thread because it will not be answered until capacity exists.
    threading.Thread(
        target=lambda: client.get(read_timeout=SLEEP_SECONDS + 5),
        daemon=True,
    ).start()

    # Past idle_start + idle_timeout, where the reaper acts on the worker it
    # believes is idle.
    deadline = time.time() + IDLE_TIMEOUT * 3 + 2
    peak = set()

    while time.time() < deadline:
        peak |= worker_pids()

        assert len(worker_pids()) <= 1, (
            f'{len(worker_pids())} live PHP children under "max": 1 -- '
            f'a worker running detached work was replaced rather than '
            f'counted'
        )

        time.sleep(0.2)

    assert first <= peak, 'the original worker stayed alive throughout'

    assert drain(), 'the detached work finished and the count cleared'


def test_php_detached_completion_releases_the_worker():
    """When the detached work ends, the worker rejoins the idle economy.

    The parked request must then be served -- and served without the router
    having started a second process while the first was still running.
    """

    client.load(
        'detached_worker',
        processes={'max': 1, 'spare': 0, 'idle_timeout': IDLE_TIMEOUT},
    )

    done = marker('detached_done')
    short = 6

    assert client.get(url=f'/?sleep={short}&done={done}')['status'] == 200

    served = {}

    def parked():
        served['resp'] = client.get(read_timeout=short + 15)

    thread = threading.Thread(target=parked, daemon=True)
    thread.start()

    # While the detached work runs the worker is neither idle nor reapable.
    time.sleep(IDLE_TIMEOUT * 2)

    procs = app_processes()
    assert procs.get('detached', 0) == 1, (
        f'the worker is not reported detached: {procs}'
    )
    assert procs.get('idle', 0) == 0, f'a busy worker is reported idle: {procs}'
    assert len(worker_pids()) == 1, 'still one live worker'

    thread.join(timeout=short + 20)

    assert served.get('resp', {}).get('status') == 200, (
        'the parked request was served once capacity existed'
    )
    assert os.path.exists(done), 'the detached work ran to completion'

    # The detached state clears once the script returns.
    deadline = time.time() + 10
    while time.time() < deadline and app_processes().get('detached', 0) != 0:
        time.sleep(0.2)

    assert app_processes().get('detached', 0) == 0, 'detached count cleared'


def test_php_detached_parked_request_times_out():
    """limits.timeout bounds the wait for capacity, and the request never runs.

    The second request cannot be served until the detached work ends, so a
    timeout below the detached duration must answer 503 -- and the script for
    that request must never execute, before or after the first one returns.
    """

    ran = marker('timed_out_ran')
    timeout = 3

    client.load(
        'detached_worker',
        processes={'max': 1, 'spare': 0, 'idle_timeout': IDLE_TIMEOUT},
        limits={'timeout': timeout},
    )

    assert client.get(url=f'/?sleep={SLEEP_SECONDS}')['status'] == 200

    start = time.time()
    resp = client.get(url=f'/?ran={ran}', read_timeout=timeout + 10)
    elapsed = time.time() - start

    assert resp['status'] == 503, f'parked request timed out, got {resp}'
    assert elapsed < timeout + 5, f'answered at the deadline, took {elapsed:.1f}s'
    assert not os.path.exists(ran), 'the timed-out request never executed'

    # And it must still not run once the detached worker becomes free.
    time.sleep(3)
    assert not os.path.exists(ran), (
        'the timed-out request executed after the worker freed up'
    )

    assert drain(), 'the detached work finished and the count cleared'


def test_php_detached_start_after_the_worker_went_idle():
    """A detached start for a worker the router has already parked as idle.

    The start edge is sent before the last response message, but that only
    orders the two on the wire.  A request that is failed rather than
    answered releases its worker with no start edge in sight at all:
    "limits": {"timeout"} answers 503 while the script is still running, and
    the release puts the port in idle_ports.  The start edge then arrives for
    a port that is already there.

    Unless it is taken back out, the reaper QUITs a process that is still
    executing PHP -- the exact failure this feature exists to prevent -- and
    clears the port's application on the way, after which nothing settles the
    detached state: nxt_port_close() skips nxt_router_app_port_close() for a
    port with no application, the finish edge is ignored for the same reason,
    and the count sticks for the life of the application.
    """

    timeout = 2
    idle_timeout = 6

    # Answer after the deadline has failed the request, and well before the
    # reaper's own deadline, so the start edge really does land on a port
    # sitting in idle_ports.
    before = timeout + 2

    # And keep running past the reap, so a reaped worker is one that was
    # killed mid-script.
    after = idle_timeout + 4

    client.load(
        'detached_worker',
        processes={'max': 1, 'spare': 0, 'idle_timeout': idle_timeout},
        limits={'timeout': timeout},
    )

    done = marker('detached_late_start')

    start = time.time()
    resp = client.get(
        url=f'/?before={before}&sleep={after}&done={done}',
        read_timeout=before + 10,
    )

    assert resp['status'] == 503, f'the request outlived limits.timeout: {resp}'

    first = worker_pids()
    assert len(first) == 1, 'one worker is running the script'

    # After the start edge, before the reaper's deadline.
    while time.time() - start < before + 2:
        time.sleep(0.2)

    procs = app_processes()

    assert procs.get('detached', 0) == 1, (
        f'the worker did not report itself detached: {procs}'
    )
    assert procs.get('idle', 0) == 0, (
        f'a worker running detached work is still counted idle and is '
        f'reapable: {procs}'
    )

    # Past the reaper's deadline, with the script still running.
    while time.time() - start < before + idle_timeout + 2:
        assert len(worker_pids()) <= 1, '"max": 1 still bounds live workers'
        time.sleep(0.2)

    assert worker_pids() == first, 'the worker was not replaced'

    procs = app_processes()
    assert procs.get('running', 0) == 1, (
        f'the worker was reaped while it was still running: {procs}'
    )

    assert drain(timeout=after + 20), (
        'the detached state cleared once the script returned'
    )
    assert os.path.exists(done), 'the detached work ran to completion'


def test_php_detached_repeated_requests_hold_the_bound():
    """Repeated detached requests must not accumulate live workers.

    This is the shape that turns a brief overshoot into an unbounded one: if
    each reap starts a replacement while the previous worker is still running
    PHP, one extra child appears per idle_timeout for as long as the scripts
    run.  Spans several intervals for that reason.
    """

    client.load(
        'detached_worker',
        processes={'max': 1, 'spare': 0, 'idle_timeout': IDLE_TIMEOUT},
    )

    peak = 0
    rounds = 6

    for _ in range(rounds):
        client.get(url=f'/?sleep={SLEEP_SECONDS}', read_timeout=10)

        for _ in range(int(IDLE_TIMEOUT * 5)):
            live = len(worker_pids())
            peak = max(peak, live)

            assert live <= 1, (
                f'{live} live PHP children under "max": 1 after repeated '
                f'detached requests'
            )

            time.sleep(0.2)

    assert peak == 1, f'the application kept exactly one worker, saw {peak}'

    assert drain(), 'the detached work finished and the count cleared'


def test_php_detached_exit_and_fatal_still_report_finished():
    """exit() and a fatal error after the response still end the detached state.

    libunit reports the work finished when the request handler returns, and
    PHP reaches that return through its bailout as well as through an
    ordinary return.  If it did not, the worker would stay pinned out of the
    idle economy for good -- a worse failure than the one this feature
    exists to fix, because nothing would ever clear it.
    """

    for after in ('exit', 'fatal'):
        client.load(
            'detached_worker',
            processes={'max': 1, 'spare': 0, 'idle_timeout': IDLE_TIMEOUT},
        )

        done = marker(f'detached_{after}')

        assert (
            client.get(url=f'/?sleep=2&done={done}&after={after}')['status']
            == 200
        ), f'{after}: the response went out'

        assert drain(), f'{after}: the detached state cleared'
        assert os.path.exists(done), f'{after}: the detached work ran'

        # And the application still works afterwards.
        assert client.get(url='/')['status'] == 200, f'{after}: still serving'

        assert drain(), f'{after}: cleared again'


def test_php_detached_survives_configuration_removal():
    """Removing the application while a worker runs detached work is safe.

    A detached worker holds no request, and a request is what otherwise
    keeps the application alive, so the detached state has to hold its own
    reference.  Without it the application is freed under a running process.
    The assertion the router itself makes is in nxt_router_free_app(), which
    is a debug build; here the observable is that the control plane survives
    and the work still completes.
    """

    client.load(
        'detached_worker',
        processes={'max': 1, 'spare': 0, 'idle_timeout': IDLE_TIMEOUT},
    )

    done = marker('detached_removed')

    assert client.get(url=f'/?sleep=4&done={done}')['status'] == 200

    # Remove the whole configuration while the script is still running.  The
    # application cannot be deleted on its own while a listener still routes
    # to it, so the listener goes with it.
    assert 'success' in client.conf(
        {'listeners': {}, 'applications': {}}, '/config'
    )

    # The control plane is still answering, and the router did not die with it.
    assert 'applications' in Control().conf_get('/status')

    deadline = time.time() + 20
    while time.time() < deadline and not os.path.exists(done):
        time.sleep(0.2)

    assert os.path.exists(done), 'the detached work finished after removal'
