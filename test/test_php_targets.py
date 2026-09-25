from unit.applications.lang.php import ApplicationPHP
from unit.option import option

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
