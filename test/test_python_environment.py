from unit.applications.lang.python import ApplicationPython

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()


def test_python_environment_name_null():
    client.load('environment')

    assert 'error' in client.conf(
        {"va\0r": "val1"}, 'applications/environment/environment'
    ), 'name null'


def test_python_environment_name_equals():
    client.load('environment')

    assert 'error' in client.conf(
        {"var=": "val1"}, 'applications/environment/environment'
    ), 'name equals'


def test_python_environment_value_null():
    client.load('environment')

    assert 'error' in client.conf(
        {"var": "\0val"}, 'applications/environment/environment'
    ), 'value null'


def test_python_environment_update():
    client.load('environment')

    client.conf({"var": "val1"}, 'applications/environment/environment')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'X-Variables': 'var',
                'Connection': 'close',
            }
        )['body']
        == 'val1'
    ), 'set'

    client.conf({"var": "val2"}, 'applications/environment/environment')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'X-Variables': 'var',
                'Connection': 'close',
            }
        )['body']
        == 'val2'
    ), 'update'


def test_python_environment_replace():
    client.load('environment')

    client.conf({"var1": "val1"}, 'applications/environment/environment')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'X-Variables': 'var1',
                'Connection': 'close',
            }
        )['body']
        == 'val1'
    ), 'set'

    client.conf({"var2": "val2"}, 'applications/environment/environment')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'X-Variables': 'var1,var2',
                'Connection': 'close',
            }
        )['body']
        == 'val2'
    ), 'replace'


def test_python_environment_clear():
    client.load('environment')

    client.conf(
        {"var1": "val1", "var2": "val2"},
        'applications/environment/environment',
    )

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'X-Variables': 'var1,var2',
                'Connection': 'close',
            }
        )['body']
        == 'val1,val2'
    ), 'set'

    client.conf({}, 'applications/environment/environment')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'X-Variables': 'var1,var2',
                'Connection': 'close',
            }
        )['body']
        == ''
    ), 'clear'


def test_python_environment_replace_default():
    client.load('environment')

    home_default = client.get(
        headers={
            'Host': 'localhost',
            'X-Variables': 'HOME',
            'Connection': 'close',
        }
    )['body']

    assert len(home_default) > 1, 'get default'

    client.conf({"HOME": "/"}, 'applications/environment/environment')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'X-Variables': 'HOME',
                'Connection': 'close',
            }
        )['body']
        == '/'
    ), 'replace default'

    client.conf({}, 'applications/environment/environment')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'X-Variables': 'HOME',
                'Connection': 'close',
            }
        )['body']
        == home_default
    ), 'restore default'


def test_python_environment_executable_no_path():
    # With no "home", the module starts Python from the interpreter it was
    # built against, not from a PATH search: with no Python in the PATH,
    # sys.executable is still a working interpreter.
    client.load('executable', environment={'PATH': '/nonexistent'})

    resp = client.get()
    assert resp['status'] == 200, 'status'
    assert resp['headers']['X-Executable'].startswith('/'), 'absolute'
    assert resp['headers']['X-Executable-Ok'] == 'True', 'executable'
