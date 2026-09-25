"""Tests for debuggable config validation errors.

Covers the additive response fields introduced by roadmap item D5:
 - ``location.path`` (RFC 6901 JSON Pointer to the offending member).
 - ``suggestion`` (best-match member name when the input was a close typo).

Legacy response fields (``error``, ``detail``, ``location.offset/line/column``)
must remain unchanged; several tests guard that.

All cases are expressed using listeners + routes so they run without any
language SAPI module configured.
"""

from unit.control import Control

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
    assert r['location']['path'] == '/listeners/*:8080'


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
