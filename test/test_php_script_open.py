import fcntl
import json
import os
import re
import shutil
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.option import option

prerequisites = {'modules': {'php': 'all'}}

client = ApplicationPHP()

# ApplicationPHP.load() sets the root to this directory.
script_open_dir = f'{option.test_dir}/php/script_open'


@pytest.fixture
def script_tree(temp_dir):
    root = f'{temp_dir}/script_open'
    shutil.copytree(script_open_dir, root)

    return root


def load_script_tree(root, script=None, **kwargs):
    client.load('script_open', **kwargs)
    assert 'success' in client.conf(
        json.dumps(root), 'applications/script_open/root'
    )
    assert 'success' in client.conf(
        json.dumps(root), 'applications/script_open/working_directory'
    )
    if script is not None:
        assert 'success' in client.conf(
            json.dumps(script), 'applications/script_open/script'
        )


def test_php_script_open_basedir():
    client.load(
        'script_open',
        options={
            'admin': {
                'open_basedir': '/nonexistent',
                'display_errors': '0',
            }
        },
    )

    assert (
        client.get(url='/info.php')['status'] == 403
    ), 'script outside open_basedir'


def test_php_script_open_basedir_display_errors_on():
    client.load(
        'script_open',
        options={
            'admin': {
                'open_basedir': '/nonexistent',
                # display_errors is on, as in a PHP with no php.ini.  If PHP
                # opened the script, it would print the warning below into
                # the body and the status would be 200.
                'display_errors': '1',
            }
        },
    )

    resp = client.get(url='/info.php')

    assert resp['status'] == 403, 'script outside open_basedir'
    assert resp['body'] == '', 'no PHP diagnostics'


def test_php_script_open_basedir_allowed():
    # A control, green before and after the change: the check must not refuse
    # a script inside the configured path.
    client.load(
        'script_open',
        options={'admin': {'open_basedir': script_open_dir}},
    )

    assert (
        client.get(url='/info.php')['status'] == 200
    ), 'script inside open_basedir'


def test_php_script_open_directory():
    client.load(
        'script_open',
        options={'admin': {'display_errors': '0'}},
    )

    assert client.get(url='/info.php')['status'] == 200, 'script'

    assert (
        client.get(url='/dir.php')['status'] == 404
    ), 'directory named like a script'


def test_php_script_open_directory_display_errors_on():
    client.load(
        'script_open',
        # As above, and PHP's answer would carry the warning in the body.
        options={'admin': {'display_errors': '1'}},
    )

    resp = client.get(url='/dir.php')

    assert resp['status'] == 404, 'directory named like a script'
    assert resp['body'] == '', 'no PHP diagnostics'


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


def test_php_script_open_relative_basedir(script_tree):
    Path(f'{script_tree}/sub').mkdir()
    Path(f'{script_tree}/sub/info.php').write_text('<?php echo "sub";')
    load_script_tree(
        script_tree,
        processes={'spare': 1, 'max': 1},
        options={'admin': {'open_basedir': '.'}},
    )

    for url, body in (
        ('/sub/info.php', 'sub'),
        ('/info.php', 'info.php'),
        ('/info.php', 'info.php'),
        ('/sub/info.php', 'sub'),
    ):
        resp = client.get(url=url)
        assert resp['status'] == 200, (url, resp)
        assert resp['body'] == body, (url, resp)


def test_php_script_open_symlink(script_tree):
    Path(f'{script_tree}/link.php').symlink_to('info.php')
    load_script_tree(script_tree)

    resp = client.get(url='/link.php')
    assert resp['status'] == 200, resp
    assert resp['body'] == 'info.php', resp


def test_php_script_open_long_path():
    client.load('script_open')
    # Each component is short; the full path exceeds PHP's MAXPATHLEN.
    resp = client.get(url='/' + 'a/' * 2050 + 'info.php')

    assert resp['status'] == 404, resp
    assert resp['body'] == '', resp


@pytest.mark.skipif(
    option.system != 'Linux' or os.geteuid() != 0,
    reason='requires Linux /proc/locks and root for the worker lock',
)
def test_php_script_open_fifo_replaced_after_stat(script_tree):
    script = f'{script_tree}/info.php'
    session_dir = f'{script_tree}/sessions'
    Path(session_dir).mkdir(mode=0o777)
    os.chmod(session_dir, 0o777)
    session = f'{session_dir}/sess_scriptopen'

    # Session auto-start waits during RINIT, after Unit's stat and before
    # the script open.  A blocked flock in /proc/locks proves that phase
    # has been reached; no timing assumption controls the replacement.
    with open(session, 'w') as lock:
        os.chmod(session, 0o666)
        fcntl.flock(lock, fcntl.LOCK_EX)
        inode = os.fstat(lock.fileno()).st_ino
        load_script_tree(
            script_tree,
            options={
                'admin': {
                    'session.auto_start': '1',
                    'session.save_handler': 'files',
                    'session.save_path': session_dir,
                    'display_errors': '1',
                }
            },
        )

        with ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(
                client.get,
                url='/info.php',
                headers={'Cookie': 'PHPSESSID=scriptopen'},
                read_timeout=10,
            )
            try:
                blocked = False
                for _ in range(100):
                    locks = Path('/proc/locks').read_text()
                    if re.search(rf'-> FLOCK .*:[0]*{inode} ', locks):
                        blocked = True
                        break
                    if future.done():
                        break
                    time.sleep(0.05)

                assert blocked, 'PHP session startup is waiting for the lock'
                fifo = f'{script_tree}/replacement'
                os.mkfifo(fifo)
                os.replace(fifo, script)
                fcntl.flock(lock, fcntl.LOCK_UN)

                resp = future.result(timeout=15)
                assert resp.get('status') == 404, resp
                assert resp.get('body') == '', resp
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
                # Release a blocking pre-fix open so teardown stays usable.
                if Path(script).is_fifo():
                    fd = os.open(script, os.O_RDWR | os.O_NONBLOCK)
                    os.close(fd)


def test_php_script_open_static_script_basedir_display_errors_on(script_tree):
    load_script_tree(
        script_tree,
        script='info.php',
        options={
            'admin': {
                'open_basedir': '/nonexistent',
                'display_errors': '1',
            }
        },
    )

    resp = client.get(url='/')

    assert resp['status'] == 403, 'static script outside open_basedir'
    assert resp['body'] == '', 'no PHP diagnostics'


def test_php_script_open_static_script_basedir_allowed(script_tree):
    # A control for the denial test above, with "open_basedir" set to the
    # tree: an inverted check would refuse the fixed script inside it.
    load_script_tree(
        script_tree,
        script='info.php',
        options={'admin': {'open_basedir': script_tree}},
    )

    resp = client.get(url='/')

    assert resp['status'] == 200, 'static script inside open_basedir'
    assert resp['body'] == 'info.php', resp


def test_php_script_open_static_script_unlinked(script_tree):
    script = Path(f'{script_tree}/info.php')
    load_script_tree(
        script_tree,
        script='info.php',
        processes={'spare': 1, 'max': 1},
        # display_errors is on, as in a PHP with no php.ini.  If PHP opened
        # the fixed script itself, a removed script would answer 200 with
        # its path in the body.  OPcache is off so that the module's open,
        # and not the cache, answers.
        options={'admin': {'display_errors': '1', 'opcache.enable': '0'}},
    )

    resp = client.get(url='/')
    assert resp['status'] == 200, resp
    assert resp['body'] == 'info.php', resp

    script.unlink()

    resp = client.get(url='/')
    assert resp['status'] == 404, resp
    assert resp['body'] == '', resp


def test_php_script_open_static_script_directory(script_tree):
    # A fixed target has no per-request stat(), so the opened descriptor is
    # the only check that refuses a directory at the script path.
    script = Path(f'{script_tree}/info.php')
    load_script_tree(
        script_tree,
        script='info.php',
        processes={'spare': 1, 'max': 1},
        options={'admin': {'display_errors': '1', 'opcache.enable': '0'}},
    )

    resp = client.get(url='/')
    assert resp['status'] == 200, resp

    script.unlink()
    script.mkdir()

    resp = client.get(url='/')
    assert resp['status'] == 404, resp
    assert resp['body'] == '', resp


def test_php_script_open_static_script_opcache(script_tree):
    script = Path(f'{script_tree}/info.php')

    def write_script(body):
        script.write_text(
            '<?php\n'
            'header("X-OPcache: " . (int) ('
            'function_exists("opcache_is_script_cached") '
            '&& opcache_is_script_cached(__FILE__)));\n'
            f'echo "{body}";\n',
            encoding='utf-8',
        )

    write_script('first')

    load_script_tree(
        script_tree,
        script='info.php',
        processes={'spare': 1, 'max': 1},
        options={
            'admin': {
                'display_errors': '1',
                'opcache.validate_timestamps': '0',
                'opcache.file_update_protection': '0',
            }
        },
    )

    resp = client.get(url='/')
    assert resp['status'] == 200, resp
    assert resp['body'] == 'first', resp

    if resp['headers'].get('X-OPcache') != '1':
        pytest.skip('opcache is not enabled')

    # The stream the module opens does not defeat OPcache: with timestamps
    # not validated, the compiled script is served and the new content is
    # not read.
    write_script('second')

    resp = client.get(url='/')
    assert resp['status'] == 200, resp
    assert resp['body'] == 'first', resp

    # The file still governs: the script is opened on every request, so a
    # removed script is refused although OPcache holds its compiled form.
    script.unlink()

    resp = client.get(url='/')
    assert resp['status'] == 404, resp
    assert resp['body'] == '', resp


# Keep the FIFO parking regressions last: the pre-fix code blocks in open().
def test_php_script_open_z_static_script_fifo_replaced(script_tree):
    script = f'{script_tree}/info.php'
    load_script_tree(
        script_tree,
        script='info.php',
        processes={'spare': 1, 'max': 1},
        options={'admin': {'display_errors': '1', 'opcache.enable': '0'}},
    )

    resp = client.get(url='/')
    assert resp['status'] == 200, resp

    fifo = f'{script_tree}/replacement'
    os.mkfifo(fifo)
    os.replace(fifo, script)

    try:
        resp = client.get(url='/', read_timeout=2)

        assert resp.get('status') == 404, resp
        assert resp.get('body') == '', resp
    finally:
        # Release a blocking pre-fix open so teardown stays usable.
        if Path(script).is_fifo():
            fd = os.open(script, os.O_RDWR | os.O_NONBLOCK)
            os.close(fd)


def test_php_script_open_z_fifo(script_tree):
    os.mkfifo(f'{script_tree}/fifo.php')
    load_script_tree(
        script_tree,
        options={'admin': {'display_errors': '1'}},
    )

    resp = client.get(url='/fifo.php', read_timeout=2)

    assert resp.get('status') == 404, resp
    assert resp.get('body') == '', resp
