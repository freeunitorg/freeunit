"""Router handling of an application that runs out of shared memory.

What this drives: a PHP application whose shared-memory budget
(``applications/<name>/limits/shm``) is one segment, writing a response several
times larger than that segment.  Every write goes through
``nxt_unit_response_write()``, which allocates out of the application's
outgoing segments, and those chunks stay busy until the router has written them
to the client and completed the buffers.  The client stalls before reading, so
the queued response keeps the segment's free map full while the application
still has output left; ``nxt_unit_mmap_get()`` (src/nxt_unit.c) then reports
the out-of-shared-memory condition to the router and blocks until it is
acknowledged.

Why that matters here: the OOSM message is the only thing that reaches
``nxt_router_oosm_handler()`` (src/nxt_router.c), and the ACK the application
waits for is the only thing that reaches ``nxt_process_broadcast_shm_ack()``
(src/nxt_port_memory.c), which the router sends from the ``hdr->oosm`` branch
of ``nxt_port_mmap_buf_completion()`` as it frees the chunks.  All three look
the peer process up by pid, and all three were converted from
``nxt_runtime_process_find()``, which returns a process without a reference, to
``nxt_runtime_process_ref()``.  Nothing else in this suite sends an OOSM
message, so before this test those conversions had no coverage at all.

Which engine runs what: ``nxt_unit_send_oosm()`` always sends to
``lib->router_port``, so in practice ``nxt_router_oosm_handler()`` runs on the
router's *main* engine -- confirmed by the thread id in its log record -- even
though ``.oosm`` is in the worker port handler table too.  The reference is
load-bearing anyway, because the engine that runs the handler is not the only
one manipulating the process refcount: with ``listen_threads`` above one, the
worker engines take and drop references of their own on every shared-memory
message, and one of those drops can be the last.  ``nxt_port_mmap_buf_completion()``
does run on a worker engine, since that is where the response was written out.

What this proves and what it does not: it proves the code ran -- the assertions
are on log records emitted from inside those handlers, not on a response that
would look identical either way.  It does *not* prove the absence of the
use-after-free they were converted to fix; reaching the handler is necessary
for that race, not sufficient, and an uninstrumented build will usually survive
a live one.  Read a pass as "the OOSM/ACK round trip happened and the daemon
stayed correct".  The value of the test is as a driver for the sanitiser legs,
where a stale process pointer becomes a hard failure instead of a coin flip.

Cheap on purpose: one request and one deliberate client stall, about a second
of work once the PHP worker is warm.
"""

import subprocess
import threading
import time

import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.log import Log

prerequisites = {'modules': {'php': 'all'}}

client = ApplicationPHP()

APP = 'big_response'

# One segment is PORT_MMAP_DATA_SIZE == 10 MiB (src/nxt_port_memory_int.h), and
# nxt_unit_init() rounds "limits"/"shm" up to whole segments with a floor of
# one, so any value at or below 10 MiB leaves the application a single segment.
SHM_LIMIT = 1

# Comfortably past that 10 MiB even after the kernel socket buffers have
# absorbed their share of the response.  Measured on this tree: 12 MiB produces
# no OOSM at all, because the socket buffers alone hold the overflow; 16 MiB
# produces 1-9 messages depending on timing; 24 MiB produced 12-14 on every
# run.  Sized for the margin, not for the minimum.
RESPONSE_MB = 24
RESPONSE_SIZE = RESPONSE_MB * 1024 * 1024

# The client sends, then does not read for this long, so the router's queued
# response buffers - which are the application's shared memory, busy until the
# write to the client completes - pile up instead of draining.
STALL = 0.3

# > 1 so router worker engines exist and are taking and dropping their own
# references on the same process while the OOSM round trip runs.
LISTEN_THREADS = 4

READ_TIMEOUT = 30


def _body(resp, size):
    """Trailing `size` bytes of a raw response, i.e. the body: the application
    sets Content-Length and the connection is not chunked.  Kept as bytes so a
    24 MiB response is not decoded into a second copy."""

    return resp[-size:]


def test_process_shm_oosm():
    client.load(APP, limits={"shm": SHM_LIMIT}, processes=1)

    assert 'success' in client.conf(
        {"listen_threads": LISTEN_THREADS}, 'settings'
    ), 'listen_threads'

    sock = client.get(url=f'/?mb={RESPONSE_MB}', no_recv=True)

    time.sleep(STALL)

    resp = client.recvall(sock, read_timeout=READ_TIMEOUT, buff_size=65536)
    sock.close()

    body = _body(resp, RESPONSE_SIZE)

    assert len(body) == RESPONSE_SIZE, 'whole response'
    assert body.count(b'x') == RESPONSE_SIZE, 'intact response'

    log = Log.read()

    # The records asserted on below are nxt_debug()/nxt_unit_debug(), compiled
    # out unless Unit was configured with --debug.  Skip loudly rather than
    # let the test degrade into a smoke test that proves nothing about the
    # path it claims to cover.  The sanitiser workflow builds --debug, which
    # is the leg this test exists for.
    if '[debug]' not in log:
        pytest.skip('OOSM evidence needs a --debug build')

    # nxt_router_oosm_handler() logs this on entry, immediately before the
    # reference it now takes; its presence is the proof that the converted
    # call site ran.
    assert 'oosm in ' in log, 'router handled an OOSM message'

    # The application logs "oosm: retry" only after nxt_unit_wait_shm_ack()
    # has returned OK, and that happens only on an NXT_PORT_MSG_SHM_ACK
    # message (src/nxt_unit.c).  The router's only sender of that message is
    # nxt_port_broadcast_shm_ack(), reached through the converted
    # nxt_process_broadcast_shm_ack() -- so this record is proof that the
    # cross-engine post, and the reference it now carries, ran too.
    #
    # Which of the two callers broadcast is deliberately not asserted: with
    # the segment this full, nxt_router_oosm_handler() almost never finds a
    # free chunk to acknowledge with ("oosm: already free" is absent from
    # these runs) and the ACK comes from nxt_port_mmap_buf_completion().
    assert 'oosm: retry' in log, 'application got a SHM_ACK back'

    # Still serving, from the same worker, after the round trip.
    sock = client.get(url='/?mb=1', no_recv=True)
    resp = client.recvall(sock, read_timeout=READ_TIMEOUT, buff_size=65536)
    sock.close()

    assert _body(resp, 1024 * 1024).count(b'x') == 1024 * 1024, (
        'still serving'
    )

    Log.check_alerts()


# ---------------------------------------------------------------------------
# The OOSM ack path against an application that is being torn down (issue #195)
# ---------------------------------------------------------------------------

# Kills, and the gap between them.  Each one has to land while the segment is
# still full and clients are still stalled, which is what puts a SHM_ACK
# broadcast and the port teardown of the process it is aimed at on two engines
# at once.
KILLS = 5
KILL_INTERVAL = 0.4

# Stalled readers, so the response buffers -- the application's shared memory
# -- stay busy for the whole run instead of draining between kills.
STALLERS = 3

# Bounded on purpose: this is a race driver, not a soak.  Five kills at 0.4 s
# is about three seconds of workload, cheap enough for every leg including the
# sanitiser ones.  It is timing-dependent by nature: a green run is evidence
# only in the same weak sense as test_process_abrupt_teardown.py, and the
# deterministic check of the primitive this test exercises lives in the C
# suite (src/test/nxt_port_use_unless_zero_test.c).


def _oosm_app_pids():
    """PIDs currently serving the application, via ps (as conftest does)."""
    out = subprocess.check_output(
        ['ps', '-ax', '-o', 'pid', '-o', 'command']
    ).decode()

    marker = f'unit: "{APP}" application'

    return {line.split()[0] for line in out.splitlines() if marker in line}


def _wait_for_fresh_pids(spent, timeout=5):
    """A worker this run has not killed yet, so every kill is a distinct
    abrupt teardown rather than a repeat shot at a corpse.  Same reasoning as
    test_process_abrupt_teardown.py."""

    deadline = time.time() + timeout

    while time.time() < deadline:
        pids = _oosm_app_pids() - spent

        if pids:
            return pids

        time.sleep(0.05)

    return set()


def test_process_shm_oosm_kill(skip_alert):
    """Kill the OOSM-blocked worker while the router is acknowledging it.

    What this drives: the same one-segment application as above, but with
    several clients stalled at once so the segment stays full, and the worker
    SIGKILLed underneath them.  The router then has two things happening on
    two engines: on a worker engine,
    ``nxt_port_mmap_buf_completion()`` (src/nxt_port_memory.c) drains a
    completed response buffer, wins ``cmp_set(&hdr->oosm, 1, 0)`` and calls
    ``nxt_process_broadcast_shm_ack()``; on the router's main thread, the
    SIGCHLD-driven ``nxt_port_remove_pid()`` -> ``nxt_process_close_ports()``
    has dropped the app port's references and ``nxt_port_release()``
    (src/nxt_port.c) is destroying its memory pool.

    ``nxt_process_broadcast_shm_ack()`` read the app port out of
    ``process->ports`` as a bare pointer and handed it to ``nxt_port_post()``,
    whose unconditional ``nxt_atomic_fetch_add(&port->use_count, 1)`` lands on
    a port whose count is already zero: the ``port->use_count == 0`` assertion
    in ``nxt_port_mp_cleanup()`` in a --debug build, and a use-after-free of
    the port and of the posted work item in a release one.  The fix takes the
    port with ``nxt_port_use_unless_zero()`` under ``rt->processes_mutex``,
    which ``nxt_port_release()`` now unlinks under.

    What this proves and what it does not: on a --debug build the assertion
    fires as an alert and this test catches it directly -- that is the
    reproduction from the issue.  On a non-debug build it is only a UAF
    driver, worth its runtime under ASan and little without it.  A green run
    is weak evidence, as in test_process_abrupt_teardown.py; read it as "the
    OOSM ack raced repeated abrupt teardowns and the router stayed correct".
    """

    # SIGKILLing a worker is the point of the test, so the main process'
    # report of it is expected, not a finding.  Nothing else is waived --
    # in particular the assertion alert this test exists to catch is not.
    skip_alert(r'process \d+ exited on signal 9')

    client.load(APP, limits={"shm": SHM_LIMIT}, processes=1)

    assert 'success' in client.conf(
        {"listen_threads": LISTEN_THREADS}, 'settings'
    ), 'listen_threads'

    stop = threading.Event()
    failures = []

    def staller():
        """Ask for a response far larger than the segment and then read it
        slowly, so the application blocks in nxt_unit_wait_shm_ack() and the
        router keeps completing buffers for it."""

        while not stop.is_set():
            try:
                sock = client.get(url=f'/?mb={RESPONSE_MB}', no_recv=True)
                time.sleep(STALL)
                client.recvall(sock, read_timeout=READ_TIMEOUT,
                               buff_size=65536)
                sock.close()

            except Exception:
                # A request cut short by a kill is the point of the test, not
                # a failure.  Only a hang or a crashed daemon is, and those
                # are caught by the join and the assertions below.
                pass

    stallers = [threading.Thread(target=staller) for _ in range(STALLERS)]

    for thread in stallers:
        thread.start()

    # Let the first responses reach the OOSM state before killing anything.
    time.sleep(STALL * 2)

    killed = []

    for round_ in range(KILLS):
        pids = _wait_for_fresh_pids(set(killed))

        if not pids:
            failures.append(f'no fresh worker for kill {round_ + 1}')
            break

        subprocess.call(['kill', '-9', *pids])

        killed.extend(pids)

        time.sleep(KILL_INTERVAL)

    stop.set()

    for thread in stallers:
        thread.join(timeout=120)
        assert not thread.is_alive(), 'stalled request thread hung'

    assert not failures, f'{len(failures)} failure(s), first: {failures[0]}'
    assert len(killed) >= KILLS, (
        f'only {len(killed)} worker(s) killed: {killed}'
    )

    log = Log.read()

    # The direct hit.  nxt_assert() aborts the router in a --debug build, and
    # the record it prints names the file and line -- src/nxt_port.c, the
    # use_count assertion in nxt_port_mp_cleanup().  Checked before anything
    # else, because a dead router makes every other assertion here fail for
    # the wrong reason.
    assert 'assertion failed' not in log, (
        'a port was referenced after its last drop: '
        + next(
            line
            for line in log.splitlines()
            if 'assertion failed' in line
        )
    )

    # Still serving, on a worker started after the last kill.
    sock = client.get(url='/?mb=1', no_recv=True)
    resp = client.recvall(sock, read_timeout=READ_TIMEOUT, buff_size=65536)
    sock.close()

    assert _body(resp, 1024 * 1024).count(b'x') == 1024 * 1024, (
        'still serving'
    )

    if '[debug]' not in log:
        pytest.skip('OOSM evidence needs a --debug build')

    # Same proof as the test above that the OOSM round trip really ran; here
    # it also establishes that the kills landed on a blocked worker rather
    # than an idle one.
    assert 'oosm in ' in log, 'router handled an OOSM message'

    Log.check_alerts()
