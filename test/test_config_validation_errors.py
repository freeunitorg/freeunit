"""Tests for debuggable config validation errors.

Covers the additive response fields introduced by roadmap item D5:
 - ``location.path`` (RFC 6901 JSON Pointer to the offending member).
 - ``suggestion`` (best-match member name when the input was a close typo).

Legacy response fields (``error``, ``detail``, ``location.offset/line/column``)
must remain unchanged; several tests guard that.

All cases are expressed using listeners + routes so they run without any
language SAPI module configured.
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


def _put(conf, url='/config'):
    return client.conf(conf, url)


def test_unknown_top_level_key_has_root_path():
    r = _put({"foo": 1})
    assert 'error' in r
    assert 'foo' in r['detail']
    assert r['location']['path'] == ''


def test_misspelled_listeners_suggests_listeners():
    r = _put({"listners": {}})
    assert 'error' in r
    assert r.get('suggestion') == 'listeners'


def test_misspelled_applications_suggests_applications():
    r = _put({"aplications": {}})
    assert 'error' in r
    assert r.get('suggestion') == 'applications'


def test_nested_unknown_key_has_container_path():
    r = _put(
        {
            "listeners": {
                "*:8080": {"pass": "routes", "unknownkey": 1}
            },
            "routes": [{"action": {"return": 200}}],
        }
    )
    assert 'error' in r
    assert r['location']['path'] == f'/listeners/*:{port_map.port(8080)}'


def test_array_element_path_points_into_routes():
    r = _put(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {"action": {"return": 200}},
                {"action": {"return": 200}},
                {"action": {"return": 200}, "unknownkey": 1},
            ],
        }
    )
    assert 'error' in r
    assert r['location']['path'].startswith('/routes/2')


def test_type_error_carries_path():
    r = _put(
        {
            "listeners": {"*:8080": {"pass": 123}},
            "routes": [{"action": {"return": 200}}],
        }
    )
    assert 'error' in r
    assert '/listeners' in r['location']['path']


def test_rfc6901_escapes_tilde():
    """Route name ``a~b`` must encode as ``a~0b`` in the JSON Pointer."""
    r = _put(
        {
            "listeners": {"*:8080": {"pass": "routes/a~b"}},
            "routes": {
                "a~b": [{"action": {"return": 200}, "unknownkey": 1}]
            },
        }
    )
    assert 'error' in r
    assert r['location']['path'] == '/routes/a~0b/0'


def test_rfc6901_escapes_slash():
    """Route name ``a/b`` must encode as ``a~1b``, not split the pointer."""
    r = _put(
        {"routes": {"a/b": [{"action": {"return": 200}, "unknownkey": 1}]}}
    )
    assert 'error' in r
    assert r['location']['path'] == '/routes/a~1b/0'


def test_parse_error_keeps_legacy_location_shape():
    # A malformed document never reaches validation, so it has no pointer to
    # report; the location must stay exactly what it was before this change.
    r = client.conf('{"listeners"', '/config')
    assert 'error' in r
    assert 'offset' in r['location']
    assert 'path' not in r['location']


def test_no_suggestion_when_distance_exceeds_threshold():
    r = _put({"zzzzzzz": 1})
    assert 'error' in r
    assert 'suggestion' not in r


def test_no_suggestion_for_short_unrelated_name():
    # The distance threshold alone admits 2 edits for a two-byte name, which
    # is every two-letter member in the schema.  A suggestion must also cover
    # at most half the shorter name, so "zz" gets none rather than "if".
    r = _put({"access_log": {"zz": 1}})
    assert 'error' in r
    assert 'zz' in r['detail']
    assert 'suggestion' not in r


def test_app_type_errors_point_at_the_type_member():
    # nxt_conf_vldt_app() reaches into the "type" member itself rather than
    # descending through the instrumented iterator, so all three of its
    # checks have to report against "type" and not against the application
    # object that contains it.
    r = _put({"applications": {"demo": {"type": 1}}})
    assert 'error' in r
    assert r['location']['path'] == '/applications/demo/type'

    r = _put({"applications": {"demo": {}}})
    assert 'error' in r
    assert r['location']['path'] == '/applications/demo/type'

    r = _put({"applications": {"demo": {"type": "nosuch"}}})
    assert 'error' in r
    assert r['location']['path'] == '/applications/demo/type'


def test_upstream_missing_servers_points_at_the_member():
    # Same convention as a missing NXT_CONF_VLDT_REQUIRED member: report
    # against the absent member, not its container.
    r = _put({"upstreams": {"u1": {}}})
    assert 'error' in r
    assert r['location']['path'] == '/upstreams/u1/servers'


def _app(processes):
    return {
        "applications": {
            "d": {
                "type": "external",
                "executable": "x",
                "processes": processes,
            }
        }
    }


def test_post_traversal_checks_point_at_the_member():
    # Semantic checks that run after nxt_conf_vldt_object() has finished the
    # traversal see a path with the member's segment already popped, so they
    # have to re-attribute explicitly or they name the container instead.
    r = _put({"access_log": {"path": ""}})
    assert 'error' in r
    assert r['location']['path'] == '/access_log/path'

    r = _put({"access_log": {"path": "/tmp/a", "format": "`{"}})
    assert 'error' in r
    assert r['location']['path'] == '/access_log/format'

    for processes, member in [
        ({"spare": -1}, 'spare'),
        ({"max": 0}, 'max'),
        ({"spare": 5, "max": 2}, 'spare'),
        ({"idle_timeout": -1}, 'idle_timeout'),
    ]:
        r = _put(_app(processes))
        assert 'error' in r, processes
        assert (
            r['location']['path'] == f'/applications/d/processes/{member}'
        ), processes


def test_backward_compat_success_has_no_new_fields():
    r = _put(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    )
    assert 'success' in r
    assert 'error' not in r
    assert 'suggestion' not in r


def test_backward_compat_error_shape_preserved():
    r = _put({"foo": 1})
    assert 'error' in r
    assert 'detail' in r
    # 'success' key must never coexist with an error.
    assert 'success' not in r
    # Legacy message wording preserved verbatim.
    assert r['detail'] == 'Unknown parameter "foo".'


def _access_log(access_format):
    return {
        "listeners": {"*:8080": {"pass": "routes"}},
        "routes": [{"action": {"return": 200}}],
        "access_log": {
            "path": f'{option.temp_dir}/access.log',
            "format": access_format,
        },
    }


def _warning(variable, field=None):
    if field is None:
        return (
            rf'\[warn\].+the access log format uses the "{variable}" '
            r'variable\.  It writes the value'
        )

    return (
        rf'\[warn\].+the access log format uses the "{variable}" '
        rf'variable in the "{field}" field\.'
    )


@pytest.mark.parametrize(
    'variable',
    [
        'header_authorization',
        'header_Proxy_Authorization',
        'header_cookie',
        'header_set_cookie',
        'response_header_set_cookie',
        'response_header_Authorization',
    ],
)
def test_access_log_warns_on_credential_header(
    variable, wait_for_record, findall
):
    # The configuration is accepted, as in earlier versions, with a warning.
    r = _put(_access_log(f"seen ${{{variable}}} at $time_local"))
    assert 'success' in r
    assert wait_for_record(_warning(variable))

    # One warning for one variable.
    assert len(findall(_warning(variable))) == 1


def test_access_log_warns_on_credential_header_in_object_format(
    wait_for_record,
):
    r = _put(
        _access_log(
            {
                "time": "$time_local",
                "auth": "Bearer $header_authorization",
            }
        )
    )
    assert 'success' in r
    assert wait_for_record(
        _warning('header_authorization', 'auth')
    )


def test_access_log_no_warning_for_a_refused_update(search_in_file):
    # The router warns when it applies the format, so a configuration that
    # the control API refuses gives no warning.
    conf = _access_log("[$header_proxy_authorization]")
    conf['unknown'] = 1

    assert 'error' in _put(conf)
    assert search_in_file(r'"header_proxy_authorization" variable') is None


def test_access_log_no_warning_on_other_header_variables(search_in_file):
    r = _put(
        _access_log(
            "$header_referer $header_user_agent "
            "$response_header_content_length $header_authorization_x"
        )
    )
    assert 'success' in r
    assert (
        search_in_file(
            r'uses the "(header_referer|header_user_agent'
            r'|response_header_content_length|header_authorization_x)"'
        )
        is None
    )


@pytest.mark.parametrize(
    'access_format, field',
    [
        ("c=[$header_cookie]", None),
        ({"t": "$time_local", "c": "$header_cookie"}, 'c'),
    ],
)
def test_access_log_credential_header_stored(
    access_format, field, requires_restart, wait_for_record
):
    """A stored configuration with a credential header in the access log
    format loads, warns, serves, logs the header as before, and can still
    be updated through the API."""

    assert 'success' in _put(_access_log("$remote_addr"))

    unit_stop()

    # unit_run() makes a new temporary directory, so the log is kept beside
    # the state.
    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    log = statedir / 'access.log'

    conf = _access_log(access_format)
    conf['access_log']['path'] = str(log)
    (statedir / 'conf.json').write_text(
        json.dumps(port_map.expected(conf))
    )

    try:
        unit_run(state_dir=str(statedir))

        assert (
            client.conf_get('access_log/format') == access_format
        ), 'stored configuration loaded'

        assert wait_for_record(_warning('header_cookie', field)), 'warning'

        assert (
            client.get(
                headers={
                    'Host': 'localhost',
                    'Cookie': 'session=STORED',
                    'Connection': 'close',
                }
            )['status']
            == 200
        ), 'served'

        for _ in range(50):
            if log.exists() and 'STORED' in log.read_text():
                break
            time.sleep(0.1)
        else:
            pytest.fail('the header is not in the access log')

        assert 'success' in client.conf(conf['routes'], 'routes'), 'update'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)
