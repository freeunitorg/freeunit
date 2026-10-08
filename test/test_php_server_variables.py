import json
import os

import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.applications.tls import ApplicationTLS
from unit.option import option

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()
client_tls = ApplicationTLS()

# Each request of these tests runs on the same worker.  The script returns
# the fixed $_SERVER entries and then changes several of them, so a value
# kept from an earlier request, or a change that reaches a shared string,
# shows in the next response.


def root_dir():
    return os.path.realpath(f'{option.test_dir}/php/server_variables')


def server_variables(resp):
    assert resp['status'] == 200, 'status'

    return json.loads(resp['body'])


def fixed(resp):
    return {
        'SERVER_SOFTWARE': resp['headers']['Server'],
        'SERVER_PROTOCOL': 'HTTP/1.1',
        'DOCUMENT_ROOT': root_dir(),
        'REMOTE_ADDR': '127.0.0.1',
        'SERVER_ADDR': '127.0.0.1',
        'SERVER_PORT': '8080',
    }


def check_two_requests(get, post, script=None, https=None):
    root = root_dir()
    extra = {} if https is None else {'HTTPS': https}

    resp = get(
        url='/index.php/info?a=1',
        headers={
            'Host': 'one.example',
            'X-Probe': 'first',
            'Connection': 'close',
        },
    )
    first = server_variables(resp)

    if script is None:
        expect = {
            'PHP_SELF': '/index.php/info',
            'PATH_INFO': '/info',
            'SCRIPT_NAME': '/index.php',
            'SCRIPT_FILENAME': f'{root}/index.php',
        }
    else:
        expect = {
            'PHP_SELF': '/index.php',
            'SCRIPT_NAME': '/index.php',
            'SCRIPT_FILENAME': f'{root}/index.php',
        }

    assert first == {
        'pid': first['pid'],
        **fixed(resp),
        **expect,
        **extra,
        'REQUEST_METHOD': 'GET',
        'REQUEST_URI': '/index.php/info?a=1',
        'QUERY_STRING': 'a=1',
        'SERVER_NAME': 'one.example',
        'HTTP_X_PROBE': 'first',
    }, 'first request'

    resp = post(
        url='/other.php?b=2',
        headers={
            'Host': 'two.example',
            'Content-Type': 'text/plain',
            'Connection': 'close',
        },
        body='12345',
    )
    second = server_variables(resp)

    if script is None:
        expect = {
            'PHP_SELF': '/other.php',
            'SCRIPT_NAME': '/other.php',
            'SCRIPT_FILENAME': f'{root}/other.php',
        }

    assert second == {
        'pid': first['pid'],
        **fixed(resp),
        **expect,
        **extra,
        'REQUEST_METHOD': 'POST',
        'REQUEST_URI': '/other.php?b=2',
        'QUERY_STRING': 'b=2',
        'SERVER_NAME': 'two.example',
        'CONTENT_LENGTH': '5',
        'CONTENT_TYPE': 'text/plain',
    }, 'second request'


def test_php_server_variables_root():
    client.load('server_variables')

    check_two_requests(client.get, client.post)


def test_php_server_variables_script():
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "applications/server"}},
            "applications": {
                "server": {
                    "type": client.get_application_type(),
                    "processes": {"spare": 0},
                    "root": root_dir(),
                    "script": "index.php",
                }
            },
        }
    )

    check_two_requests(client.get, client.post, script='index.php')


def test_php_server_variables_https():
    if not option.available['modules']['openssl']:
        pytest.skip('requires openssl')

    client.load('server_variables')

    client_tls.certificate()

    assert 'success' in client.conf(
        {
            "pass": "applications/server_variables",
            "tls": {"certificate": "default"},
        },
        'listeners/*:8080',
    )

    check_two_requests(client_tls.get_ssl, client_tls.post_ssl, https='on')
