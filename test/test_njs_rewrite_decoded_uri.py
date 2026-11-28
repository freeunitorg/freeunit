"""An njs "rewrite" or "pass" result is data: it is not percent-decoded.

An njs template gives one string.  Its literal text cannot be told apart
from the values in it, so the whole string is percent-encoded before the
result is parsed.  The parse gives the string back as it is.  A rewrite
path is then normalized as before.  A "pass" result names a target by the
string as it is, so it cannot name one whose name holds a slash.
"""

import pytest

from unit.applications.proto import ApplicationProto

prerequisites = {'modules': {'njs': 'any'}}

client = ApplicationProto()

REWRITTEN = r'\[notice\].*URI rewritten to "(.*)"'

# Each case: template, request target, expected status, the path that the
# "URI rewritten to" line names (None: no such line).
CASES = [
    ('`/${host}`', '/', 200, '/localhost'),
    ('`/app${uri}`', '/v1/pub.txt', 200, '/app/v1/pub.txt'),
    ('`/app${uri}`', '/v1/a%2Fb', 200, '/app/v1/a/b'),
    # "$uri" is decoded once only.
    ('`/app${uri}`', '/v1/a%3Fb', 200, '/app/v1/a?b'),
    ('`/app${uri}`', '/v1/100%25', 200, '/app/v1/100%'),
    (
        '`/app${uri}`',
        '/v1/%252e%252e/%252e%252e/x',
        200,
        '/app/v1/%2e%2e/%2e%2e/x',
    ),
    # The literal text is data too.
    ('`/new?drop`', '/', 200, '/new?drop'),
    ('`/a%41`', '/', 200, '/a%41'),
    # The raw target is data as well: it is not decoded.
    (
        '`/index.php${vars.request_uri}`',
        '/a%20b/c?x=1',
        200,
        '/index.php/a%20b/c?x=1',
    ),
    # A NUL byte in the result is refused.
    ('`/files/${args.x}`', '/?x=a%00b', 500, None),
    # A ".." segment is resolved, as before.
    ('`/files/${args.x}`', '/?x=..%2Fx', 200, '/x'),
    ('`/files/${args.x}`', '/?x=..%2F..%2Fx', 500, None),
]


@pytest.mark.parametrize('template, url, status, path', CASES)
def test_njs_rewrite_result_is_data(findall, template, url, status, path):
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"rewrite": template, "return": 200}}],
            "applications": {},
            "settings": {"http": {"log_route": True}},
        }
    ), 'configuration'

    before = len(findall(REWRITTEN))
    resp = client.get(url=url)
    found = findall(REWRITTEN)

    assert resp['status'] == status, 'status'
    assert (found[-1] if len(found) > before else None) == path, 'path'


@pytest.mark.parametrize(
    'pass_, host, status',
    [
        ('`routes/${host}`', 'blog', 200),
        ('`routes/${host}`', 'admin', 403),
        ('`routes/${host}`', '%61dmin', 404),
        ('`routes/%61dmin`', 'localhost', 404),
    ],
)
def test_njs_pass_result_is_data(pass_, host, status):
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes/main"}},
            "routes": {
                "main": [{"action": {"pass": pass_}}],
                "blog": [{"action": {"return": 200}}],
                "admin": [{"action": {"return": 403}}],
            },
            "applications": {},
        }
    ), 'configuration'

    resp = client.get(headers={'Host': host, 'Connection': 'close'})

    assert resp['status'] == status, 'status'
