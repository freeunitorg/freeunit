"""Abrupt application death while large requests are in flight.

What this drives: router worker engines on several event engines
(``settings/listen_threads``) moving 1 MiB request and response bodies through
an application's shared memory, while the test repeatedly ``kill -9``s the one
process serving that application.  The application is configured with a single
process, so every kill leaves the router with no worker for the application and
forces a respawn, and every kill lands on a process that is mid-transfer.

Why that matters here: this is the *abrupt* teardown path, and it is a
different one from the graceful, configuration-driven teardown that
test_process_teardown_churn.py drives.  A killed worker is noticed by the main
process' SIGCHLD handler, which broadcasts NXT_PORT_MSG_REMOVE_PID; the router
handles it in ``nxt_router_remove_pid_handler()`` -> ``nxt_port_remove_pid()``
(src/nxt_port.c).  That function was converted from ``nxt_runtime_process_find()``
to ``nxt_runtime_process_ref()``, and it is the site where review found a double
free: it hands its result to ``nxt_process_close_ports()``, which takes its own
+1/-1 around the port loop, so on a process a router worker had already dropped
to zero that pair is a second 0 -> 1 -> 0 transition and so a second teardown,
racing the one already posted.  Nothing else in the suite reaches that site with
shared-memory traffic in flight: test_respawn.py kills an idle worker, and the
churn test never kills anything.

What this proves and what it does not: a green run is *weak* evidence for the
absence of the double free.  The window is a handful of instructions wide and
an uninstrumented build will usually miss it.  The value of this test is as a
crash and ASan/UBSan *driver* -- run under ``.github/workflows/sanitize.yml``
instrumentation, a second teardown of the same process becomes a hard failure.
Read a pass as "workers died hard under load, the router noticed each one, and
the daemon kept serving correctly", nothing stronger.

Correctness assertions are written so that a request legitimately failed by a
kill is not a failure, but a corrupt or hung one is.  About two seconds of
workload: six kills, at least 0.3 s apart, against four request threads.  Each
round waits for a worker the run has not killed before, and then for the signal
to have actually removed it, so all six are distinct abrupt teardowns rather
than repeat shots at one corpse.
"""

import subprocess
import threading
import time

import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.log import Log
from unit.shm_body import describe_mismatch, make_body

prerequisites = {'modules': {'php': 'all'}}

client = ApplicationPHP()

APP = 'mirror'

# Same reasoning as test_process_teardown_churn.py: 1 MiB spans 64 chunks of
# shared memory in each direction (PORT_MMAP_CHUNK_SIZE is 16 KiB,
# src/nxt_port_memory_int.h), so a request is inside the chunk allocator, and
# the process/port lookups that reach it, for long enough that a kill on
# another thread lands in the middle of one.
#
# The bytes are not uniform: every request carries a fresh body stamped, every
# 64 bytes, with that request's tag and the byte offset of the record (see
# unit/shm_body.py).  WORKERS requests are in shared memory at once here, so a
# chunk recycled out from under one of them most likely holds another one's
# payload; against 'x' * BODY_SIZE that chunk is byte-identical to what
# belongs there and the check below cannot see it.
BODY_SIZE = 1024 * 1024

# > 1 so more than one router worker engine is live; this is a top-level
# "settings" member (src/nxt_conf_validation.c: nxt_conf_vldt_setting_members).
LISTEN_THREADS = 4

WORKERS = 4
KILLS = 6
KILL_INTERVAL = 0.3

# Not the suite default (60s), so that a stalled request returns instead of
# calling pytest.fail() on a helper thread; the stall is detected by timing it
# and reported as a hard failure below.
READ_TIMEOUT = 10


def _app_pids():
    """PIDs currently serving the application, via ps (as conftest does)."""
    out = subprocess.check_output(
        ['ps', '-ax', '-o', 'pid', '-o', 'command']
    ).decode()

    marker = f'unit: "{APP}" application'

    return {line.split()[0] for line in out.splitlines() if marker in line}


def _wait_for_fresh_app_pids(spent, timeout=5):
    """Wait for a worker that is not one already killed, so every kill in the
    loop below lands on a live process instead of on the gap left by the
    previous one -- or, worse, on the corpse of it.

    Excluding `spent` rather than just waiting for a non-empty set is what
    makes each iteration a distinct abrupt teardown.  A SIGKILLed worker
    leaves `_app_pids()` promptly, because the marker is the setproctitle
    string in /proc/<pid>/cmdline and a zombie has no cmdline, so ps falls
    back to "[comm] <defunct>" -- but "promptly" is a scheduling property,
    and this test exists to be run on loaded sanitizer machines.  Making the
    loop wait for a *new* pid turns that timing assumption into something the
    test checks."""

    deadline = time.time() + timeout

    while time.time() < deadline:
        pids = _app_pids() - spent

        if pids:
            return pids

        time.sleep(0.05)

    return set()


def _wait_for_pids_gone(pids, timeout=5):
    """Wait until none of `pids` serves the application any more, i.e. until
    the SIGKILL has actually taken a worker away rather than merely being
    queued on one."""

    deadline = time.time() + timeout

    while time.time() < deadline:
        if not (_app_pids() & pids):
            return True

        time.sleep(0.05)

    return False


def test_process_abrupt_teardown(skip_alert, skip_fds_check):
    # Killing workers leaves descriptor accounting unsettled in all three
    # long-lived processes for as long as the respawn takes, which is not what
    # this test is about.
    skip_fds_check(main=True, router=True, controller=True)

    # Expected, and in fact asserted on below: this is how the main process
    # reports each kill.
    skip_alert(r'app process \d+ exited on signal 9')

    # Exactly one process, so a kill always removes the only worker and the
    # router has to tear the process down rather than route around it, and so
    # every request thread contends on the same incoming.mutex.
    client.load(APP, processes={"spare": 1, "max": 1, "idle_timeout": 30})

    assert 'success' in client.conf(
        {"listen_threads": LISTEN_THREADS}, 'settings'
    ), 'listen_threads'

    assert client.post(body='ping', read_timeout=READ_TIMEOUT)['status'] == 200

    stop = threading.Event()
    lock = threading.Lock()
    failures = []
    outcomes = []
    killed = []

    def record(bucket, item):
        with lock:
            bucket.append(item)

    def request_worker():
        while not stop.is_set():
            expected = make_body(BODY_SIZE)
            started = time.time()

            try:
                resp = client.post(
                    body=expected,
                    read_timeout=READ_TIMEOUT,
                    read_buffer_size=65536,
                )

            except KeyboardInterrupt:
                raise

            # pytest.fail() inside the helpers raises BaseException
            except BaseException as exc:  # noqa: BLE001
                # The listener stays open across a worker death, so a refused
                # or reset connection is still not expected; record it and let
                # the assertions below decide.
                record(failures, f'request raised {exc!r}')
                continue

            elapsed = time.time() - started
            status = resp.get('status')
            body = resp.get('body', '')

            if status == 200:
                # A short body is legitimate: the worker can die after its
                # headers are on the wire.  Wrong *bytes* are not - that is
                # what reading freed or recycled shared memory looks like.
                if body != expected[: len(body)]:
                    # describe_mismatch() reports the first wrong byte and the
                    # record stamp found there, so a failure says whether the
                    # bytes came from another offset of this request or from
                    # another request entirely.  It keeps the fail-open guard
                    # too: the overlong-but-correct-prefix shape has no
                    # mismatching byte, and a bare next() would raise
                    # StopIteration here, outside the try that guards the
                    # request, killing this thread instead of recording the
                    # corruption.
                    record(
                        failures,
                        f'corrupt body: {len(body)} bytes, '
                        + describe_mismatch(body, expected),
                    )
                    continue

                record(outcomes, 200 if len(body) == BODY_SIZE else 'truncated')

            elif status is None:
                # No parsable response: connection closed as the worker died.
                # Distinguish that from a hang, which is a real defect.
                if elapsed >= READ_TIMEOUT:
                    record(failures, f'no response in {elapsed:.1f}s')
                else:
                    record(outcomes, 'closed')

            elif status in (500, 503):
                record(outcomes, status)

            else:
                record(failures, f'unexpected status {status}')

    workers = [threading.Thread(target=request_worker) for _ in range(WORKERS)]

    for worker in workers:
        worker.start()

    for round_ in range(KILLS):
        pids = _wait_for_fresh_app_pids(set(killed))

        if not pids:
            record(
                failures, f'no fresh application worker for kill {round_ + 1}'
            )
            break

        subprocess.call(['kill', '-9', *pids])

        # Only count the round once the signal has demonstrably removed those
        # workers.  Otherwise a kill that has not landed yet would leave the
        # next iteration looking at the same process, and the run would lose
        # abrupt-teardown coverage without losing a single assertion.
        if not _wait_for_pids_gone(pids):
            record(failures, f'kill -9 did not remove {sorted(pids)}')
            break

        killed.extend(pids)

        time.sleep(KILL_INTERVAL)

    stop.set()

    for worker in workers:
        worker.join(timeout=120)
        assert not worker.is_alive(), 'request thread hung'

    assert not failures, f'{len(failures)} failure(s), first: {failures[0]}'

    # The daemon is still alive and still serves the whole payload correctly.
    expected = make_body(BODY_SIZE)

    resp = client.post(
        body=expected, read_timeout=READ_TIMEOUT, read_buffer_size=65536
    )

    assert resp['status'] == 200, 'still serving'
    assert len(resp['body']) == BODY_SIZE, 'still serving whole body'
    assert resp['body'] == expected, 'still serving intact body'

    # Requests must not have been shut out entirely, or nothing was in flight
    # when a worker died.
    assert outcomes.count(200) > 0, f'no request completed: {outcomes[:20]}'

    # Every round killed a worker that had never been killed before, and the
    # loop ran all KILLS of them -- so the router really did tear a process
    # down and build a new one, repeatedly, and not two or three times with
    # the rest of the rounds shooting at the same corpse.  The loop enforces
    # the first property; assert it anyway, since it is the coverage claim.
    assert len(set(killed)) == len(killed), (
        f'a worker was killed twice: {killed}'
    )
    assert len(killed) >= KILLS, (
        f'only {len(killed)} worker(s) killed: {killed}'
    )

    log = Log.read()

    # Available at any log level - this is the alert the main process emits
    # from its SIGCHLD handler, and the same handler is what broadcasts
    # NXT_PORT_MSG_REMOVE_PID to the router.  One per kill.
    for pid in set(killed):
        assert f'app process {pid} exited on signal 9' in log, (
            f'main process did not report the death of {pid}'
        )

    # The direct evidence - "port remove pid <pid> handler" is logged by
    # nxt_port_remove_pid() itself - is nxt_debug(), compiled out unless Unit
    # was configured with --debug.  Skip loudly rather than let the test
    # degrade into a smoke test that never checks the site it names.
    if '[debug]' not in log:
        pytest.skip('remove_pid evidence needs a --debug build')

    for pid in set(killed):
        assert f'port remove pid {pid} handler' in log, (
            f'nxt_port_remove_pid() did not run for {pid}'
        )

    Log.check_alerts()
