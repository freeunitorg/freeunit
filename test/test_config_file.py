"""unitd started with "--config FILE".

The configuration comes from FILE, so a container can start unitd without a
writable state directory; a FILE that cannot run stops unitd with exit
status 1 instead of starting it empty.
"""

import json
import os
import re
import signal
from pathlib import Path

import pytest

from unit.http import HTTP1
from unit.unitd import Unitd
from unit.utils import waitforfiles

client = HTTP1()


@pytest.fixture
def unitd():
    instance = Unitd()
    instance.config = f'{instance.dir}/config.json'

    yield instance

    instance.stop()


def start(unitd, conf, control=True):
    """Starts unitd on conf, written to unitd.config unless it is None."""
    if conf is not None:
        text = conf if isinstance(conf, str) else json.dumps(conf)
        Path(unitd.config).write_text(text, encoding='utf-8')

    unitd.start(
        '--config',
        unitd.config,
        pid='none',
        control=None if control else 'none',
    )


def test_config_file_no_control_no_pid_no_state(unitd):
    defaults = {path: path.exists() for path in unitd.default_paths().values()}

    start(unitd, unitd.returning(204), control=False)

    assert waitforfiles(unitd.listener), f'listener\n{unitd.read_log()}'
    assert unitd.get()['status'] == 204, 'configuration from the file'

    assert unitd.process.poll() is None, 'running'
    assert not Path(unitd.state).exists(), 'state directory not created'
    assert sorted(p.name for p in Path(unitd.dir).iterdir()) == [
        'app.sock',
        'config.json',
        'stderr.log',
        'unit.log',
    ], 'no pid file and no control socket'

    # "none" must not fall back to the built-in paths either.
    for path, existed in defaults.items():
        assert path.exists() == existed, f'{path} untouched'


def test_config_file_utf8_bom(unitd):
    Path(unitd.config).write_bytes(
        b'\xef\xbb\xbf' + json.dumps(unitd.returning(204)).encode()
    )

    start(unitd, None)

    assert waitforfiles(unitd.listener), f'listener\n{unitd.read_log()}'
    assert unitd.get()['status'] == 204, 'a BOM is skipped as by the API'


def test_config_file_ignores_state(unitd):
    Path(unitd.state).mkdir()
    Path(f'{unitd.state}/conf.json').write_text(
        json.dumps(unitd.returning(418)), encoding='utf-8'
    )

    start(unitd, unitd.returning(204))

    assert waitforfiles(unitd.listener), f'listener\n{unitd.read_log()}'
    assert unitd.get()['status'] == 204, 'the file wins over conf.json'


def test_config_file_read_only_api(unitd):
    conf = unitd.returning(204)

    start(unitd, conf)

    assert waitforfiles(unitd.listener, unitd.control), unitd.read_log()

    resp = unitd.get('/config', addr=unitd.control)
    assert resp['status'] == 200, 'GET /config'
    assert json.loads(resp['body']) == conf, 'GET returns the file'

    assert unitd.get('/status', addr=unitd.control)['status'] == 200, 'status'

    for method in ('PUT', 'POST', 'DELETE'):
        resp = client.http(
            method,
            url='/config/routes',
            body='[]',
            sock_type='unix',
            addr=unitd.control,
        )
        assert resp['status'] == 405, method
        assert 'read-only' in resp['body'], f'{method} body'

    assert unitd.get()['status'] == 204, 'configuration unchanged'


def controller_pid(main_pid):
    """The pid of the controller child of main, or None."""
    children = Path(f'/proc/{main_pid}/task/{main_pid}/children').read_text()

    for pid in children.split():
        cmdline = Path(f'/proc/{pid}/cmdline').read_bytes()
        if b'controller' in cmdline:
            return int(pid)

    return None


def control_get(unitd):
    """GET /config, or None while no controller answers."""
    try:
        resp = unitd.get('/config', addr=unitd.control)
    except OSError:
        return None

    return resp if resp['status'] == 200 else None


def test_config_file_controller_restart_keeps_config(unitd):
    conf = unitd.returning(204)

    start(unitd, conf)

    assert waitforfiles(unitd.listener, unitd.control), unitd.read_log()

    old = controller_pid(unitd.process.pid)
    assert old is not None, 'controller'

    # A broken file must not reach the restarted controller.
    Path(unitd.config).write_text('{"listeners": ', encoding='utf-8')
    os.kill(old, signal.SIGKILL)

    unitd.wait_for(
        lambda: controller_pid(unitd.process.pid) not in (None, old),
        'controller not restarted',
    )

    assert unitd.process.poll() is None, 'unitd keeps running'

    resp = unitd.wait_for(lambda: control_get(unitd), 'no GET /config')

    assert json.loads(resp['body']) == conf, 'the checked configuration'
    assert unitd.get()['status'] == 204, 'still served'


def invalid_cases():
    """(conf, alert, modules); {config} and {dir} are filled in per test."""
    return [
        pytest.param(
            None,
            r'cannot read the configuration file "{config}"',
            {},
            id='missing',
        ),
        pytest.param(
            '{"listeners": ',
            r'the configuration file "{config}" is not valid JSON at '
            r'line 1, column \d+',
            {},
            id='json',
        ),
        pytest.param(
            {"listeners": {"*:7999": {"pass": "routes/missing"}}},
            r'the configuration file "{config}" is invalid at '
            r'"/listeners/\*:7999/pass"',
            {},
            id='invalid',
        ),
        pytest.param(
            {
                "listeners": {
                    "*:7999": {
                        "pass": "routes",
                        "tls": {"certificate": "missing"},
                    }
                },
                "routes": [],
            },
            r'the configuration file "{config}" is invalid at '
            r'"/listeners/\*:7999/tls/certificate": '
            r'Certificate "missing" is not found',
            {'openssl': 'any'},
            id='certificate',
        ),
        pytest.param(
            {
                "listeners": {"unix:{dir}/missing/x.sock": {"pass": "routes"}},
                "routes": [{"action": {"return": 204}}],
            },
            r'failed to apply the configuration file "{config}"',
            {},
            id='apply',
        ),
    ]


@pytest.mark.parametrize('conf, alert, modules', invalid_cases())
def test_config_file_invalid_exits(unitd, require, conf, alert, modules):
    if modules:
        require({'modules': modules})

    if isinstance(conf, dict):
        conf = json.loads(json.dumps(conf).replace('{dir}', unitd.dir))

    start(unitd, conf)

    assert unitd.process.wait(timeout=30) == 1, 'exit status'

    alert = alert.replace('{config}', re.escape(unitd.config))

    assert re.search(rf'\[alert\].+{alert}', unitd.read_log()), (
        unitd.read_log()
    )
