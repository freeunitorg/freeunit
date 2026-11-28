"""In a "rewrite" template the value of a variable is data.

The literal text of the template and "$request_uri" are read as an encoded
URI.  The value of every other variable, such as "$uri", reaches the new
path as it is: it is not percent-decoded again, and a "?" or "#" in it does
not end the path.  So the action gets the path that the route rules
compared, with the text of the template around it.  Each test sends every
request first and prints what it saw (run with -s), then asserts.
"""

from unit.applications.lang.python import ApplicationPython
from unit.option import option

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

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


def test_python_rewrite_uri_pass(findall):
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "match": {"uri": "/v1/*"},
                    "action": {
                        "rewrite": "/app$uri",
                        "pass": "applications/prefix",
                    },
                }
            ],
            "applications": {
                "prefix": {
                    "type": client.get_application_type(),
                    "processes": {"spare": 0},
                    "path": f'{option.test_dir}/python/prefix',
                    "working_directory": f'{option.test_dir}/python/prefix',
                    "module": "wsgi",
                }
            },
            "settings": {"http": {"log_route": True}},
        }
    ), 'configuration'

    ok = send(findall, '/v1/x')
    dot = send(findall, '/v1/%252e%252e/%252e%252e/x')
    qmark = send(findall, '/v1/a%3Fb')
    pct = send(findall, '/v1/100%25')

    assert ok['status'] == 200 and ok['path_info'] == '/app/v1/x'

    assert dot['path_info'] == '/app/v1/%2e%2e/%2e%2e/x'
    assert qmark['path_info'] == '/app/v1/a?b'
    assert pct['status'] == 200 and pct['path_info'] == '/app/v1/100%'
