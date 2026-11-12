"""A served file whose mtime has no HTTP date.

An HTTP date has a year of 4 digits, so Unit gives one only from
January 1, 1000 to December 31, 9999 (src/nxt_http.h).  A file with an mtime
inside this range gets a Last-Modified date.  A file outside it gets no
Last-Modified, and no date in If-Modified-Since or If-Range can match it.
Its ETag still works.

ext4 clamps such times, and the suite often keeps its files on ext4.  So
the served files go to a directory on a filesystem that keeps them: the
suite's own directory if it does, else a new directory under /dev/shm.
Only a system with neither skips the tests.
"""

import os
import re
import shutil
import tempfile
from pathlib import Path

import pytest

from unit.applications.proto import ApplicationProto
from unit.utils import public_dir

client = ApplicationProto()

DATE_MIN = -30610224000
DATE_MAX = 253402300799

IN_RANGE = {
    'min': (DATE_MIN, 'Wed, 01 Jan 1000 00:00:00 GMT'),
    'max': (DATE_MAX, 'Fri, 31 Dec 9999 23:59:59 GMT'),
}

OUT_OF_RANGE = {
    'min_minus_1': DATE_MIN - 1,
    'max_plus_1': DATE_MAX + 1,
    # Year 1, printed as "   1" by "%4d".
    'year_1': -62135596800,
    # Past the last day that nxt_gmtime() gives with a 4-byte nxt_uint_t.
    'above': 865550793600 + 86400,
    # A month in year 0 that nxt_gmtime() still gives.
    'below': -62162000000,
    # Before the first day that gives a month inside the name table.
    'far_below': -62200000000,
}

DATE = re.compile(
    r'^(Mon|Tue|Wed|Thu|Fri|Sat|Sun), \d\d '
    r'(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) \d{4} '
    r'\d\d:\d\d:\d\d GMT$'
)


def keeps_times(directory):
    path = f'{directory}/probe'

    try:
        Path(path).write_text('', encoding='utf-8')

        for mtime in (min(OUT_OF_RANGE.values()), max(OUT_OF_RANGE.values())):
            os.utime(path, (mtime, mtime))

            if os.stat(path).st_mtime != mtime:
                return False

    except (OSError, OverflowError):
        return False

    finally:
        Path(path).unlink(missing_ok=True)

    return True


@pytest.fixture(name='assets', autouse=True)
def assets_fixture(temp_dir):
    assets = f'{temp_dir}/assets'
    Path(assets).mkdir(parents=True)
    shm = None

    if not keeps_times(assets):
        try:
            shm = tempfile.mkdtemp(prefix='unit-test-mtime-', dir='/dev/shm')
        except OSError:
            pytest.skip('no filesystem keeps the file times')

        public_dir(shm)
        assets = shm

        if not keeps_times(assets):
            shutil.rmtree(shm)
            pytest.skip('no filesystem keeps the file times')

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"share": f'{assets}$uri'}}],
            "applications": {},
        }
    )

    yield assets

    if shm is not None:
        shutil.rmtree(shm, ignore_errors=True)


def make_file(assets, name, mtime):
    path = f'{assets}/{name}.txt'
    Path(path).write_text(name, encoding='utf-8')
    os.chmod(path, 0o644)
    os.utime(path, (mtime, mtime))

    assert os.stat(path).st_mtime == mtime, 'the mtime is kept'


def get(name, headers=None):
    hdrs = {'Host': 'localhost', 'Connection': 'close'}
    hdrs.update(headers or {})

    return client.get(url=f'/{name}.txt', headers=hdrs, read_timeout=5)


@pytest.mark.parametrize('name', sorted(IN_RANGE))
def test_static_mtime_in_range(assets, name):
    mtime, date = IN_RANGE[name]
    make_file(assets, name, mtime)

    resp = get(name)
    assert resp['status'] == 200
    assert resp['body'] == name
    assert resp['headers']['Last-Modified'] == date


@pytest.mark.parametrize('name', sorted(OUT_OF_RANGE))
def test_static_mtime_out_of_range(assets, name):
    make_file(assets, name, OUT_OF_RANGE[name])

    for _ in range(2):
        resp = get(name)
        assert resp['status'] == 200 and resp['body'] == name, 'served'

        last_modified = resp['headers'].get('Last-Modified')
        assert last_modified is None or DATE.match(last_modified), (
            'Last-Modified is absent or a valid HTTP date'
        )
        assert last_modified is None, 'no Last-Modified'

    etag = resp['headers']['ETag']

    # No date is compared with a file without a Last-Modified.

    for _, date in IN_RANGE.values():
        resp = get(name, {'If-Modified-Since': date})
        assert resp['status'] == 200, 'If-Modified-Since'

        resp = get(name, {'If-Unmodified-Since': date})
        assert resp['status'] == 200, 'If-Unmodified-Since'

    # A date in If-Range can equal the file time only for "year_1", and
    # only where nxt_uint_t is 8 bytes.  With 4 bytes nxt_time_parse() gives
    # no time before 1970.  A file time after now makes the ETag weak, and
    # then a date in If-Range never matches.

    if name == 'year_1':
        headers = {
            'Range': 'bytes=0-0',
            'If-Range': 'Mon, 01 Jan 0001 00:00:00 GMT',
        }

        resp = get(name, headers)
        assert resp['status'] == 200, 'If-Range date'

    # The ETag still works.

    resp = get(name, {'If-None-Match': etag})
    assert resp['status'] == 304, 'If-None-Match'

    if not etag.startswith('W/'):
        resp = get(name, {'Range': 'bytes=0-0', 'If-Range': etag})
        assert resp['status'] == 206, 'If-Range ETag'
        assert resp['body'] == name[0]
