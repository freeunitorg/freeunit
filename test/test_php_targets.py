import os
from pathlib import Path

from unit.applications.lang.php import ApplicationPHP
from unit.option import option
from unit.utils import waitforfiles

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()


def test_php_application_targets():
    targets_dir = f"{option.test_dir}/php/targets"
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "match": {"uri": "/1"},
                    "action": {"pass": "applications/targets/1"},
                },
                {
                    "match": {"uri": "/2"},
                    "action": {"pass": "applications/targets/2"},
                },
                {"action": {"pass": "applications/targets/default"}},
            ],
            "applications": {
                "targets": {
                    "type": client.get_application_type(),
                    "processes": {"spare": 0},
                    "targets": {
                        "1": {
                            "script": "1.php",
                            "root": targets_dir,
                        },
                        "2": {
                            "script": "2.php",
                            "root": f'{targets_dir}/2',
                        },
                        "default": {
                            "index": "index.php",
                            "root": targets_dir,
                        },
                    },
                }
            },
        }
    )

    assert client.get(url='/1')['body'] == '1'
    assert client.get(url='/2')['body'] == '2'
    assert client.get(url='/blah')['status'] == 404
    assert client.get(url='/')['body'] == 'index'
    assert client.get(url='/1.php?test=test.php/')['body'] == '1'

    assert 'success' in client.conf(
        "\"1.php\"", 'applications/targets/targets/default/index'
    ), 'change targets index'
    assert client.get(url='/')['body'] == '1'

    assert 'success' in client.conf_delete(
        'applications/targets/targets/default/index'
    ), 'remove targets index'
    assert client.get(url='/')['body'] == 'index'


def test_php_application_targets_cwd(temp_dir):
    # libunit fills a released request with 0xA5 (nxt_unit_mmap_release()).
    # The module read the target index after the request ended, so it kept
    # 0xA5 (165) as the last target.  The target at index 165 then skipped
    # chdir() and ran in the directory of the previous target.  The index
    # follows the hash order of the names, so every target gets its own
    # directory, and the test requests each of them.  libunit fills released
    # memory only in --debug builds, so this test fails on the old code only
    # there.  The test below does not depend on the fill.
    routes = []
    targets = {}

    for i in range(166):
        name = f't{i}'
        root = f'{temp_dir}/{name}'

        Path(root).mkdir()
        Path(f'{root}/index.php').write_text(
            '<?php echo getcwd();', encoding='utf-8'
        )

        routes.append(
            {
                "match": {"uri": f'/{name}'},
                "action": {"pass": f'applications/targets/{name}'},
            }
        )
        targets[name] = {"root": root, "script": "index.php"}

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": routes,
            "applications": {
                "targets": {
                    "type": client.get_application_type(),
                    "processes": 1,
                    "targets": targets,
                }
            },
        }
    )

    # Each request follows a request for another target.
    names = list(targets)

    for name in names[-1:] + names:
        cwd = str(Path(f'{temp_dir}/{name}').resolve())
        assert client.get(url=f'/{name}')['body'] == cwd, name


def test_php_application_targets_cwd_finish_request(temp_dir):
    # fastcgi_finish_request() ends the request while the script keeps
    # running.  The router then puts the next request into the released
    # memory.  The module read the target index after the script ended, so
    # it kept the index of that next request as the last target.  Its next
    # request for that target then skipped chdir().  The next request is
    # still running when the index is read, so libunit has not released or
    # filled that memory, and the test fails on the old code in release
    # builds too.  Signal files order the requests, so the test does not
    # depend on timing.
    sig = Path(f'{temp_dir}/sig')
    sig.mkdir()
    sig.chmod(0o777)

    script = """<?php
echo getcwd();

if (isset($_GET['finish'])) {
    fastcgi_finish_request();
}

if (isset($_GET['signal'])) {
    touch('SIG/' . $_GET['signal']);
}

if (isset($_GET['wait'])) {
    $end = microtime(true) + 10;

    while (!file_exists('SIG/' . $_GET['wait']) && microtime(true) < $end) {
        usleep(10000);
        clearstatcache();
    }
}
""".replace('SIG', str(sig))
    cwd = {}
    targets = {}

    for name in ('a', 'b'):
        root = f'{temp_dir}/{name}'

        Path(root).mkdir()
        Path(f'{root}/index.php').write_text(script, encoding='utf-8')

        cwd[name] = str(Path(root).resolve())
        targets[name] = {"root": root, "script": "index.php"}

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "match": {"uri": f'/{name}'},
                    "action": {"pass": f'applications/targets/{name}'},
                }
                for name in targets
            ],
            "applications": {
                "targets": {
                    "type": client.get_application_type(),
                    "processes": 2,
                    "targets": targets,
                }
            },
        }
    )

    # The first worker ends /a and releases its memory.  The script then
    # creates "a" and waits for "b".
    assert client.get(url='/a?finish&signal=a&wait=b')['body'] == cwd['a']
    assert waitforfiles(f'{sig}/a'), 'a released'

    # The second worker takes /b, in the memory /a released.  The script
    # creates "b" and holds the memory until the test creates "done".
    sock = client.get(url='/b?signal=b&wait=done', no_recv=True)
    assert waitforfiles(f'{sig}/b'), 'b running'

    # The script of /a now ends.  The first worker is the only free one, so
    # it takes this request.
    assert client.get(url='/b')['body'] == cwd['b'], 'b after a'

    Path(f'{sig}/done').touch()

    resp = client._resp_to_dict(client.recvall(sock).decode())
    sock.close()
    body = client._parse_chunked_body(resp['body']).decode()
    assert body == cwd['b'], 'b while a waits'


def test_php_application_targets_error():
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "applications/targets/default"}},
            "applications": {
                "targets": {
                    "type": client.get_application_type(),
                    "processes": {"spare": 0},
                    "targets": {
                        "default": {
                            "index": "index.php",
                            "root": f"{option.test_dir}/php/targets",
                        },
                    },
                }
            },
        }
    ), 'initial configuration'
    assert client.get()['status'] == 200

    assert 'error' in client.conf(
        {"pass": "applications/targets/blah"}, 'listeners/*:8080'
    ), 'invalid targets pass'
    assert 'error' in client.conf(
        f'"{option.test_dir}/php/targets"',
        'applications/targets/root',
    ), 'invalid root'
    assert 'error' in client.conf(
        '"index.php"', 'applications/targets/index'
    ), 'invalid index'
    assert 'error' in client.conf(
        '"index.php"', 'applications/targets/script'
    ), 'invalid script'
    assert 'error' in client.conf_delete(
        'applications/targets/default/root'
    ), 'root remove'


def test_php_application_index_nul():
    # "index" is appended to a directory and handed to open() as a
    # NUL-terminated C string, so an embedded NUL truncates the name at the
    # sink.  This validator is shared with the static "share" action
    # (test_static.py::test_static_index_nul); here it is exercised on both
    # PHP schemas that attach it: a target's "index" and the top-level
    # "index" used without targets.  "spare": 0 validates the configuration
    # without spawning a PHP worker.
    root = f"{option.test_dir}/php/targets"

    def conf_targets_index(index):
        return client.conf(
            {
                "listeners": {"*:8080": {"pass": "applications/targets"}},
                "applications": {
                    "targets": {
                        "type": client.get_application_type(),
                        "processes": {"spare": 0},
                        "targets": {
                            "default": {"index": index, "root": root},
                        },
                    }
                },
            }
        )

    def conf_notargets_index(index):
        return client.conf(
            {
                "listeners": {"*:8080": {"pass": "applications/notargets"}},
                "applications": {
                    "notargets": {
                        "type": client.get_application_type(),
                        "processes": {"spare": 0},
                        "root": root,
                        "index": index,
                    }
                },
            }
        )

    resp = conf_targets_index("index\0.php")
    assert (
        'must not contain null character' in resp.get('detail', '')
    ), 'targets index with null character'

    assert 'success' in conf_targets_index(
        "index.php"
    ), 'clean targets index still accepted'

    resp = conf_notargets_index("index\0.php")
    assert (
        'must not contain null character' in resp.get('detail', '')
    ), 'notargets index with null character'

    assert 'success' in conf_notargets_index(
        "index.php"
    ), 'clean notargets index still accepted'


def test_php_application_root_cwd(temp_dir):
    # The working directory of a root-only target must follow the script:
    # in the same directory, in another directory, and after a script that
    # called chdir().
    for d in ('a', 'b'):
        Path(f'{temp_dir}/{d}').mkdir()
        Path(f'{temp_dir}/{d}/cwd.php').write_text(
            '<?php echo getcwd() . "|" . file_get_contents("rel.txt");',
            encoding='utf-8',
        )
        Path(f'{temp_dir}/{d}/rel.txt').write_text(d, encoding='utf-8')
        Path(f'{temp_dir}/{d}/chdir.php').write_text(
            '<?php chdir("/"); echo getcwd();', encoding='utf-8'
        )

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "applications/root"}},
            "applications": {
                "root": {
                    "type": client.get_application_type(),
                    "processes": 1,
                    "root": temp_dir,
                }
            },
        }
    )

    def check(d):
        cwd = str(Path(f'{temp_dir}/{d}').resolve())
        assert client.get(url=f'/{d}/cwd.php')['body'] == f'{cwd}|{d}', d

    for d in ('a', 'a', 'b', 'b', 'a'):
        check(d)

    assert client.get(url='/a/chdir.php')['body'] == '/'
    check('a')

    assert client.get(url='/b/chdir.php')['body'] == '/'
    check('b')
    check('a')


def test_php_application_root_symlink_swap(temp_dir):
    # A deploy can replace a script directory at the same path, for example
    # by a rename of a new symlink over the old one.  The next request must
    # run in the new directory, not in the old one.  The symlink is below
    # "root", because the module resolves "root" itself only at startup.
    for d in ('A', 'B'):
        Path(f'{temp_dir}/{d}').mkdir()
        Path(f'{temp_dir}/{d}/cwd.php').write_text(
            '<?php echo getcwd() . "|" . file_get_contents("rel.txt");',
            encoding='utf-8',
        )
        Path(f'{temp_dir}/{d}/rel.txt').write_text(d, encoding='utf-8')

    def check(d):
        cwd = str(Path(f'{temp_dir}/{d}').resolve())
        assert client.get(url='/current/cwd.php')['body'] == f'{cwd}|{d}', d

    os.symlink(f'{temp_dir}/A', f'{temp_dir}/current')

    try:
        assert 'success' in client.conf(
            {
                "listeners": {"*:8080": {"pass": "applications/root"}},
                "applications": {
                    "root": {
                        "type": client.get_application_type(),
                        "processes": 1,
                        "root": temp_dir,
                    }
                },
            }
        )

        check('A')
        check('A')

        os.symlink(f'{temp_dir}/B', f'{temp_dir}/next')
        os.rename(f'{temp_dir}/next', f'{temp_dir}/current')

        check('B')
        check('B')

    finally:
        # The temp_dir cleanup does not handle a symlink.
        for link in ('current', 'next'):
            if os.path.islink(f'{temp_dir}/{link}'):
                os.unlink(f'{temp_dir}/{link}')
