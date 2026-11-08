"""Requests waiting for a worker when the last one dies (#665).

The application has one process with one thread.  A first request keeps that
thread busy, a few more wait in the shared queue behind it, unacknowledged,
and the test SIGKILLs the process.  The router starts a replacement only after
it has looked at every unacknowledged request: one that no worker claimed is
sent again, and one that the dead worker claimed is answered 503 and never
run again.  Every waiting request here is still in the queue, so each must be
served by the replacement exactly once, and none may hang.

The window this change closes, a worker killed between dequeue and claim,
cannot be hit deterministically from a test, so this guards the surrounding
behaviour: the deferred start and the resend must not lose, duplicate or
stall requests that were still queued.
"""

import os
import signal
import subprocess
import threading
import time

from unit.applications.lang.python import ApplicationPython
from unit.option import option

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

WAITING = 4
READ_TIMEOUT = 15


def _app_pids():
    out = subprocess.check_output(
        ['ps', '-ax', '-o', 'pid', '-o', 'command']
    ).decode()

    return {
        int(line.split()[0])
        for line in out.splitlines()
        if 'unit: "requeue_count" application' in line
    }


def _app_pids_of(name):
    def pids():
        out = subprocess.check_output(
            ['ps', '-ax', '-o', 'pid', '-o', 'command']
        ).decode()

        return {
            int(line.split()[0])
            for line in out.splitlines()
            if f'unit: "{name}" application' in line
        }

    return pids


def _wait(predicate, timeout=10):
    deadline = time.time() + timeout

    while time.time() < deadline:
        value = predicate()

        if value:
            return value

        time.sleep(0.05)

    return None


def _lines(path):
    try:
        with open(path, encoding='utf-8') as f:
            return f.read().split()

    except FileNotFoundError:
        return []


def test_python_requeue_last_worker(skip_alert, skip_fds_check):
    skip_fds_check(main=True, router=True, controller=True)
    skip_alert(r'app process \d+ exited on signal 9')

    client.load(
        'requeue_count',
        processes={"spare": 1, "max": 1, "idle_timeout": 30},
    )

    log = f'{option.temp_dir}/requeue.log'

    def request(tag, delay=0, results=None):
        try:
            resp = client.post(
                headers={
                    'Host': 'localhost',
                    'X-Tag': tag,
                    'X-Log': log,
                    'X-Delay': str(delay),
                    'Connection': 'close',
                },
                body='x' * 64,
                read_timeout=READ_TIMEOUT,
            )
            status = resp['status']
            body = resp['body']

        except BaseException as exc:  # noqa: BLE001
            status = None
            body = repr(exc)

        if results is not None:
            results[tag] = (status, body)

    assert client.get()['status'] == 200

    pids = _wait(_app_pids)
    assert pids, 'no application process'

    results = {}
    threads = [
        threading.Thread(target=request, args=('busy', 30, results))
    ]
    threads[0].start()

    assert _wait(lambda: 'busy' in _lines(log)), 'busy request did not start'

    for i in range(WAITING):
        t = threading.Thread(target=request, args=(f'w{i}', 0, results))
        t.start()
        threads.append(t)

    # Let the waiting requests reach the shared queue behind "busy".
    time.sleep(0.5)

    assert _lines(log) == ['busy'], 'a waiting request ran too early'

    for pid in pids:
        os.kill(pid, signal.SIGKILL)

    started = time.time()

    for t in threads:
        t.join(READ_TIMEOUT + 5)

    assert not any(t.is_alive() for t in threads), 'a request hung'
    assert time.time() - started < READ_TIMEOUT, 'too slow'

    ran = _lines(log)

    for i in range(WAITING):
        tag = f'w{i}'

        assert results[tag] == (200, tag), f'{tag}: {results[tag]}'
        assert ran.count(tag) == 1, f'{tag} ran {ran.count(tag)} times'

    assert results['busy'][0] != 200, 'busy request survived its process'
    assert ran.count('busy') == 1, 'the claimed request ran again'


def test_python_requeue_last_worker_spare(skip_alert, skip_fds_check):
    """No start may run while the router decides about the waiting requests.

    With "spare" above 1 more than one start may be pending, so new requests
    that arrive while the engines retract the waiting ones could start a
    process of their own.  Such a process could take a waiting request from
    the queue before its engine retracts it, and the request would be
    answered 503 while it runs.  Every request here, the waiting ones and the
    ones that keep arriving across the kill, must get 200 and run once.
    """

    skip_fds_check(main=True, router=True, controller=True)
    skip_alert(r'app process \d+ exited on signal 9')

    client.load(
        'requeue_count',
        processes={"spare": 2, "max": 4, "idle_timeout": 30},
    )

    log = f'{option.temp_dir}/requeue.log'

    def request(tag, delay, results):
        try:
            resp = client.post(
                headers={
                    'Host': 'localhost',
                    'X-Tag': tag,
                    'X-Log': log,
                    'X-Delay': str(delay),
                    'Connection': 'close',
                },
                body='x' * 64,
                read_timeout=READ_TIMEOUT,
            )
            results[tag] = (resp['status'], resp['body'])

        except BaseException as exc:  # noqa: BLE001
            results[tag] = (None, repr(exc))

    assert client.get()['status'] == 200

    pids = _wait(lambda: len(_app_pids()) >= 2 and _app_pids())
    assert pids, 'the spare processes did not start'

    results = {}
    threads = []

    for i in range(len(pids)):
        t = threading.Thread(target=request, args=(f'busy{i}', 30, results))
        t.start()
        threads.append(t)

    assert _wait(
        lambda: sum(tag.startswith('busy') for tag in _lines(log))
        == len(pids)
    ), 'busy requests did not start'

    def start(tag):
        t = threading.Thread(target=request, args=(tag, 0, results))
        t.start()
        threads.append(t)

    for i in range(WAITING):
        start(f'w{i}')

    time.sleep(0.5)

    stop = threading.Event()

    def arrivals():
        n = 0

        while not stop.is_set():
            start(f'n{n}')
            n += 1
            time.sleep(0.01)

    feeder = threading.Thread(target=arrivals)
    feeder.start()

    time.sleep(0.1)

    for pid in pids:
        os.kill(pid, signal.SIGKILL)

    time.sleep(1)

    stop.set()
    feeder.join()

    for t in list(threads):
        t.join(READ_TIMEOUT + 5)

    assert not any(t.is_alive() for t in threads), 'a request hung'

    ran = _lines(log)

    for tag, (status, body) in results.items():
        if tag.startswith('busy'):
            assert ran.count(tag) == 1, f'{tag} ran again'
            continue

        assert (status, body) == (200, tag), f'{tag}: {status} {body}'
        assert ran.count(tag) == 1, f'{tag} ran {ran.count(tag)} times'


def test_python_requeue_last_worker_unaccounted(skip_alert, skip_fds_check):
    """No sweep while a process left by an expired start may still register.

    Such a process is in unaccounted_processes, and once it announces itself
    nxt_router_app_port_ready() lets it take messages from the shared queue.
    Had the router retracted the waiting requests meanwhile, a request this
    process took would be counted for the dead worker and answered 503 while
    it runs.  The router therefore leaves the waiting requests to their
    acknowledgement deadline.  The exact interleaving cannot be forced from
    here; this pins the path: no request hangs, none runs twice, and none is
    answered anything but 200 after it ran.
    """

    skip_fds_check(main=True, router=True, controller=True)
    skip_alert(
        r'app process \d+ exited on signal 9',
        r'did not start within',
        r'start_timeout',
    )

    marker = f'{option.temp_dir}/late.marker'
    log = f'{option.temp_dir}/requeue.log'

    client.load(
        'requeue_late',
        processes={"spare": 1, "max": 2, "idle_timeout": 30},
        limits={"start_timeout": 1},
        environment={"UNIT_LATE_MARKER": marker, "UNIT_LATE_DELAY": "4"},
    )

    def processes():
        return client.conf_get('/status/applications/requeue_late/processes')

    def request(tag, delay, results):
        try:
            resp = client.post(
                headers={
                    'Host': 'localhost',
                    'X-Tag': tag,
                    'X-Log': log,
                    'X-Delay': str(delay),
                    'Connection': 'close',
                },
                body='x' * 64,
                read_timeout=READ_TIMEOUT,
            )
            results[tag] = (resp['status'], resp['body'])

        except BaseException as exc:  # noqa: BLE001
            results[tag] = (None, repr(exc))

    # The late start below must be the second process, so no request is sent
    # before the marker exists.  A worker can pause its first read on an mmap
    # segment, and the router then starts another process for that request
    # at once: that process does not sleep, and no start can expire.
    assert _wait(lambda: processes()['idle'] == 1), processes()

    pids = _wait(_app_pids_of('requeue_late'))
    assert pids, 'no application process'
    assert processes()['running'] == 1, processes()

    with open(marker, 'w', encoding='utf-8'):
        pass

    results = {}
    threads = []

    def start(tag, delay=0):
        t = threading.Thread(target=request, args=(tag, delay, results))
        t.start()
        threads.append(t)

    start('busy', 30)
    assert _wait(lambda: 'busy' in _lines(log)), 'busy request did not start'

    for i in range(WAITING):
        start(f'w{i}')

    # The second process sleeps past its start deadline.
    assert _wait(lambda: processes()['unaccounted'] == 1), processes()

    os.unlink(marker)

    for pid in pids:
        os.kill(pid, signal.SIGKILL)

    for t in threads:
        t.join(READ_TIMEOUT + 5)

    assert not any(t.is_alive() for t in threads), 'a request hung'

    ran = _lines(log)

    assert ran.count('busy') == 1, 'the busy request ran again'

    for i in range(WAITING):
        tag = f'w{i}'

        assert ran.count(tag) <= 1, f'{tag} ran {ran.count(tag)} times'

        if ran.count(tag) == 1:
            assert results[tag] == (200, tag), f'{tag}: {results[tag]}'

    # The late process is adopted and serves.
    assert _wait(lambda: processes()['running'] >= 1), processes()
    assert client.get()['status'] == 200
