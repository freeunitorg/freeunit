from unit.applications.lang.python import ApplicationPython
from unit.option import option

prerequisites = {'modules': {'python': 'all'}}

client = ApplicationPython()


def test_python_targets():
    python_dir = f'{option.test_dir}/python'

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
            ],
            "applications": {
                "targets": {
                    "type": client.get_application_type(),
                    "working_directory": f'{python_dir}/targets/',
                    "path": f'{python_dir}/targets/',
                    "targets": {
                        "1": {
                            "module": "wsgi",
                            "callable": "wsgi_target_a",
                        },
                        "2": {
                            "module": "wsgi",
                            "callable": "wsgi_target_b",
                        },
                    },
                }
            },
        }
    )

    resp = client.get(url='/1')
    assert resp['status'] == 200
    assert resp['body'] == '1'

    resp = client.get(url='/2')
    assert resp['status'] == 200
    assert resp['body'] == '2'


def test_python_targets_prefix():
    python_dir = f'{option.test_dir}/python'

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "match": {"uri": ["/app*"]},
                    "action": {"pass": "applications/targets/app"},
                },
                {
                    "match": {"uri": "*"},
                    "action": {"pass": "applications/targets/catchall"},
                },
            ],
            "applications": {
                "targets": {
                    "type": "python",
                    "working_directory": f'{python_dir}/targets/',
                    "path": f'{python_dir}/targets/',
                    "protocol": "wsgi",
                    "targets": {
                        "app": {
                            "module": "wsgi",
                            "callable": "wsgi_target_prefix",
                            "prefix": "/app/",
                        },
                        "catchall": {
                            "module": "wsgi",
                            "callable": "wsgi_target_prefix",
                            "prefix": "/api",
                        },
                    },
                }
            },
        }
    )

    def check_prefix(url, body):
        resp = client.get(url=url)
        assert resp['status'] == 200
        assert resp['body'] == body

    check_prefix('/app', '/app ')
    check_prefix('/app/', '/app /')
    check_prefix('/app/rest/user/', '/app /rest/user/')
    check_prefix('/catchall', 'No Script Name /catchall')
    check_prefix('/api', '/api ')
    check_prefix('/api/', '/api /')
    check_prefix('/apis', 'No Script Name /apis')
    check_prefix('/api/users/', '/api /users/')


def applications_conf(apps):
    python_dir = f'{option.test_dir}/python/targets/'

    applications = {}

    for name, targets in apps.items():
        app = {
            "type": client.get_application_type(),
            "working_directory": python_dir,
            "path": python_dir,
        }

        if targets:
            app["targets"] = {
                "1": {"module": "wsgi", "callable": "wsgi_target_a"},
                "2": {"module": "wsgi", "callable": "wsgi_target_b"},
            }

        else:
            app["module"] = "wsgi"
            app["callable"] = "wsgi_target_b"

        applications[name] = app

    return applications


def conf_variable_pass(apps, routes=()):
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "match": {"uri": "/targets"},
                    "action": {"pass": "applications/targets/$arg_t"},
                },
                {
                    "match": {"uri": "/plain"},
                    "action": {"pass": "applications/plain/$arg_t"},
                },
                *routes,
            ],
            "applications": applications_conf(apps),
        }
    )


def test_python_targets_lookup_bound(findall):
    conf_variable_pass({"targets": True, "plain": False})

    # A target that the application does not have: before the fix the
    # lookup read past the list of targets.

    for t in ['missing', 'Missing', '3', '11', '%33']:
        resp = client.get(url=f'/targets?t={t}')
        assert resp['status'] == 404, f'missing target {t}'

    # "%2F" in the argument is decoded to "/" before the substitution, so
    # the pass gets one more segment.  nxt_http_pass_segments() declines it
    # before the target lookup.

    resp = client.get(url='/targets?t=a%2Fb')
    assert resp['status'] == 404, 'extra pass segment'

    # Target names are compared exactly and after the arguments are
    # decoded, so "%31" is the target "1".

    resp = client.get(url='/targets?t=%31')
    assert resp['status'] == 200, 'decoded target'
    assert resp['body'] == '1'

    # The value of $arg_t is decoded once, when the query is parsed.  The
    # pass keeps the value as it is, so "%2531" is the target "%31", which
    # the application does not have.

    resp = client.get(url='/targets?t=%2531')
    assert resp['status'] == 404, 'target decoded once'

    resp = client.get(url='/targets?t=2')
    assert resp['status'] == 200, 'known target after misses'
    assert resp['body'] == '2'

    # An empty variable leaves an empty last segment in the pass, which
    # nxt_http_pass_segments() declines, so it gives 404 and does not select
    # the first target.

    resp = client.get(url='/targets?t=')
    assert resp['status'] == 404, 'empty target'

    resp = client.get(url='/targets')
    assert resp['status'] == 404, 'no target argument'

    # An application without "targets": before the fix a non-empty target
    # dereferenced NULL.

    resp = client.get(url='/plain?t=x')
    assert resp['status'] == 404, 'target of an application without targets'

    resp = client.get(url='/plain?t=')
    assert resp['status'] == 404, 'empty target, no targets'

    # A second configuration changes the routes and keeps the applications.
    # nxt_router_conf_create() then reuses the applications of the previous
    # configuration, with their lists of targets.  The lookup must give the
    # same results on that path.

    conf_variable_pass(
        {"targets": True, "plain": False},
        [
            {
                "match": {"uri": "/const"},
                "action": {"pass": "applications/targets/2"},
            }
        ],
    )

    # No application process was started again, so the router reused the
    # applications.

    assert len(findall(r'"targets" prototype started')) == 1, 'targets reused'
    assert len(findall(r'"plain" prototype started')) == 1, 'plain reused'

    resp = client.get(url='/targets?t=1')
    assert resp['status'] == 200, 'known target, reused application'
    assert resp['body'] == '1'

    resp = client.get(url='/targets?t=3')
    assert resp['status'] == 404, 'missing target, reused application'

    resp = client.get(url='/plain?t=x')
    assert resp['status'] == 404, 'reused application without targets'

    resp = client.get(url='/const')
    assert resp['status'] == 200, 'constant pass, reused application'
    assert resp['body'] == '2'

    assert not findall(r'exited on signal'), 'no crash'


def test_python_targets_lookup_bound_listener(findall):
    # A "pass" with variables in a listener goes through the same lookup as
    # one in a route action.

    assert 'success' in client.conf(
        {
            "listeners": {
                "*:8080": {"pass": "applications/targets/$arg_t"},
                "*:8081": {"pass": "applications/plain/$arg_t"},
                "*:8082": {"pass": "applications/$arg_a"},
            },
            "applications": applications_conf(
                {"targets": True, "plain": False}
            ),
        }
    )

    resp = client.get(url='/?t=1')
    assert resp['status'] == 200, 'listener, known target'
    assert resp['body'] == '1'

    resp = client.get(url='/?t=2')
    assert resp['status'] == 200, 'listener, second target'
    assert resp['body'] == '2'

    for t in ['missing', '3', '']:
        resp = client.get(url=f'/?t={t}')
        assert resp['status'] == 404, f'listener, missing target {t}'

    resp = client.get(url='/?t=x', port=8081)
    assert resp['status'] == 404, 'listener, target of an app without targets'

    # The application without "targets", with no target in the pass.

    resp = client.get(url='/?a=plain', port=8082)
    assert resp['status'] == 200, 'listener, application without targets'
    assert resp['body'] == '2'

    resp = client.get(url='/?a=missing', port=8082)
    assert resp['status'] == 404, 'listener, missing application'

    # A listener "pass" keeps the value of a variable as it is, as a route
    # action does: "%2531" is the target "%31" and "%2570lain" is the
    # application "%70lain".  Neither exists.

    resp = client.get(url='/?t=%2531')
    assert resp['status'] == 404, 'listener, target decoded once'

    resp = client.get(url='/?a=%2570lain', port=8082)
    assert resp['status'] == 404, 'listener, application decoded once'

    assert not findall(r'exited on signal'), 'no crash'
