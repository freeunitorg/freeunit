"""The "limits"/"shm" option of an application.

libunit takes the limit as a uint32_t (nxt_unit_init_t.shm_limit), and
counts it in segments of PORT_MMAP_DATA_SIZE, 10 MiB.  Earlier versions
accepted any number and cut it to its low 32 bits, so 4294967296 gave one
segment.  The sum that rounds the limit up to whole segments also wrapped
in uint32_t, so 4294967295 gave one segment too.

The application reports the number of segments that it created for its own
responses (test/python/shm_limit/wsgi.py).  A response of 24 MiB that the
client does not read at once needs more than one segment, if the limit lets
the application create them (see test_process_shm_oosm.py).
"""

import json
import shutil
import tempfile
import time
from pathlib import Path

import pytest
from conftest import unit_run, unit_stop
from unit.applications.lang.python import ApplicationPython
from unit.option import option
from unit.status import Status

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

LIMIT = 2**32 - 1

# The application counts its segments in /proc/self/maps.
linux_only = pytest.mark.skipif(
    option.system != 'Linux', reason='/proc/self/maps is Linux only'
)

RESPONSE_SIZE = 24 * 1024 * 1024


def segments_after_large_response():
    """Sends a request for RESPONSE_SIZE bytes, reads the response after a
    pause, and returns the number of segments the application created."""

    sock = client.get(
        headers={
            'Host': 'localhost',
            'X-Length': str(RESPONSE_SIZE),
            'Connection': 'close',
        },
        no_recv=True,
    )

    time.sleep(0.3)

    resp = client.recvall(sock, read_timeout=30, buff_size=65536)
    sock.close()

    assert resp[-RESPONSE_SIZE:].count(b'x') == RESPONSE_SIZE, 'response'

    # One process, so the same process as for the large response.
    resp = client.get(headers={'Host': 'localhost', 'Connection': 'close'})

    return int(resp['headers']['X-Segments'])


def test_python_shm_limit_validation():
    client.load('empty')

    def shm(value):
        return client.conf({'shm': value}, 'applications/empty/limits')

    for value in (LIMIT + 1, 2**40):
        resp = shm(value)

        assert 'error' in resp, f'shm {value}'
        assert (
            resp['detail'] == f'The "shm" number must not exceed {LIMIT}.'
        ), f'shm {value} detail'
        assert (
            resp['location']['path'] == '/applications/empty/limits/shm'
        ), f'shm {value} pointer'

    assert 'success' in shm(LIMIT), 'largest shm'
    assert client.conf_get('applications/empty/limits/shm') == LIMIT


@linux_only
def test_python_shm_limit_segments():
    # The control: one segment.
    client.load('shm_limit', limits={'shm': 1}, processes=1)

    assert segments_after_large_response() == 1, 'one segment'

    client.load('shm_limit', limits={'shm': LIMIT}, processes=1)

    assert segments_after_large_response() > 1, 'largest shm'


@linux_only
def test_python_shm_limit_stored(
    requires_restart, wait_for_record, monkeypatch
):
    """Earlier versions accepted a larger "shm".  unitd still loads a stored
    configuration with one, and the application gets LIMIT, not the low
    32 bits of the number."""

    client.load('shm_limit', limits={'shm': 1}, processes=1)

    conf = client.conf_get()
    conf['applications']['shm_limit']['limits']['shm'] = 2**33

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    (statedir / 'conf.json').write_text(json.dumps(conf))

    # unit_run() checks that /status lists no application.  The stored
    # configuration has one.
    monkeypatch.setattr(Status, '_check_zeros', lambda: None)

    try:
        unit_run(state_dir=str(statedir))

        assert (
            client.conf_get('applications/shm_limit/limits/shm') == 2**33
        ), 'stored configuration loaded'

        # 2**33 cut to 32 bits is 0, and so one segment.
        assert segments_after_large_response() > 1, 'stored shm'

        assert wait_for_record(
            r'\[warn\].+the restored configuration has a "shm" number over '
            rf'{LIMIT} at "/applications/shm_limit/limits/shm"'
        ), 'warning'

        # The control API validates the whole configuration.
        resp = client.conf(
            {'*:8080': {'pass': 'applications/shm_limit'}}, 'listeners'
        )
        assert 'error' in resp, 'update refused'
        assert (
            resp['location']['path'] == '/applications/shm_limit/limits/shm'
        ), 'pointer'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)
