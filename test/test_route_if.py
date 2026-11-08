"""Tests for the "if" option of the route "match" object.

The condition uses the same syntax as the "if" option of the access log:
a template string that is false when it renders to an empty string, "0",
"false", "null" or "undefined", and true otherwise; a leading "!" negates
the result.  A condition that fails to render, such as an njs expression
that throws, is an error in a route match rather than a non-match: the
request ends as 500.  The access log writes no record for such a request.
"""

import json

import pytest

from unit.applications.proto import ApplicationProto

client = ApplicationProto()


@pytest.fixture(autouse=True)
def setup_method_fixture():
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "match": {"method": "GET"},
                    "action": {"return": 200},
                }
            ],
            "applications": {},
        }
    ), 'routing configure'


def set_if(condition):
    assert 'success' in client.conf(
        json.dumps(condition), 'routes/0/match/if'
    ), 'if configure'


def set_match(match):
    assert 'success' in client.conf(match, 'routes/0/match'), 'match configure'


def check_if(condition, url, status):
    set_if(condition)
    assert client.get(url=url)['status'] == status, f'if {condition} {url}'


def test_route_if_const():
    def try_if(condition, status):
        check_if(condition, f'/{condition}', status)

    try_if('', 404)
    try_if('0', 404)
    try_if('false', 404)
    try_if('null', 404)
    try_if('undefined', 404)
    try_if('!', 200)
    try_if('!0', 200)
    try_if('!null', 200)
    try_if('1', 200)
    try_if('!1', 404)


def test_route_if_variable():
    set_if('$arg_foo')
    assert client.get(url='/bar?bar')['status'] == 404, 'no argument'
    assert client.get(url='/foo_empty?foo')['status'] == 404, 'empty argument'
    assert client.get(url='/foo?foo=1')['status'] == 200, 'argument'

    set_if('!$arg_foo')
    assert client.get(url='/bar?bar')['status'] == 200, 'negated, no argument'
    assert (
        client.get(url='/foo_empty?foo')['status'] == 200
    ), 'negated, empty argument'
    assert client.get(url='/foo?foo=1')['status'] == 404, 'negated, argument'

    set_if('$host')
    assert client.get()['status'] == 200, 'host'

    set_if('$arg_foo$arg_bar')
    assert client.get(url='/?bar=1')['status'] == 200, 'two variables'
    assert client.get(url='/?baz=1')['status'] == 404, 'two variables empty'

    # The falsy set is compared against the rendered value, so a variable
    # carrying one of those literals is false just as the literal is.

    set_if('$arg_foo')
    for value in ['0', 'false', 'null', 'undefined']:
        assert (
            client.get(url=f'/?foo={value}')['status'] == 404
        ), f'"{value}" through a variable'
    assert (
        client.get(url='/?foo=00')['status'] == 200
    ), '"00" through a variable'


def test_route_if_njs(require):
    require({'modules': {'njs': 'any'}})

    # comparison

    set_if("`${args.foo == '1'}`")
    assert client.get(url='/foo_1?foo=1')['status'] == 200, 'equal'
    assert client.get(url='/foo_2?foo=2')['status'] == 404, 'not equal'

    set_if("!`${args.foo == '1'}`")
    assert client.get(url='/foo_1?foo=1')['status'] == 404, 'negated equal'
    assert client.get(url='/foo_2?foo=2')['status'] == 200, 'negated not equal'

    set_if("`${uri == '/admin'}`")
    assert client.get(url='/admin')['status'] == 200, 'uri equal'
    assert client.get(url='/admin/')['status'] == 404, 'uri not equal'

    # regular expression

    set_if('`${/^\\/admin/.test(uri)}`')
    assert client.get(url='/admin/index.html')['status'] == 200, 'regexp'
    assert client.get(url='/user/index.html')['status'] == 404, 'regexp no match'

    set_if("`${headers['User-Agent'].split('/')[0] == 'curl'}`")
    assert (
        client.get(headers={'Host': 'localhost', 'User-Agent': 'curl/8.0'})[
            'status'
        ]
        == 200
    ), 'header split'
    assert (
        client.get(headers={'Host': 'localhost', 'User-Agent': 'wget/1.0'})[
            'status'
        ]
        == 404
    ), 'header split no match'

    # variable existence

    set_if("`${vars.arg_foo != '' ? 1 : 0}`")
    assert client.get(url='/?foo=1')['status'] == 200, 'variable exists'
    assert client.get(url='/?bar=1')['status'] == 404, 'variable missing'


def test_route_if_njs_exception(require, skip_alert):
    require({'modules': {'njs': 'any'}})
    skip_alert(r'js exception:')

    # A condition that passes validation but throws at run time cannot be
    # resolved to true or false, so the request fails instead of matching.

    set_if('`${uri.a.a}`')
    assert client.get()['status'] == 500, 'njs exception'

    # Negating it does not turn the error into a match: the expression is
    # evaluated before the negation is applied.

    set_if('!`${uri.a.a}`')
    assert client.get()['status'] == 500, 'negated njs exception'


def test_route_if_match():
    set_match({"method": "GET", "if": "$arg_foo"})
    assert client.get(url='/?foo=1')['status'] == 200, 'method and if'
    assert client.get(url='/')['status'] == 404, 'method and if, false'
    assert (
        client.post(url='/?foo=1', headers={'Host': 'localhost'})['status']
        == 404
    ), 'method and if, method mismatch'

    set_match({"uri": "/admin", "host": "localhost", "if": "!$arg_foo"})
    assert client.get(url='/admin')['status'] == 200, 'uri, host and if'
    assert client.get(url='/admin?foo=1')['status'] == 404, 'if false'
    assert client.get(url='/user')['status'] == 404, 'uri mismatch'

    set_match({"arguments": {"foo": "1"}, "if": "$arg_bar"})
    assert client.get(url='/?foo=1&bar=1')['status'] == 200, 'arguments and if'
    assert client.get(url='/?foo=2&bar=1')['status'] == 404, 'arguments mismatch'
    assert client.get(url='/?foo=1')['status'] == 404, 'if false'

    set_match({"headers": {"X-Test": "yes"}, "if": "$arg_foo"})
    assert (
        client.get(
            url='/?foo=1', headers={'Host': 'localhost', 'X-Test': 'yes'}
        )['status']
        == 200
    ), 'headers and if'
    assert (
        client.get(url='/?foo=1', headers={'Host': 'localhost'})['status'] == 404
    ), 'headers mismatch'

    # "if" only

    set_match({"if": "$arg_foo"})
    assert client.get(url='/?foo=1')['status'] == 200, 'if only'
    assert client.get(url='/')['status'] == 404, 'if only, false'
    assert (
        client.post(url='/?foo=1', headers={'Host': 'localhost'})['status']
        == 200
    ), 'if only, any method'


def test_route_if_fallthrough():
    assert 'success' in client.conf(
        [
            {"match": {"if": "$arg_foo"}, "action": {"return": 201}},
            {"match": {"if": "$arg_bar"}, "action": {"return": 202}},
            {"action": {"return": 203}},
        ],
        'routes',
    ), 'routes configure'

    assert client.get(url='/?foo=1')['status'] == 201, 'first route'
    assert client.get(url='/?bar=1')['status'] == 202, 'second route'
    assert client.get(url='/?baz=1')['status'] == 203, 'last route'


def test_route_if_invalid():
    assert 'error' in client.conf('"$arg_"', 'routes/0/match/if'), 'invalid var'
    assert 'error' in client.conf('"$"', 'routes/0/match/if'), 'empty var'
    assert 'error' in client.conf('1', 'routes/0/match/if'), 'not a string'
    assert 'error' in client.conf('["$arg_foo"]', 'routes/0/match/if'), 'array'

    # A leading "!" does not excuse an invalid condition: nxt_conf_vldt_if()
    # strips it and validates the rest.  This is rejected without the strip
    # too; the case that needs the strip is in test_route_if_invalid_njs().

    assert 'error' in client.conf(
        '"!$arg_"', 'routes/0/match/if'
    ), 'negated invalid var'


def test_route_if_invalid_njs(require):
    require({'modules': {'njs': 'any'}})

    assert 'error' in client.conf(
        '"`${args.foo`"', 'routes/0/match/if'
    ), 'invalid njs'

    # Accepting this one needs the "!" stripped: nxt_tstr_is_js() looks for
    # the backtick at offset 0, so an unstripped "!" would send the template
    # to the variable parser, which rejects "${".

    assert 'success' in client.conf(
        '"!`${args.foo}`"', 'routes/0/match/if'
    ), 'negated valid njs'
