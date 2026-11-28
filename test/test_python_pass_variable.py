"""In a "pass" template the value of a variable is data.

"$uri" is decoded once by the request line parse, and "$host" is the Host
header in lower case, without the port, and not decoded.  The route rules
compare these values.  A
"pass" template that inserts them names the application by the same value:
it is not percent-decoded a second time.
"""

import pytest

from unit.applications.lang.python import ApplicationPython
from unit.option import option

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()


def app(name):
    return {
        "type": client.get_application_type(),
        "processes": {"spare": 0},
        "path": f'{option.test_dir}/python/environment',
        "working_directory": f'{option.test_dir}/python/environment',
        "module": "wsgi",
        "environment": {"APP": name},
    }


def configure(rule, pass_):
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {"match": rule, "action": {"return": 403}},
                {"action": {"pass": pass_}},
            ],
            "applications": {"blog": app("blog"), "admin": app("admin")},
        }
    ), 'configuration'


def send(url, host='localhost'):
    resp = client.get(
        url=url,
        headers={'Host': host, 'X-Variables': 'APP', 'Connection': 'close'},
    )
    seen = (url, host, resp['status'], resp['body'])
    print(f'seen {seen}')
    return seen


def test_python_pass_uri_value():
    configure({"uri": "/admin*"}, "applications$uri")

    blog = send('/blog')
    admin = send('/admin')
    twice = send('/%2561dmin')

    assert blog[2:] == (200, 'blog')
    assert admin[2] == 403
    assert twice[2] == 404, 'no application named "%61dmin"'


def test_python_pass_host_value():
    configure({"host": "admin"}, "applications/$host")

    blog = send('/', 'blog')
    admin = send('/', 'admin')
    once = send('/', '%61dmin')

    assert blog[2:] == (200, 'blog')
    assert admin[2] == 403
    assert once[2] == 404, 'no application named "%61dmin"'


@pytest.mark.parametrize(
    'pass_, url, status, body',
    [
        # A value with no byte to encode selects as before.
        ('applications$uri', '/blog', 200, 'blog'),
        ('applications/$arg_a', '/?a=admin', 200, 'admin'),
        ('applications/$arg_a', '/?a=%2561dmin', 404, None),
        # The literal text is read as an encoded name, as before.
        ('applications/%61dmin', '/', 200, 'admin'),
    ],
)
def test_python_pass_value_is_data(pass_, url, status, body):
    configure({"uri": "/nothing"}, pass_)

    seen = send(url)

    assert seen[2] == status
    if body is not None:
        assert seen[3] == body
