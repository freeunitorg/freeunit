"""The top-level "schedules" object (docs/adr/0004-schedules.md)."""

import os
import re
import shutil
import signal
import subprocess
import tempfile
import time

import pytest

from unit.applications.lang.python import ApplicationPython
from unit.log import Log
from unit.option import option
from unit.utils import jsonl_records, waitforrecords

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

MISSING = object()


def app(name, **kw):
    path = f'{option.test_dir}/python/{name}'
    return {
        "type": client.get_application_type(),
        "processes": {"spare": 0},
        "path": path,
        "working_directory": path,
        **kw,
    }


def base_conf(schedules):
    one = {"one": {"module": "wsgi", "callable": "application_200"}}
    return {
        "listeners": {"*:8080": {"pass": "routes/main"}},
        "routes": {"main": [{"action": {"pass": "applications/empty"}}]},
        "upstreams": {"up": {"servers": {"127.0.0.1:8081": {}}}},
        "applications": {
            "empty": app("empty", module="wsgi"),
            "targets": app("targets", targets=one),
        },
        "schedules": schedules,
    }


def put(schedules):
    return client.conf(base_conf(schedules))


def schedule(**kwargs):
    s = {"pass": "applications/empty", "uri": "/cron", "interval": 60}
    s.update(kwargs)
    return {"cron": {k: v for k, v in s.items() if v is not MISSING}}


def assert_error(resp, detail=None, path=None):
    assert 'error' in resp, resp
    assert detail is None or detail in resp['detail'], resp
    assert path is None or resp['location']['path'] == path, resp


def test_schedules_validation_valid():
    s = schedule()['cron']
    one = {"pass": "applications/targets/one"}
    headers = {"Host": "example.org", "X-Cron": "1"}

    for schedules in [
        {},
        schedule(),
        schedule(uri="/cron/K?x=1&y=%2F", interval=300, jitter=15,
                 timeout=240, overlap="skip", headers=headers,
                 run_on_start=False),
        schedule(run_on_start=True, **one),
        schedule(interval=1, jitter=0, timeout=1),
        schedule(interval=2147483, timeout=2147483),
        schedule(interval=1073741, jitter=1073741),
        schedule(uri='/' + 'a' * 4095),
        schedule(headers={"X-Test": "a\tb c"}),
        schedule(headers={f'X-{i:02d}': 'v' * 1990 for i in range(4)}),
        # Python adds no prefix to field names: the whole 255 bytes fit.
        schedule(headers={'X-' + 'a' * 253: "x"}),
        {"drupal cron.1": s, "a" * 128: s, "b": dict(s, **one)},
    ]:
        assert 'success' in put(schedules), schedules

    assert client.conf_get('schedules/b/interval') == 60


C = '/schedules/cron'
SECONDS = [0, -1, 2147484, 1.5, "60", None, True]


def bad(key, values, detail=None):
    return [({key: v}, detail, f'{C}/{key}') for v in values]


INVALID = (
    [({k: MISSING}, f'Required parameter "{k}" is missing', f'{C}/{k}')
     for k in ('pass', 'uri', 'interval')]
    + bad('pass', ['routes', 'upstreams/up', 'applications', '$uri', 'empty',
                   'applications/missing', 'applications/targets/missing',
                   'applications/empty/extra/segment', 1])
    + bad('pass', ['routes/main'], 'must be "applications/<name>"')
    + bad('pass', ['applications/$host'], 'must not contain variables')
    + bad('uri', ['', 'cron', '*', 'http://example.org/cron', '/cron job',
                  '/cron\tjob', '/cron#frag', '/cron\x01', '/cron\x7f',
                  '/crön', '/' + 'a' * 4096])
    + bad('interval', SECONDS)
    + bad('timeout', SECONDS)
    + bad('jitter', [-1, 1.5, "1"])
    + [({"interval": 10, "jitter": 11}, 'must not exceed "interval"',
        f'{C}/jitter'),
       ({"interval": 2000000, "jitter": 1000000},
        'The sum of "interval" and "jitter"', f'{C}/jitter')]
    + bad('overlap', ['SKIP', 'queue', '', 1, True])
    + bad('run_on_start', [1, "true", None])
    + [({"headers": {n: "x"}}, 'cannot be set by a schedule',
        f'{C}/headers/{n}')
       for n in ['Content-Length', 'content-length', 'Transfer-Encoding',
                 'Connection', 'Upgrade', 'Keep-Alive', 'TE', 'te', 'Expect',
                 'Sec-WebSocket-Key', 'sec-websocket-version']]
    + [({"headers": {n: "x"}}, None, None)
       for n in ['', 'Bad Name', 'Bad:Name', 'X-é', 'X\r\nY', '(x)']]
    + [({"headers": {"X-Test": v}}, None, f'{C}/headers/X-Test')
       for v in ['a\rb', 'a\nb', 'a\x00b', 'a\x7fb', 1, None, ["x"]]]
    + [({"uri": u}, 'does not make a request', f'{C}/uri')
       for u in ['/%zz', '/../x']]
    + [({"uri": u, "headers": {"X-Test": "1"}}, 'does not make a request',
        f'{C}/uri')
       for u in ['/%zz', '/../x']]
    + [({"headers": h}, 'do not make a request', f'{C}/headers')
       for h in [{"Host": "a..b"}, {"Host": "x", "host": "y"}]]
    + [({"headers": {'X-' + 'a' * 254: "x"}}, 'is 256 bytes long',
        f'{C}/headers/X-' + 'a' * 254)]
    + [({"headers": {f'X-{i:02d}': 'v' * 2100 for i in range(4)}},
        'must not exceed 8192 bytes', f'{C}/headers'),
       ({"headers": ["Host: x"]}, None, f'{C}/headers'),
       ({"method": "POST"}, 'Unknown parameter "method"', C)]
)


@pytest.mark.parametrize('fields, detail, path', INVALID)
def test_schedules_validation_invalid(fields, detail, path):
    assert_error(put(schedule(**fields)), detail, path)


def test_schedules_validation_object():
    s = schedule()['cron']

    assert_error(put({"cron": "applications/empty"}), path=C)
    assert_error(client.conf([], 'schedules'))
    assert_error(put({"": s}), 'must be 1 to 128 bytes')
    assert_error(put({"a" * 129: s}), 'must be 1 to 128 bytes')
    assert_error(put({"cr\non": s}), 'printable ASCII')
    assert_error(put({"crön": s}), 'printable ASCII')
    assert_error(put({"a": s, "b": dict(s, interval=0)}),
                 path='/schedules/b/interval')

    assert 'success' in put({})
    assert 'success' in client.conf(s, 'schedules/x')
    assert_error(client.conf({"uri": "/", "interval": 5}, 'schedules/y'))


def test_schedules_validation_app_removed():
    conf = base_conf(schedule())
    conf['routes']['main'][0]['action']['pass'] = 'applications/targets/one'

    assert 'success' in client.conf(conf)
    assert_error(client.conf_delete('applications/empty'), path=f'{C}/pass')
    assert 'success' in client.conf_delete('schedules/cron')
    assert 'success' in client.conf_delete('applications/empty')


# Runs: the timer bias is 50 ms and CI can stall, so bounds are loose.  The
# application keeps one spare process, so a run does not wait for a start.


def run_log():
    return f'{option.temp_dir}/schedule.log'


def put_run(schedules, listeners=True, limits=None, processes=None, **extra):
    sched = app("schedule", module="wsgi",
                environment={"SCHEDULE_LOG": run_log()})
    sched["processes"] = processes or {"max": 4, "spare": 1}

    if limits is not None:
        sched["limits"] = limits

    assert 'success' in client.conf({
        "listeners": {"*:8080": {"pass": "applications/schedule"}}
        if listeners else {},
        "applications": {"schedule": sched},
        "schedules": schedules,
        **extra,
    })


def run(name="cron", **kwargs):
    return {name: {"pass": "applications/schedule", "uri": "/cron",
                   "interval": 1, **kwargs}}


def records(event=None, uri=None):
    return jsonl_records(run_log(), event, uri)


def wait_for_starts(n, timeout, uri=None):
    return waitforrecords(run_log(), n, timeout, uri=uri)


def assert_no_concurrency():
    running = 0

    for rec in records():
        running += 1 if rec['event'] == 'start' else -1
        assert running in (0, 1), 'two runs at once'


def gaps(starts):
    return [b['time'] - a['time'] for a, b in zip(starts, starts[1:])]


def status_schedules():
    return client.conf_get('/status/schedules')


def wait_for_status(name, check, timeout):
    end = time.monotonic() + timeout

    while time.monotonic() < end:
        scheds = status_schedules()

        if isinstance(scheds, dict) and name in scheds and check(scheds[name]):
            return scheds[name]

        time.sleep(0.1)

    pytest.fail(f'"{name}" not reached: {status_schedules()}')


def wait_for_runs(name, n, timeout):
    return wait_for_status(name, lambda s: s['runs'] >= n, timeout)


def wait_for_no_schedules(timeout):
    end = time.monotonic() + timeout

    while time.monotonic() < end:
        if 'schedules' not in client.conf_get('/status'):
            return

        time.sleep(0.1)

    pytest.fail(f'the schedules stay in /status: {status_schedules()}')


def test_schedules_run_fires():
    uri = '/cron/SECRET_KEY?x=1'
    begin = time.time()

    put_run({
        **run(uri=uri, headers={"Host": "example.org", "X-Cron": "yes"}),
        **run("b", uri="/b", headers={"User-Agent": "cron/1"}),
    })

    starts = wait_for_starts(4, 12, uri)

    for rec in starts:
        assert rec['method'] == 'GET'
        assert rec['path'] == '/cron/SECRET_KEY'
        assert rec['query'] == 'x=1'
        assert rec['host'] == rec['server_name'] == 'example.org'
        assert rec['x_cron'] == 'yes'
        assert rec['user_agent'] == 'FreeUnit-Schedule/cron'
        assert rec['server_port'] == '80'
        assert rec['remote_addr'] == '127.0.0.1'

    # From the second run: the first one can wait for the application.
    assert all(0.5 < gap < 2.5 for gap in gaps(starts[1:])), gaps(starts)

    b = wait_for_starts(2, 5, '/b')[0]
    assert b['host'] is None and b['server_name'] == 'localhost', b
    assert b['user_agent'] == 'cron/1', b

    # A success is not logged at info level.
    assert not Log.findall(r'\[(info|notice|warn)\].*schedule "cron" run'), \
        'a successful run was logged'

    sched = wait_for_runs('cron', 4, 5)
    assert sched['skipped'] == sched['failed'] == sched['timed_out'] == 0
    assert sched['last_status'] == 200
    # The application answers at once; the unit is milliseconds.
    assert sched['last_duration_ms'] < 1000, sched
    assert abs(sched['last_start'] - begin) < 15, sched
    assert wait_for_runs('b', 2, 5)


def test_schedules_run_unsafe_field_names():
    # A listener discards a client field with "_" or "!" in its name
    # (settings.http.discard_unsafe_fields).  A configured header is kept.
    put_run(run(uri='/u', headers={"X_Cron": "under", "!Bang": "bang"}))

    fields = wait_for_starts(1, 5, '/u')[0]['fields']
    assert fields.get('HTTP_X_CRON') == 'under', fields
    assert fields.get('HTTP_!BANG') == 'bang', fields


def test_schedules_run_header_name_255():
    # The name reaches the application in a uint8_t.  Python adds no
    # prefix, so a 255-byte name fits, and the validator refuses 256 bytes
    # (test_schedules_validation_invalid).
    name = 'X-' + 'a' * 253
    put_run(run(uri='/long', headers={name: "v"}))

    fields = wait_for_starts(1, 5, '/long')[0]['fields']
    assert fields.get('HTTP_X_' + 'A' * 253) == 'v', list(fields)

    # "runs" counts a run when it starts; "last_status" is set when it ends.
    sched = wait_for_status('cron', lambda s: s['last_status'] != 0, 5)
    assert sched['failed'] == 0 and sched['last_status'] == 200, sched

    resp = client.conf(dict(run(uri='/long')['cron'],
                            headers={name + 'a': "v"}), 'schedules/cron')
    assert 'is 256 bytes long' in resp.get('detail', ''), resp


def test_schedules_run_on_start():
    begin = time.time()

    put_run({**run(uri="/a", interval=60, run_on_start=True),
             **run("b", uri="/b", interval=3)})

    assert wait_for_starts(1, 5, '/a')[0]['time'] - begin < 2.5
    assert Log.findall(r'schedule "cron": first run in \d+ ms')

    time.sleep(max(0, begin + 2 - time.time()))
    assert records('start', '/b') == [], 'ran before the interval'

    wait_for_starts(1, 5, '/b')
    assert len(records('start', '/a')) == 1, 'ran again before the interval'


def test_schedules_run_on_start_jitter():
    # The first run waits 1 s plus up to "jitter".
    put_run({f'c{i}': run(interval=20, jitter=10, run_on_start=True)['cron']
             for i in range(5)})

    delays = [int(d) for d in
              Log.findall(r'schedule "c\d": first run in (\d+) ms')]

    assert len(delays) == 5, delays
    assert all(1000 <= d <= 11000 for d in delays), delays
    # All five at or below 1.5 s has a chance of 0.05^5.
    assert max(delays) > 1500, delays


def test_schedules_run_jitter_bounds():
    put_run(run(interval=1, jitter=1))

    starts = wait_for_starts(7, 25)
    g = gaps(starts[1:])

    assert all(0.7 < gap < 2.7 for gap in g), g
    # Without jitter every gap is about 1 s.  With it, all five at or below
    # 1.2 s has a chance of 0.2^5.
    assert max(g) > 1.2, g


def test_schedules_run_overlap_skip():
    put_run(run(uri="/?sleep=2.5", overlap="skip", timeout=10))

    wait_for_starts(2, 10)
    time.sleep(1)

    assert_no_concurrency()
    skips = Log.findall(r'"cron": run skipped, the previous one is still run')
    assert len(skips) >= 2, skips

    # The second run is still in progress; the first one took 2.5 s.
    sched = wait_for_status(
        'cron', lambda s: s['running'] == 1 and s['skipped'] >= 2, 5)
    assert sched['runs'] >= 2, sched
    assert 2400 <= sched['last_duration_ms'] < 4500, sched
    assert sched['last_status'] == 200, sched


def test_schedules_run_timeout():
    put_run(run(uri="/?sleep=3", interval=2, timeout=1))

    assert Log.wait_for_record(r'"cron" run 1: GET /\.\.\. timed out after')

    # The timeout frees the schedule; the request goes on in its worker.
    assert Log.wait_for_record(r'schedule "cron" run 2: GET /\.\.\. ')
    assert not Log.findall(r'run skipped')

    sched = status_schedules()['cron']
    assert sched['timed_out'] >= 1 and sched['failed'] == 0, sched

    time.sleep(1.5)
    assert records('end')


def test_schedules_run_timeout_one_process(skip_alert):
    # A worker that does not answer keeps its slot in "processes" until it
    # answers or exits.  With "max": 1 the later runs wait in the queue and
    # time out there.  The test ends the worker; the router starts another.
    skip_alert(r'process \d+ exited on signal 9')

    flag = f'{option.temp_dir}/hang'
    put_run(run(uri=f'/?hang={flag}', interval=2, timeout=1,
                run_on_start=True),
            processes={"max": 1, "spare": 1})

    stuck = wait_for_starts(1, 10)[0]['pid']

    try:
        sched = wait_for_status('cron', lambda s: s['timed_out'] >= 3, 20)
        assert sched['last_status'] == 503 and sched['failed'] == 0, sched
        assert sched['runs'] == sched['timed_out'] + sched['running'], sched
        assert len(records('start')) == 1, 'a run reached the held worker'

        procs = client.conf_get('/status/applications/schedule/processes')
        assert procs['running'] == 1 and procs['detached'] == 1, procs

    finally:
        # The worker would otherwise sleep through the end of the test.
        os.kill(stuck, signal.SIGKILL)

    rec = wait_for_starts(2, 15)[1]
    assert rec['pid'] != stuck, rec

    waitforrecords(run_log(), 1, 5, 'end')
    sched = wait_for_status('cron', lambda s: s['last_status'] == 200, 5)
    assert sched['failed'] == 0, sched

    assert client.get(url='/client')['status'] == 200

    procs = client.conf_get('/status/applications/schedule/processes')
    assert procs['detached'] == 0, procs


def test_schedules_run_timeout_after_header():
    put_run(run(uri="/?stream=3", interval=10, run_on_start=True, timeout=1))

    assert Log.wait_for_record(r'"cron" run 1: GET /\.\.\. timed out after')

    # The application sent 200 and a part of the body.  The router drops
    # the rest, and the status shows the timeout.
    sched = wait_for_status('cron', lambda s: s['timed_out'] > 0, 5)

    assert sched['timed_out'] == 1 and sched['failed'] == 0, sched
    assert sched['last_status'] == 503, sched

    time.sleep(2.5)
    assert records('end')


def test_schedules_run_status():
    put_run({**run(uri="/cron/SECRET_KEY?status=500"),
             **run("slow", uri="/?sleep=3", interval=10, run_on_start=True)},
            limits={"timeout": 1})

    # The line shows the path without the query, cut after its last "/",
    # and then the start of the response body.
    assert Log.wait_for_record(
        r'\[warn\].*schedule "cron" run 1: GET /cron/\.\.\. -> 500 in \d+ ms: '
        r'"ran /cron/SECRET_KEY\?status=500\."'
    )
    assert not Log.findall(r'GET /cron/SECRET'), 'the key was logged'

    # The application's own "limits" answer 503 first.
    assert Log.wait_for_record(
        r'\[warn\].*schedule "slow" run 1: GET /\.\.\. -> 503 in \d+ ms'
    )

    sched = wait_for_status('cron', lambda s: s['failed'] >= 1, 5)
    assert sched['last_status'] == 500 and sched['timed_out'] == 0, sched
    assert sched['failed'] <= sched['runs'], sched

    sched = wait_for_status('slow', lambda s: s['failed'] >= 1, 5)
    assert sched['last_status'] == 503 and sched['timed_out'] == 0, sched


def test_schedules_run_no_listeners():
    put_run(run(), listeners=False)
    wait_for_starts(3, 10)


def test_schedules_run_reconfigure_running():
    s = {"overlap": "skip", "timeout": 10}
    put_run(run(uri="/?sleep=2.5", **s))
    wait_for_starts(1, 5)

    # The state carries over by name: the run is still known to be going on.
    put_run(run(uri="/?sleep=2.5", jitter=1, **s))
    put_run(run(uri="/?sleep=2.5&v=2", jitter=1, **s))

    starts = wait_for_starts(2, 8)

    assert_no_concurrency()
    assert Log.findall(r'run skipped'), 'the running state was lost'
    assert [r['uri'] for r in starts[:2]] == ['/?sleep=2.5', '/?sleep=2.5&v=2']


def test_schedules_run_reconfigure_keeps_clock():
    begin = time.time()
    put_run({**run(interval=3), **run("b", uri="/b", interval=3)})
    time.sleep(1.5)

    # A new "uri" keeps the wait.  A new "interval" counts from the start
    # of the current wait: 4 s from the first PUT, not 1.5 + 4.
    changed = time.time()
    put_run({**run(interval=3, uri="/v2"), **run("b", uri="/b", interval=4)})

    a = wait_for_starts(1, 6, '/v2')[0]
    b = wait_for_starts(1, 6, '/b')[0]

    # The first PUT returns after the spare process is up, so the bounds
    # from "begin" are loose.  Both waits count from the same start, thus
    # the gap between the two runs is 1 s.  The waits started before the
    # first PUT returned, so the runs come at most 1.5 s and 2.5 s after
    # "changed".  A wait that starts again at the second PUT gives 3 s and
    # 4 s or more.
    assert 2.8 < a['time'] - begin < 5, a['time'] - begin
    assert 3.8 < b['time'] - begin < 6, b['time'] - begin
    assert 0.5 < b['time'] - a['time'] < 1.5, b['time'] - a['time']
    assert a['time'] - changed < 2.5, a['time'] - changed
    assert b['time'] - changed < 3.5, b['time'] - changed
    assert records('start', '/cron') == []
    assert len(Log.findall(r'schedule "cron": first run in')) == 1


def test_schedules_run_reconfigure_interval_running():
    # Run 1 starts about 1 s after the PUT and sleeps 3.5 s.  At 2.5 s into
    # it, "interval" goes from 60 to 2: the wait is over, so the next run is
    # due at once and is skipped.  The one after it is due 2 s later.
    s = {"uri": "/?sleep=3.5", "run_on_start": True, "timeout": 10}
    put_run(run(interval=60, **s))

    first = wait_for_starts(1, 5)[0]
    time.sleep(max(0, first['time'] + 2.5 - time.time()))

    changed = time.time()
    put_run(run(interval=2, **s))

    second = wait_for_starts(2, 8)[1]
    sched = status_schedules()['cron']

    assert sched['skipped'] == 1, sched
    assert 1.5 < second['time'] - changed < 3.0, second['time'] - changed
    assert len(Log.findall(r'schedule "cron": first run in')) == 1


def test_schedules_run_remove():
    put_run(run(uri="/?sleep=2", timeout=10))
    wait_for_starts(1, 5)
    put_run({})

    # The run in flight completes and is reported; nothing starts after.
    assert Log.findall(r'schedule "cron": removed, the run in progress')
    assert status_schedules()['cron']['running'] == 1

    waitforrecords(run_log(), 1, 5, 'end')

    # With no "schedules" the key is left out, not emptied.
    wait_for_no_schedules(5)

    n = len(records('start'))
    time.sleep(2.5)
    assert len(records('start')) == n


def test_schedules_run_rename():
    put_run(run())
    wait_for_starts(1, 5)
    put_run(run("other"))

    assert wait_for_runs('other', 1, 5)
    assert Log.findall(r'schedule "cron": removed')


def test_schedules_run_access_log():
    put_run(run(uri="/logged"), access_log=f'{option.temp_dir}/access.log')

    # Listener requests are served alongside.
    for _ in range(10):
        assert client.get()['status'] == 200

    assert Log.wait_for_record(
        r'127\.0\.0\.1 - - \[.+\] "GET /logged HTTP/1\.1" 200 \d+ "-" '
        r'"FreeUnit-Schedule/cron"',
        'access.log',
    )
    assert [r['user_agent'] for r in records('start')].count(None) == 10


def test_schedules_run_listen_threads():
    put_run(run(uri="/?sleep=1.5", timeout=10),
            settings={"listen_threads": 4})
    wait_for_starts(1, 5)

    # The run's engine may be the one that goes; it must stay until the end.
    assert 'success' in client.conf({"listen_threads": 1}, 'settings')
    waitforrecords(run_log(), 1, 5, 'end')
    wait_for_starts(3, 8)

    assert 'success' in client.conf({"listen_threads": 2}, 'settings')
    wait_for_starts(len(records('start')) + 2, 8)


def fifo_closed(fd):
    # EOF on the read end: no process has the FIFO open for writing.
    try:
        while os.read(fd, 65536):
            pass

    except BlockingIOError:
        return False

    return True


def test_schedules_run_lifecycle():
    # Each configuration has its own access log, a FIFO.  The router closes
    # the log when the last reference to its configuration goes, and a run
    # holds one through the joint.  So a FIFO still open for writing is a
    # configuration that was never released.  The FIFOs are outside the
    # test directory, which the suite copies.
    tmp = tempfile.mkdtemp(prefix='unit-schedule-')
    logs = [f'{tmp}/lifecycle{i}.log' for i in range(4)]
    fds = []

    try:
        for log in logs:
            os.mkfifo(log)
            fds.append(os.open(log, os.O_RDONLY | os.O_NONBLOCK))

        for i in range(3):
            put_run(run(uri=f'/?sleep=1.2&v={i}', timeout=10),
                    access_log=logs[i])
            wait_for_starts(i + 1, 5)

        put_run(run(uri='/?sleep=1.2&v=3', timeout=10), listeners=False,
                access_log=logs[3])
        time.sleep(0.5)

        put_run({})
        assert Log.wait_for_record(r'schedule "cron": removed')
        wait_for_no_schedules(5)

        starts = records('start')
        ends = records('end')
        assert len(starts) == len(ends) >= 3, (starts, ends)

        for _ in range(50):
            left = [log for log, fd in zip(logs, fds) if not fifo_closed(fd)]

            if not left:
                break

            time.sleep(0.1)

        assert not left, f'configurations never released: {left}'

    finally:
        for fd in fds:
            os.close(fd)

        shutil.rmtree(tmp, ignore_errors=True)

    log = Log.read()

    if '[debug]' not in log:
        return

    # A --debug build also logs each step.
    posted = re.findall(r'schedule "cron": run (\d+) posted', log)
    reported = re.findall(r'schedule "cron" run (\d+): GET', log)
    assert posted and sorted(posted) == sorted(reported), (posted, reported)

    inserted = re.findall(r'schedules joint ([0-9A-F]+) inserted', log)
    released = re.findall(r'schedules joint ([0-9A-F]+) released', log)
    assert len(inserted) == 4, inserted
    assert sorted(inserted) == sorted(released), (inserted, released)


def test_schedules_run_otel(tmp_path):
    """A run is traced as a client's request would be: one span per run."""
    from test_otel import (FAKE_OTLP_BIN, _get_free_port, _kill,
                           _require_otel, _run_fake_otlp, _valid_telemetry)

    _require_otel()

    if not os.path.exists(FAKE_OTLP_BIN):
        pytest.skip(f'{FAKE_OTLP_BIN} not installed (build via test/fake_otlp)')

    port = _get_free_port()
    dump = str(tmp_path / 'otlp_dump.bin')
    proc = _run_fake_otlp(port, requests=1, dump=dump)

    try:
        put_run(run(uri="/cron/otel", run_on_start=True),
                settings={"telemetry": _valid_telemetry(port)})

        wait_for_starts(1, 10, '/cron/otel')

        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            pytest.fail('the run exported no span')

        with open(dump, 'rb') as f:
            body = f.read()

        assert b'/cron/otel' in body, body
        assert b'http.response.status_code' in body, body

    finally:
        _kill(proc)
