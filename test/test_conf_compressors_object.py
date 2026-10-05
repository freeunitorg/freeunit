"""#431: the object form of "compressors" crashed the router's validator."""

import pytest

from unit.control import Control

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
