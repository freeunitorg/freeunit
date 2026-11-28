"""In a "rewrite" template the value of a variable is data.

The literal text of the template and "$request_uri" are read as an encoded
URI.  The value of every other variable, such as "$uri", reaches the new
path as it is: it is not percent-decoded again, and a "?" or "#" in it does
not end the path.  So the action gets the path that the route rules
compared, with the text of the template around it.  Each test sends every
request first and prints what it saw (run with -s), then asserts.
"""

from pathlib import Path

import pytest

from unit.applications.proto import ApplicationProto

client = ApplicationProto()

REWRITTEN = r'\[notice\].*URI rewritten to "(.*)"'


def last_rewrite(findall, before):
    found = findall(REWRITTEN)
    return found[-1] if len(found) > before else None


def send(findall, url):
    before = len(findall(REWRITTEN))
    resp = client.get(url=url)

    seen = {
        'url': url,
        'status': resp['status'],
        'body': resp['body'],
        'rewritten': last_rewrite(findall, before),
        'path_info': resp['headers'].get('Path-Info'),
    }
    print(f'seen {seen}')
    return seen


def test_rewrite_uri_share(temp_dir, findall):
    root = Path(f'{temp_dir}/root')

    # What the first route is meant to expose: /app/v1/...
    (root / 'app/v1').mkdir(parents=True)
    (root / 'app/v1/pub.txt').write_text('pub', encoding='utf-8')
    # Under the share root, outside the /app/v1 prefix.
    (root / 'outside.txt').write_text('outside', encoding='utf-8')

    # The second route exposes *.txt only.
    static = root / 'files/static'
    static.mkdir(parents=True)
    (static / 'plain.txt').write_text('plain', encoding='utf-8')
    (static / 'name.html').write_text('html', encoding='utf-8')
    (static / 'name.html?.txt').write_text('qfile', encoding='utf-8')
    (static / 'name.html#.txt').write_text('hfile', encoding='utf-8')
    (static / '100%.txt').write_text('percent', encoding='utf-8')

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "match": {"uri": "/v1/*"},
                    "action": {
                        "rewrite": "/app$uri",
                        "share": f'{root}$uri',
                    },
                },
                {
                    "match": {"uri": "/static/*.txt"},
                    "action": {
                        "rewrite": "/files$uri",
                        "share": f'{root}$uri',
                    },
                },
            ],
            "applications": {},
            "settings": {"http": {"log_route": True}},
        }
    ), 'configuration'

    pub = send(findall, '/v1/pub.txt')
    dot = send(findall, '/v1/%252e%252e/%252e%252e/outside.txt')
    dot1 = send(findall, '/v1/%2e%2e/%2e%2e/outside.txt')
    plain = send(findall, '/static/plain.txt')
    html = send(findall, '/static/name.html')
    qmark = send(findall, '/static/name.html%3F.txt')
    hash_ = send(findall, '/static/name.html%23.txt')
    pct = send(findall, '/static/100%25.txt')

    # Controls.
    assert pub['status'] == 200 and pub['body'] == 'pub'
    assert plain['status'] == 200 and plain['body'] == 'plain'
    assert html['status'] == 404, 'the *.txt rule refuses .html'
    assert dot1['status'] == 400, 'one encoding level is resolved at parse'

    # A path that the client encoded twice stays under the prefix.
    assert dot['rewritten'] == '/app/v1/%2e%2e/%2e%2e/outside.txt'
    assert dot['body'] != 'outside'

    # An encoded "?" or "#" is part of the file name.
    assert qmark['rewritten'] == '/files/static/name.html?.txt'
    assert qmark['body'] == 'qfile'
    assert hash_['rewritten'] == '/files/static/name.html#.txt'
    assert hash_['body'] == 'hfile'

    # A "%" in the decoded path is part of the file name.
    assert pct['status'] == 200 and pct['body'] == 'percent'


# The whole result is normalized as before: slashes are merged and "." and
# ".." segments are resolved, those of a value too.
#
# Each case: template, request target, request headers (None: Host only),
# expected status, the path that the "URI rewritten to" line names (None: no
# such line).
EDGE_CASES = [
    # A value decoded once stays as it is.
    ('/app$uri', '/v1/pub.txt', None, 200, '/app/v1/pub.txt'),
    ('/app$uri', '/v1/a%20b', None, 200, '/app/v1/a b'),
    ('/app$uri', '/v1/a%2Fb', None, 200, '/app/v1/a/b'),
    ('/app$uri', '/v1/%C3%A9', None, 200, '/app/v1/é'),
    # A "%XX" inside a value is not decoded again.
    ('/app$uri', '/v1/a%2541', None, 200, '/app/v1/a%41'),
    ('/app$uri', '/v1/a%253Fb', None, 200, '/app/v1/a%3Fb'),
    ('/app$uri', '/v1/a%23b', None, 200, '/app/v1/a#b'),
    # A header or cookie value is data.
    (
        '/sites/$host/x',
        '/',
        {'Host': 'a%2f%2e%2e%2f%2e%2e'},
        200,
        '/sites/a%2f%2e%2e%2f%2e%2e/x',
    ),
    (
        '$header_x_original_uri',
        '/',
        {'X-Original-URI': '/a%20b/c?x=1'},
        200,
        '/a%20b/c?x=1',
    ),
    (
        '/c/$cookie_x',
        '/',
        {'Cookie': 'x=a%2Fb%3Fc'},
        200,
        '/c/a%2Fb%3Fc',
    ),
    # An argument value is data: "?" does not end the path.
    (
        '/files/$arg_x',
        '/?x=name.html%3F.txt',
        None,
        200,
        '/files/name.html?.txt',
    ),
    ('/files/$arg_x', '/?x=%252e%252e%2Fx', None, 200, '/files/%2e%2e/x'),
    # A ".." segment from a value is resolved, as before.
    ('/files/$arg_x', '/?x=..%2Fx', None, 200, '/x'),
    ('/files/$arg_x', '/?x=..%2F..%2Fx', None, 500, None),
    ('/files/$arg_x', '/', None, 200, '/files/'),
    # A NUL byte in a value is refused.
    ('/files/$arg_x', '/?x=a%00b', None, 500, None),
    # Literal text and "$request_uri" are still decoded.
    ('${request_uri}o%6F', '/%66', None, 200, '/foo'),
    ('/new?some', '/', None, 200, '/new'),
    ('/foo%2Ffoo', '/', None, 200, '/foo/foo'),
    ('/index.php$request_uri', '/a%20b/c?x=1', None, 200, '/index.php/a b/c'),
    # Two values side by side stay apart.
    ('/f/$arg_x$arg_y', '/?x=%25&y=41', None, 200, '/f/%41'),
    # A literal "%" joins the value that follows it.
    ('/a%$arg_x', '/?x=41', None, 200, '/aA'),
]


@pytest.mark.parametrize('template, url, headers, status, path', EDGE_CASES)
def test_rewrite_value_is_data(findall, template, url, headers, status, path):
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"rewrite": template, "return": 200}}],
            "applications": {},
            "settings": {"http": {"log_route": True}},
        }
    ), 'configuration'

    before = len(findall(REWRITTEN))
    sent = {'Host': 'localhost', 'Connection': 'close', **(headers or {})}
    resp = client.get(url=url, headers=sent)

    assert resp['status'] == status, 'status'
    assert last_rewrite(findall, before) == path, 'rewritten path'
