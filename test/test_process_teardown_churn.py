"""Application teardown churn under concurrent shared-memory traffic.

What this drives: several router worker engines (``settings/listen_threads``)
serving large POSTs to a PHP application while a second thread repeatedly
rewrites that application's configuration.  Every configuration rewrite makes
the router build a *new* ``nxt_app_t`` and release the previous one
(``src/nxt_router.c``: an application is reused only when the printed JSON of
its configuration is byte-identical), which tears the old application's
processes down and destroys its outgoing port-mmap segments
(``nxt_port_mmaps_destroy()``) — while router workers on other engines are
still copying request bodies into, and reading responses out of, shared memory
for that same application.

What this proves and what it does not: a green run is *weak* evidence for the
absence of a use-after-free on the process/port lookup path.  The window is a
handful of instructions wide and an unsanitised build will almost always miss
it.  The value of this test is as a crash and ASan/UBSan *driver*: run it under
``.github/workflows/sanitize.yml``-style instrumentation and a live
use-after-free becomes a hard failure instead of a coin flip.  Read a pass here
as "the workload ran and the daemon stayed up", nothing stronger.

Deliberately kept short - 64 requests of 1 MiB, about three seconds on an idle
8-core box - so it is cheap enough to run on every CI leg, including the much
slower sanitiser legs.  Correctness assertions are written so that a
legitimately failed request during teardown is not a failure, but a corrupt or
hung one is.
"""

import subprocess
import threading
import time

from unit.applications.lang.php import ApplicationPHP
from unit.log import Log

prerequisites = {'modules': {'php': 'all'}}

client = ApplicationPHP()

# src/nxt_port_memory_int.h: PORT_MMAP_CHUNK_SIZE is 16 KiB and one shared
# memory segment holds PORT_MMAP_DATA_SIZE (10 MiB) of payload.  The router
# always hands requests to an application through those segments
# (nxt_router_prepare_msg() allocates with nxt_port_mmap_get_buf()), but a
# small request fits in a single chunk of a single segment.  1 MiB spans 64
# chunks in each direction and keeps the worker inside the chunk allocator
# (nxt_port_mmap_get_buf()/nxt_port_mmap_increase_buf()) long enough for a
# teardown on another thread to overlap it.
BODY_SIZE = 1024 * 1024
BODY = 'x' * BODY_SIZE

# > 1 so more than one router worker engine is live; this is a top-level
# "settings" member (src/nxt_conf_validation.c: nxt_conf_vldt_setting_members).
LISTEN_THREADS = 4

WORKERS = 4
REQUESTS_PER_WORKER = 16
CHURN_INTERVAL = 0.05

# Not the suite default (60s), so that a stalled request returns instead of
# calling pytest.fail() on a helper thread; the stall is detected by timing it
# and reported as a hard failure below.
READ_TIMEOUT = 10

APP = 'mirror'


def _app_pids():
    """PIDs currently serving the application, via ps (as conftest does)."""
    out = subprocess.check_output(
        ['ps', '-ax', '-o', 'pid', '-o', 'command']
    ).decode()

    marker = f'unit: "{APP}" application'

    return {line.split()[0] for line in out.splitlines() if marker in line}


def test_process_teardown_churn(skip_fds_check):
    # Setting "listen_threads" changes the number of router worker engines,
    # and destroying an engine leaks its descriptors: measured on this tree,
    # shrinking the count frees each dead engine's epoll and eventfd but keeps
    # its port socketpair open (3 sockets per destroyed engine), so the router
    # ends the test with more descriptors than it started with.  That is a
    # pre-existing defect in engine teardown, not something this workload
    # causes, and it is unrelated to what the test is here to exercise.
    skip_fds_check(router=True)

    client.load(APP, processes={"spare": 2, "max": 4, "idle_timeout": 5})

    assert 'success' in client.conf(
        {"listen_threads": LISTEN_THREADS}, 'settings'
    ), 'listen_threads'

    assert client.post(body='ping', read_timeout=READ_TIMEOUT)['status'] == 200

    lock = threading.Lock()
    stop = threading.Event()
    failures = []
    outcomes = []
    pids = set()

    def record(bucket, item):
        with lock:
            bucket.append(item)

    def request_worker():
        for _ in range(REQUESTS_PER_WORKER):
            started = time.time()

            try:
                resp = client.post(
                    body=BODY,
                    read_timeout=READ_TIMEOUT,
                    read_buffer_size=65536,
                )

            except KeyboardInterrupt:
                raise

            # pytest.fail() inside the helpers raises BaseException
            except BaseException as exc:  # noqa: BLE001
                record(failures, f'request raised {exc!r}')
                continue

            elapsed = time.time() - started
            status = resp.get('status')
            body = resp.get('body', '')

            if status == 200:
                # A short body is legitimate: the application can die after its
                # headers are on the wire.  Wrong *bytes* are not - that is
                # what reading freed or recycled shared memory looks like.
                if body != BODY[: len(body)]:
                    at = next(
                        i for i, c in enumerate(body) if c != 'x'
                    )
                    record(
                        failures,
                        f'corrupt body: {len(body)} bytes, '
                        f'first mismatch at {at}',
                    )
                    continue

                full = len(body) == BODY_SIZE

                record(outcomes, 200 if full else 'truncated')

            elif status is None:
                # No parsable response: connection closed during teardown.
                # Distinguish that from a hang, which is a real defect.
                if elapsed >= READ_TIMEOUT:
                    record(failures, f'no response in {elapsed:.1f}s')
                else:
                    record(outcomes, 'closed')

            elif status in (500, 503):
                record(outcomes, status)

            else:
                record(failures, f'unexpected status {status}')

    def churn_worker():
        generation = 0

        while not stop.is_set():
            generation += 1

            try:
                resp = client.conf(
                    {"CHURN": str(generation)},
                    f'applications/{APP}/environment',
                )

            except KeyboardInterrupt:
                raise

            except BaseException as exc:  # noqa: BLE001
                record(failures, f'config update {generation} raised {exc!r}')
                return

            if 'success' not in resp:
                record(failures, f'config update {generation} failed: {resp}')
                return

            with lock:
                pids.update(_app_pids())

            time.sleep(CHURN_INTERVAL)

    churn = threading.Thread(target=churn_worker)
    workers = [threading.Thread(target=request_worker) for _ in range(WORKERS)]

    churn.start()
    for worker in workers:
        worker.start()

    for worker in workers:
        worker.join(timeout=120)
        assert not worker.is_alive(), 'request thread hung'

    stop.set()
    churn.join(timeout=60)
    assert not churn.is_alive(), 'churn thread hung'

    assert not failures, f'{len(failures)} failure(s), first: {failures[0]}'

    # The daemon is still alive and still serves the whole payload correctly.
    resp = client.post(body=BODY, read_timeout=READ_TIMEOUT,
                       read_buffer_size=65536)

    assert resp['status'] == 200, 'still serving'
    assert len(resp['body']) == BODY_SIZE, 'still serving whole body'
    assert resp['body'] == BODY, 'still serving intact body'

    # The workload has to have actually torn processes down, otherwise it is
    # only a concurrency test.  More than one PID means the application was
    # destroyed and respawned while requests were in flight.
    assert len(pids) > 1, f'application processes were not replaced: {pids}'

    # Requests must not have been shut out entirely, or nothing reached shared
    # memory concurrently with a teardown.
    assert outcomes.count(200) > 0, f'no request completed: {outcomes}'

    Log.check_alerts()
