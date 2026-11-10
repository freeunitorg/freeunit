import json
import shutil
import tempfile
from pathlib import Path

import pytest

from conftest import unit_run, unit_stop
from unit.applications.lang.python import ApplicationPython
from unit.applications.proto import ApplicationProto
from unit.option import option
from unit.status import Status

client = ApplicationProto()
client_python = ApplicationPython()


@pytest.fixture(autouse=True)
def setup_method_fixture(temp_dir):
    path = Path(f'{temp_dir}/index.html')
    path.write_text('0123456789', encoding='utf-8')

    assert 'success' in client.conf(
        {
            "listeners": {
                "*:8080": {"pass": "routes"},
            },
            "routes": [
                {
                    "action": {
                        "share": str(path),
                        "response_headers": {
                            "X-Foo": "foo",
                        },
                    }
                }
            ],
        }
    )


def action_update(conf):
    assert 'success' in client.conf(conf, 'routes/0/action')


def test_response_headers(temp_dir):
    resp = client.get()
    assert resp['status'] == 200, 'status 200'
    assert resp['headers']['X-Foo'] == 'foo', 'header 200'

    assert 'success' in client.conf(f'"{temp_dir}"', 'routes/0/action/share')

    resp = client.get()
    assert resp['status'] == 301, 'status 301'
    assert resp['headers']['X-Foo'] == 'foo', 'header 301'

    assert 'success' in client.conf('"/blah"', 'routes/0/action/share')

    resp = client.get()
    assert resp['status'] == 404, 'status 404'
    assert 'X-Foo' not in client.get()['headers'], 'header 404'


def test_response_last_action():
    assert 'success' in client.conf(
        {
            "listeners": {
                "*:8080": {"pass": "routes/first"},
            },
            "routes": {
                "first": [
                    {
                        "action": {
                            "pass": "routes/second",
                            "response_headers": {
                                "X-Foo": "foo",
                            },
                        }
                    }
                ],
                "second": [
                    {
                        "action": {"return": 200},
                    }
                ],
            },
            "applications": {},
        }
    )

    assert 'X-Foo' not in client.get()['headers']


def test_response_pass(require):
    require({'modules': {'python': 'any'}})

    assert 'success' in client_python.conf(
        {
            "listeners": {
                "*:8080": {"pass": "routes"},
            },
            "routes": [
                {
                    "action": {
                        "pass": "applications/empty",
                        "response_headers": {
                            "X-Foo": "foo",
                        },
                    }
                },
            ],
            "applications": {
                "empty": {
                    "type": client_python.get_application_type(),
                    "processes": {"spare": 0},
                    "path": f'{option.test_dir}/python/empty',
                    "working_directory": f'{option.test_dir}/python/empty',
                    "module": "wsgi",
                }
            },
        }
    )

    assert client.get()['headers']['X-Foo'] == 'foo'


def test_response_fallback():
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "action": {
                        "share": "/blah",
                        "fallback": {
                            "return": 200,
                            "response_headers": {
                                "X-Foo": "foo",
                            },
                        },
                    }
                }
            ],
        }
    )

    assert client.get()['headers']['X-Foo'] == 'foo'


def test_response_headers_var():
    assert 'success' in client.conf(
        {
            "X-Foo": "$uri",
        },
        'routes/0/action/response_headers',
    )

    assert client.get()['headers']['X-Foo'] == '/'


def test_response_headers_remove():
    assert 'success' in client.conf(
        {"etag": None},
        'routes/0/action/response_headers',
    )

    assert 'ETag' not in client.get()['headers']


def test_response_headers_invalid(skip_alert):
    def check_invalid(conf):
        resp = client.conf(conf, 'routes/0/action/response_headers')
        assert 'error' in resp

        return resp

    resp = check_invalid({"X-Foo": "$u"})
    assert 'detail' in resp and 'Unknown variable' in resp['detail']


def test_response_headers_name_invalid():
    # Response-header names must be RFC 9110 tokens; anything outside the
    # token grammar (spaces, ':', CR/LF, HTAB) is rejected at config time so
    # a control-API client can't define a name carrying a header boundary.
    def check_invalid_name(name):
        assert 'error' in client.conf(
            {name: "v"}, 'routes/0/action/response_headers'
        ), f'name {name!r} rejected'

    check_invalid_name("X Foo")
    check_invalid_name("X\tFoo")
    check_invalid_name("X:Foo")
    check_invalid_name("X\r\nEvil")
    check_invalid_name("X\x00Foo")

    # A name using the allowed token special characters still loads.
    assert 'success' in client.conf(
        {"X-Custom_Header.1": "v"}, 'routes/0/action/response_headers'
    ), 'valid token name'


def test_response_headers_name_reserved():
    # The body length of a response is set by Unit, so these names are
    # rejected in any letter case.
    def check_reserved(name):
        assert 'error' in client.conf(
            {name: "10"}, 'routes/0/action/response_headers'
        ), f'name {name!r} rejected'

    check_reserved("Content-Length")
    check_reserved("content-length")
    check_reserved("CONTENT-LENGTH")
    check_reserved("Transfer-Encoding")
    check_reserved("transfer-encoding")
    check_reserved("tRANSFER-eNCODING")

    # Similar names still load.
    assert 'success' in client.conf(
        {"Content-Length-X": "v", "X-Transfer-Encoding": "v"},
        'routes/0/action/response_headers',
    ), 'similar names accepted'


def test_response_headers_name_reserved_stored_null(
    requires_restart, wait_for_record, monkeypatch
):
    """A stored null entry for a reserved name is kept as it was before the
    names were refused: it goes to the removal path, not to the skip of a
    value.  Unit sets the body length itself, so the response still has
    exactly one Content-Length."""

    conf = client.conf_get()
    conf['routes'][0]['action']['response_headers'] = {
        "X-Foo": "foo",
        "Content-Length": None,
    }

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    (statedir / 'conf.json').write_text(json.dumps(conf))

    monkeypatch.setattr(Status, '_check_zeros', lambda: None)

    try:
        unit_run(state_dir=str(statedir))

        assert wait_for_record(
            r'\[warn\].+the restored configuration has the "Content-Length" '
            r'response header'
        ), 'warning'

        resp = client.get()
        assert resp['status'] == 200, 'served'
        assert resp['body'] == '0123456789', 'body'
        assert resp['headers']['X-Foo'] == 'foo', 'other header kept'

        names = [n.lower() for n in resp['headers']]
        assert names.count('content-length') == 1, 'one Content-Length'
        assert resp['headers']['Content-Length'] == '10', 'file length'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


def test_response_headers_name_reserved_stored(
    requires_restart, wait_for_record, monkeypatch
):
    """Earlier versions accepted "content-length" in other letter cases and
    any "Transfer-Encoding".  unitd still loads such a stored configuration,
    warns, and does not send these fields.  An update through the API is
    refused until they are removed."""

    conf = client.conf_get()
    conf['routes'][0]['action']['response_headers'] = {
        "X-Foo": "foo",
        "content-length": "5",
        "Transfer-Encoding": "chunked",
    }

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    (statedir / 'conf.json').write_text(json.dumps(conf))

    monkeypatch.setattr(Status, '_check_zeros', lambda: None)

    try:
        unit_run(state_dir=str(statedir))

        assert client.conf_get('routes/0/action/response_headers') == {
            "X-Foo": "foo",
            "content-length": "5",
            "Transfer-Encoding": "chunked",
        }, 'stored configuration loaded'

        for name in ('content-length', 'Transfer-Encoding'):
            assert wait_for_record(
                rf'\[warn\].+the restored configuration has the "{name}" '
                rf'response header at "/routes/0/action/response_headers/'
                rf'{name}"'
            ), f'warning {name}'

        resp = client.get()
        assert resp['status'] == 200, 'served'
        assert resp['body'] == '0123456789', 'body'
        assert resp['headers']['X-Foo'] == 'foo', 'other header kept'

        names = [n.lower() for n in resp['headers']]
        assert names.count('content-length') == 1, 'one Content-Length'
        assert resp['headers']['Content-Length'] == '10', 'file length'
        assert 'transfer-encoding' not in names, 'no Transfer-Encoding'

        resp = client.conf(
            '"bar"', 'routes/0/action/response_headers/X-Bar'
        )
        assert 'error' in resp, 'update refused'

        assert 'success' in client.conf(
            {"X-Foo": "foo"}, 'routes/0/action/response_headers'
        ), 'corrected'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


def test_response_headers_value_control():
    # Static values must not carry control bytes (< 0x20 except HTAB, plus
    # DEL 0x7F) -- otherwise a CR/LF in a value splits the response header
    # block. HTAB and high/UTF-8 bytes stay allowed.
    def check_invalid_value(value):
        assert 'error' in client.conf(
            {"X-Foo": value}, 'routes/0/action/response_headers'
        ), f'value {value!r} rejected'

    check_invalid_value("a\rb")
    check_invalid_value("a\nb")
    check_invalid_value("a\x00b")
    check_invalid_value("a\x7fb")

    def check_valid_value(value):
        assert 'success' in client.conf(
            {"X-Foo": value}, 'routes/0/action/response_headers'
        ), f'value {value!r} accepted'

    check_valid_value("a\tb")
    check_valid_value("naïve")


def test_response_headers_var_control():
    # A templated value can only be validated at request time. When it
    # expands to bytes carrying a header boundary the value is dropped, not
    # injected -- and it must not appear as a smuggled header either.
    assert 'success' in client.conf(
        {"X-Foo": "$arg_foo"}, 'routes/0/action/response_headers'
    )

    # Benign expansion passes through untouched.
    assert client.get(url='/?foo=bar')['headers'].get('X-Foo') == 'bar'

    # CR/LF expansion is rejected: no split-in "Evil" header, and the
    # tainted X-Foo is dropped rather than emitted verbatim.
    resp = client.get(url='/?foo=a%0d%0aEvil:%20injected')
    assert resp['status'] == 200, 'request still served'
    assert 'Evil' not in resp['headers'], 'response header injection'
    assert 'X-Foo' not in resp['headers'], 'tainted value dropped'


def test_response_headers_inline_spill(temp_dir):
    # More than 16 response headers exercise the resp.inline_fields[16] -> list
    # spill in the response field store; together with the static handler's own
    # headers this is well past the 16-slot boundary, and all must reach the
    # client.
    headers = {f'X-Rh-{i}': str(i) for i in range(24)}

    action_update(
        {
            'share': f'{temp_dir}/index.html',
            'response_headers': headers,
        }
    )

    resp = client.get()

    assert resp['status'] == 200, 'spill status'
    for i in range(24):
        assert resp['headers'].get(f'X-Rh-{i}') == str(i), f'response header {i}'
