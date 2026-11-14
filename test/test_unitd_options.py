"""unitd command-line options that the shared unitd of the suite never uses."""

import json
from pathlib import Path

import pytest

from unit.unitd import Unitd
from unit.utils import waitforfiles


@pytest.fixture
def unitd():
    instance = Unitd()

    yield instance

    instance.stop()


def test_unitd_pid_none(unitd):
    default = unitd.default_paths()['--pid']
    existed = default.exists()

    Path(unitd.state).mkdir()
    Path(f'{unitd.state}/conf.json').write_text(
        json.dumps(unitd.returning(204)), encoding='utf-8'
    )

    unitd.start(pid='none')

    assert waitforfiles(unitd.listener, unitd.control), unitd.read_log()
    assert unitd.get()['status'] == 204, 'running'

    assert not Path(unitd.pid).exists(), 'no pid file'
    assert default.exists() == existed, f'{default} untouched'
    assert not Path('none').exists(), 'no pid file named "none"'
