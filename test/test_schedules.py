"""The top-level "schedules" object (docs/adr/0004-schedules.md)."""

import os
import re
import subprocess
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


# Runs: the timer bias is 50 ms and CI can stall, so bounds are loose.


def run_log():
    return f'{option.temp_dir}/schedule.log'


def put_run(schedules, listeners=True, limits=None, **extra):
    sched = app("schedule", module="wsgi",
                environment={"SCHEDULE_LOG": run_log()})
    sched["processes"] = {"max": 4, "spare": 0}

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


def wait_for_runs(name, n, timeout):
    end = time.monotonic() + timeout

    while time.monotonic() < end:
        scheds = status_schedules()

        if name in scheds and scheds[name]['runs'] >= n:
            return scheds[name]

        time.sleep(0.1)

    pytest.fail(f'"{name}" not at {n} runs: {status_schedules()}')


def test_schedules_run_fires():
    uri = '/cron/SECRET_KEY?x=1'
    begin = time.time()

    put_run({
        **run(uri=uri, headers={"Host": "example.org", "X-Cron": "yes"}),
        **run("b", uri="/b", headers={"User-Agent": "cron/1"}),
    })

    starts = wait_for_starts(3, 10, uri)

    for rec in starts:
        assert rec['method'] == 'GET'
        assert rec['path'] == '/cron/SECRET_KEY'
        assert rec['query'] == 'x=1'
        assert rec['host'] == rec['server_name'] == 'example.org'
        assert rec['x_cron'] == 'yes'
        assert rec['user_agent'] == 'FreeUnit-Schedule/cron'
        assert rec['server_port'] == '80'
        assert rec['remote_addr'] == '127.0.0.1'

    # The first start also waits for the application process.
    assert all(0.5 < gap < 2.5 for gap in gaps(starts)), gaps(starts)

    b = wait_for_starts(2, 5, '/b')[0]
    assert b['host'] is None and b['server_name'] == 'localhost', b
    assert b['user_agent'] == 'cron/1', b

    # The info line shows the URI up to its last "/" only.
    lines = Log.findall(r'.*\[info\].*schedule "cron" run \d+: GET .*')
    assert lines, 'no run was logged'
    assert all('/cron/... -> 200 in ' in line for line in lines), lines
    assert not any('SECRET_KEY' in line for line in lines), lines

    sched = wait_for_runs('cron', 2, 5)
    assert sched['skipped'] == sched['failed'] == sched['timed_out'] == 0
    assert sched['running'] in (0, 1)
    assert sched['last_status'] == 200
    assert sched['last_duration_ms'] >= 0
    assert abs(sched['last_start'] - begin) < 10, sched
    assert wait_for_runs('b', 2, 5)


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


def test_schedules_run_jitter_bounds():
    put_run(run(interval=1, jitter=1))

    starts = wait_for_starts(4, 15)
    assert all(0.7 < gap < 2.7 for gap in gaps(starts)), gaps(starts)


def test_schedules_run_overlap_skip():
    put_run(run(uri="/?sleep=2.5", overlap="skip", timeout=10))

    wait_for_starts(2, 10)
    time.sleep(1)

    assert_no_concurrency()
    skips = Log.findall(r'"cron": run skipped, the previous one is still run')
    assert len(skips) >= 2, skips

    sched = status_schedules()['cron']
    assert sched['skipped'] >= 2 and sched['runs'] >= 2, sched


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


def test_schedules_run_status():
    put_run({**run(uri="/?status=500"),
             **run("slow", uri="/?sleep=3", interval=10, run_on_start=True)},
            limits={"timeout": 1})

    assert Log.wait_for_record(
        r'\[warn\].*schedule "cron" run 1: GET /\.\.\. -> 500 in \d+ ms: '
        r'"ran /\?status=500\."'
    )
    # The application's own "limits" answer 503 first.
    assert Log.wait_for_record(
        r'\[warn\].*schedule "slow" run 1: GET /\.\.\. -> 503 in \d+ ms'
    )


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
    put_run(run(interval=3))
    time.sleep(1.5)

    # Changing "uri" only must not restart the 3 s wait.
    put_run(run(interval=3, uri="/v2"))

    assert wait_for_starts(1, 5)[0]['uri'] == '/v2'
    assert len(Log.findall(r'schedule "cron": first run in')) == 1


def test_schedules_run_remove():
    put_run(run(uri="/?sleep=2", timeout=10))
    wait_for_starts(1, 5)
    put_run({})

    # The run in flight completes and is reported; nothing starts after.
    assert Log.wait_for_record(r'schedule "cron" run 1: GET /\.\.\. -> 200')
    assert Log.findall(r'schedule "cron": removed, the run in progress')

    n = len(records('start'))
    time.sleep(2.5)
    assert len(records('start')) == n

    # With no "schedules" the key is left out, not emptied.
    assert 'schedules' not in client.conf_get('/status')


def test_schedules_run_rename():
    put_run(run())
    wait_for_starts(1, 5)
    put_run(run("other"))

    assert Log.wait_for_record(r'schedule "other" run 1: GET')
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
    assert Log.wait_for_record(r'schedule "cron" run 1: GET /\.\.\. -> 200')
    wait_for_starts(3, 8)

    assert 'success' in client.conf({"listen_threads": 2}, 'settings')
    wait_for_starts(len(records('start')) + 2, 8)


def test_schedules_run_lifecycle():
    # Leak detection is off under the sanitizer: the --debug log is the proof.
    for i in range(3):
        put_run(run(uri=f'/?sleep=1.2&v={i}', timeout=10))
        wait_for_starts(i + 1, 5)

    put_run(run(uri='/?sleep=1.2&v=3', timeout=10), listeners=False)
    time.sleep(0.5)

    put_run({})
    assert Log.wait_for_record(r'schedule "cron": removed')
    time.sleep(2)

    log = Log.read()

    if '[debug]' not in log:
        pytest.skip('the lifecycle evidence needs a --debug build')

    posted = re.findall(r'schedule "cron": run (\d+) posted', log)
    reported = re.findall(r'schedule "cron" run (\d+): GET', log)
    assert posted and sorted(posted) == sorted(reported), (posted, reported)

    inserted = re.findall(r'schedules joint ([0-9A-F]+) inserted', log)
    released = re.findall(r'schedules joint ([0-9A-F]+) released', log)
    assert len(inserted) == 4, inserted
    assert sorted(inserted) == sorted(released), (inserted, released)

    live = set()

    for line in log.splitlines():
        if m := re.search(r'router conf ([0-9A-F]+): \d+ schedules', line):
            assert m.group(1) not in live
            live.add(m.group(1))

        elif m := re.search(r'conf skcf [0-9A-F]+: 1, rtcf ([0-9A-F]+): 1$',
                            line):
            live.discard(m.group(1))

    assert not live, f'configurations never destroyed: {live}'


def test_schedules_run_otel(tmp_path):
    """A run is traced as a client's request would be: one span per run."""
    from test_otel import (FAKE_OTLP_BIN, _get_free_port, _kill,
                           _run_fake_otlp, _valid_telemetry)

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
