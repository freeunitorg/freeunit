"""The access log writes a credential header as "REDACTED" unless
"log_credentials" is true.

The cases use listeners + routes, so they run without any language module.
"""

import json
import shutil
import tempfile
import time
from pathlib import Path

import pytest

from conftest import unit_run, unit_stop
from unit import port as port_map
from unit.control import Control
from unit.option import option

client = Control()

SECRETS = {
    'Authorization': 'Bearer TOKEN',
    'Proxy-Authorization': 'Basic PROXY',
    'Cookie': 'session=COOKIE',
}


def _conf(log_format, log_credentials=None, path=None):
    access_log = {
        'path': path or f'{option.temp_dir}/access.log',
        'format': log_format,
    }

    if log_credentials is not None:
        access_log['log_credentials'] = log_credentials

    return {
        'listeners': {'*:8080': {'pass': 'routes'}},
        'routes': [
            {
                'action': {
                    'return': 200,
                    'response_headers': {'Set-Cookie': 'id=SETCOOKIE'},
                }
            }
        ],
        'access_log': access_log,
    }


def _request():
    assert (
        client.get(
            headers={'Host': 'localhost', 'Connection': 'close', **SECRETS}
        )['status']
        == 200
    )


TEXT = (
    '[$header_authorization] [${header_Proxy_Authorization}] '
    '[$header_cookie] [$response_header_set_cookie] '
    '[$header_host] [$response_header_content_length]'
)


def test_access_log_credentials_redacted(wait_for_record):
    assert 'success' in client.conf(_conf(TEXT))

    _request()

    assert wait_for_record(
        r'^\[REDACTED\] \[REDACTED\] \[REDACTED\] \[REDACTED\] '
        r'\[localhost\] \[0\]$',
        'access.log',
    )


def test_access_log_credentials_logged(wait_for_record):
    assert 'success' in client.conf(_conf(TEXT, log_credentials=True))

    _request()

    assert wait_for_record(
        r'^\[Bearer TOKEN\] \[Basic PROXY\] \[session=COOKIE\] '
        r'\[id=SETCOOKIE\] \[localhost\] \[0\]$',
        'access.log',
    )


def test_access_log_credentials_redacted_explicit_false(wait_for_record):
    assert 'success' in client.conf(
        _conf('a=$header_authorization', log_credentials=False)
    )

    _request()

    assert wait_for_record(r'^a=REDACTED$', 'access.log')


def test_access_log_credentials_redacted_object(wait_for_record):
    assert 'success' in client.conf(
        _conf(
            {
                'auth': 'Bearer: $header_authorization',
                'cookie': '$header_cookie',
                'host': '$header_host',
            }
        )
    )

    _request()

    line = wait_for_record(r'^\{.+\}$', 'access.log')
    assert line is not None
    assert json.loads(line.group(0)) == {
        'auth': 'Bearer: REDACTED',
        'cookie': 'REDACTED',
        'host': 'localhost',
    }


def test_access_log_credentials_other_names_kept(wait_for_record):
    # Only the four names are redacted; a longer name is another header.
    assert 'success' in client.conf(
        _conf('[$header_authorization_x] [$cookie_session] [$header_host]')
    )

    _request()

    assert wait_for_record(r'^\[-\] \[COOKIE\] \[localhost\]$', 'access.log')


def test_access_log_credentials_stored(
    requires_restart, wait_for_record
):
    """A configuration stored by an earlier version, with a credential header
    in the format, loads and logs the header as "REDACTED"."""

    assert 'success' in client.conf(_conf('$remote_addr'))

    unit_stop()

    # unit_run() makes a new temporary directory, so the log is kept beside
    # the state.
    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    log = statedir / 'access.log'

    conf = _conf('c=[$header_cookie]', path=str(log))
    (statedir / 'conf.json').write_text(
        json.dumps(port_map.expected(conf))
    )

    try:
        unit_run(state_dir=str(statedir))

        assert client.conf_get('access_log/format') == 'c=[$header_cookie]'

        _request()

        for _ in range(50):
            if log.exists() and log.read_text():
                break
            time.sleep(0.1)

        assert log.read_text() == 'c=[REDACTED]\n'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)
