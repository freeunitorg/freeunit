import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.option import option

prerequisites = {'modules': {'php': 'all'}}

client = ApplicationPHP()


def test_php_script_open_basedir():
    client.load(
        'script_open',
        options={
            'admin': {
                'open_basedir': '/nonexistent',
                # The 500 is PHP's fatal error status, and it reaches the
                # client only when PHP does not print the warning first.
                # A PHP without a php.ini has display_errors on.
                'display_errors': '0',
            }
        },
    )

    assert (
        client.get(url='/info.php')['status'] == 500
    ), 'script outside open_basedir'


def test_php_script_open_directory():
    client.load(
        'script_open',
        # As above, the status is PHP's and display_errors decides whether
        # it arrives.
        options={'admin': {'display_errors': '0'}},
    )

    assert client.get(url='/info.php')['status'] == 200, 'script'

    assert (
        client.get(url='/dir.php')['status'] == 500
    ), 'directory named like a script'


def test_php_script_open_missing():
    client.load('script_open')

    assert client.get(url='/missing.php')['status'] == 404, 'missing script'


@pytest.mark.skipif(
    option.system != 'Linux', reason='/proc/self/fd is Linux only'
)
def test_php_script_open_close_on_exec():
    client.load('script_open')

    resp = client.get(url='/fd.php')

    assert resp['status'] == 200, 'status'
    assert resp['body'] == 'visible', 'script descriptor in a child'


def test_php_script_open_prepend_order():
    client.load(
        'script_open',
        options={
            'admin': {
                'auto_prepend_file': (
                    f'{option.test_dir}/php/script_open/prep.php'
                )
            }
        },
    )

    resp = client.get(url='/info.php')

    assert resp['status'] == 200, 'status'
    assert resp['body'] == 'prep.php, info.php', 'prepend file runs first'
