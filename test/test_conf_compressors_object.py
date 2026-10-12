"""#431: the object form of "compressors" crashed the router's validator.

#647: a "level" out of the range of an int was accepted, and compression
was off for the whole configuration.
"""

import json
import shutil
import tempfile
from pathlib import Path

import pytest

from conftest import unit_run, unit_stop
from unit.control import Control
from unit.status import Status

client = Control()


@pytest.mark.parametrize(
    'compressor, result',
    [
        ({"encoding": "identity", "min_length": 10}, 'success'),
        ({"encoding": "not-a-real-encoding"}, 'error'),
    ],
)
def test_compressors_object_form(compressor, result):
    base = {
        "listeners": {"*:8080": {"pass": "routes"}},
        "routes": [{"action": {"return": 200}}],
    }
    compression = {"types": ["text/css"], "compressors": compressor}

    assert result in client.conf(
        {"settings": {"http": {"compression": compression}}, **base}
    )
    assert 'success' in client.conf(base), 'the router is still alive'


@pytest.mark.parametrize(
    'level, result',
    [
        (1, 'success'),
        (-2147483648, 'success'),
        (2147483647, 'success'),
        (2147483648, 'error'),
        (-2147483649, 'error'),
        (99999999999, 'error'),
    ],
)
def test_compressors_level_range(level, result):
    base = {
        "listeners": {"*:8080": {"pass": "routes"}},
        "routes": [{"action": {"return": 200}}],
    }
    compression = {
        "types": ["text/css"],
        "compressors": {"encoding": "identity", "level": level},
    }

    resp = client.conf(
        {"settings": {"http": {"compression": compression}}, **base}
    )

    assert result in resp, f'level {level}'

    if result == 'error':
        assert 'between -2147483648 and 2147483647' in resp['detail']


def test_compressors_level_stored(
    requires_restart, wait_for_record, monkeypatch
):
    """Earlier versions accepted any "level".  unitd still loads a stored
    configuration with one out of the range, warns, and serves requests
    with compression off.  An update through the API is refused until
    the "level" is corrected."""

    base = {
        "listeners": {"*:8080": {"pass": "routes"}},
        "routes": [{"action": {"return": 200}}],
    }
    assert 'success' in client.conf(base)

    conf = client.conf_get()
    conf['settings'] = {
        'http': {
            'compression': {
                'types': ['text/css'],
                'compressors': {'encoding': 'identity', 'level': 99999999999},
            }
        }
    }

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    (statedir / 'conf.json').write_text(json.dumps(conf))

    monkeypatch.setattr(Status, '_check_zeros', lambda: None)

    try:
        unit_run(state_dir=str(statedir))

        assert (
            client.conf_get('settings/http/compression/compressors/level')
            == 99999999999
        ), 'stored configuration loaded'

        assert wait_for_record(
            r'\[warn\].+the restored configuration has a "level" number '
            r'out of the range -2147483648 to 2147483647 at '
            r'"/settings/http/compression/compressors/level"'
        ), 'warning'

        assert client.get()['status'] == 200, 'served'

        resp = client.conf(base['routes'], 'routes')
        assert 'error' in resp, 'update refused'
        assert resp['location']['path'] == (
            '/settings/http/compression/compressors/level'
        ), 'pointer'

        assert 'success' in client.conf(
            '1', 'settings/http/compression/compressors/level'
        ), 'level corrected'
        assert 'success' in client.conf(base['routes'], 'routes'), 'update'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)
