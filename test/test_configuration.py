import re
import socket
import time

import pytest

from unit.control import Control
from unit.log import Log

prerequisites = {'modules': {'python': 'any'}}

client = Control()


def try_addr(addr):
    return client.conf(
        {
            "listeners": {addr: {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    )


def test_json_empty():
    assert 'error' in client.conf(''), 'empty'


def test_json_leading_zero():
    assert 'error' in client.conf('00'), 'leading zero'


def test_json_unicode():
    assert 'success' in client.conf(
        """
        {
            "ap\u0070": {
                "type": "\u0070ython",
                "processes": { "spare": 0 },
                "path": "\u002Fapp",
                "module": "wsgi"
            }
        }
        """,
        'applications',
    ), 'unicode'

    assert client.conf_get('applications') == {
        "app": {
            "type": "python",
            "processes": {"spare": 0},
            "path": "/app",
            "module": "wsgi",
        }
    }, 'unicode get'


def test_json_unicode_2():
    assert 'success' in client.conf(
        {
            "приложение": {
                "type": "python",
                "processes": {"spare": 0},
                "path": "/app",
                "module": "wsgi",
            }
        },
        'applications',
    ), 'unicode 2'

    assert 'приложение' in client.conf_get('applications')


def test_json_unicode_number():
    assert 'success' in client.conf(
        """
        {
            "app": {
                "type": "python",
                "processes": { "spare": \u0030 },
                "path": "/app",
                "module": "wsgi"
            }
        }
        """,
        'applications',
    ), 'unicode number'


def test_json_utf8_bom():
    assert 'success' in client.conf(
        b"""\xEF\xBB\xBF
        {
            "app": {
                "type": "python",
                "processes": {"spare": 0},
                "path": "/app",
                "module": "wsgi"
            }
        }
        """,
        'applications',
    ), 'UTF-8 BOM'


def test_json_comment_single_line():
    assert 'success' in client.conf(
        b"""
        // this is bridge
        {
            "//app": {
                "type": "python", // end line
                "processes": {"spare": 0},
                // inside of block
                "path": "/app",
                "module": "wsgi"
            }
            // double //
        }
        // end of json \xEF\t
        """,
        'applications',
    ), 'single line comments'


def test_json_comment_multi_line():
    assert 'success' in client.conf(
        b"""
        /* this is bridge */
        {
            "/*app": {
            /**
             * multiple lines
             **/
                "type": "python",
                "processes": /* inline */ {"spare": 0},
                "path": "/app",
                "module": "wsgi"
                /*
                // end of block */
            }
            /* blah * / blah /* blah */
        }
        /* end of json \xEF\t\b */
        """,
        'applications',
    ), 'multi line comments'


def test_json_comment_invalid():
    assert 'error' in client.conf(b'/{}', 'applications'), 'slash'
    assert 'error' in client.conf(b'//{}', 'applications'), 'comment'
    assert 'error' in client.conf(b'{} /', 'applications'), 'slash end'
    assert 'error' in client.conf(b'/*{}', 'applications'), 'slash star'
    assert 'error' in client.conf(b'{} /*', 'applications'), 'slash star end'


def test_applications_open_brace():
    assert 'error' in client.conf('{', 'applications'), 'open brace'


def test_applications_string():
    assert 'error' in client.conf('"{}"', 'applications'), 'string'


@pytest.mark.skip('not yet, unsafe')
def test_applications_type_only():
    assert 'error' in client.conf(
        {"app": {"type": "python"}}, 'applications'
    ), 'type only'


def test_applications_miss_quote():
    assert 'error' in client.conf(
        """
        {
            app": {
                "type": "python",
                "processes": { "spare": 0 },
                "path": "/app",
                "module": "wsgi"
            }
        }
        """,
        'applications',
    ), 'miss quote'


def test_applications_miss_colon():
    assert 'error' in client.conf(
        """
        {
            "app" {
                "type": "python",
                "processes": { "spare": 0 },
                "path": "/app",
                "module": "wsgi"
            }
        }
        """,
        'applications',
    ), 'miss colon'


def test_applications_miss_comma():
    assert 'error' in client.conf(
        """
        {
            "app": {
                "type": "python"
                "processes": { "spare": 0 },
                "path": "/app",
                "module": "wsgi"
            }
        }
        """,
        'applications',
    ), 'miss comma'


def test_applications_skip_spaces():
    assert 'success' in client.conf(b'{ \n\r\t}', 'applications'), 'skip spaces'


def test_applications_relative_path():
    assert 'success' in client.conf(
        {
            "app": {
                "type": "python",
                "processes": {"spare": 0},
                "path": "../app",
                "module": "wsgi",
            }
        },
        'applications',
    ), 'relative path'


def test_applications_cstring_nul():
    # "spare": 0 keeps the app from spawning, so validation is exercised
    # without any process startup.
    def conf_app(app):
        return client.conf({"a": app}, 'applications')

    base = {"type": "python", "processes": {"spare": 0}, "module": "wsgi"}

    # Length-tracked config strings later used as NUL-terminated C strings
    # must reject an embedded NUL (\0 survives JSON parsing) and an
    # empty value.
    for field in ["user", "group", "working_directory", "stdout", "stderr", "home"]:
        assert 'error' in conf_app({**base, field: "/x\0y"}), f'{field} nul'
        assert 'error' in conf_app({**base, field: ""}), f'{field} empty'

    # "executable" of an external app (mapped as a C string, passed to execve).
    assert 'error' in conf_app(
        {"type": "external", "processes": {"spare": 0}, "executable": "/bin/tr\0ue"}
    ), 'executable nul'
    assert 'error' in conf_app(
        {"type": "external", "processes": {"spare": 0}, "executable": ""}
    ), 'executable empty'

    # Valid values are still accepted.
    assert 'success' in conf_app(
        {**base, "working_directory": "/tmp", "home": "/tmp"}
    ), 'valid values'


def test_listeners_unix_path_nul(system):
    if system != 'Linux':
        pytest.skip('unix sockets')

    # A pathname unix socket address is used as a C string for bind()/unlink();
    # an embedded NUL must be rejected (abstract "unix:@..." sockets, tested
    # elsewhere, legitimately carry NULs and are exempt).
    assert 'error' in try_addr("unix:/tmp/x\0y"), 'pathname \0'


def test_access_log_cstring_nul(temp_dir):
    # The access log path is passed to the router and opened as a
    # NUL-terminated C string; both the string form and the object form
    # "path" must reject an embedded NUL (\0 survives JSON parsing) and
    # an empty value.
    def conf_log(value):
        return client.conf(
            {"listeners": {}, "applications": {}, "access_log": value}
        )

    assert 'error' in conf_log("/tmp/x\0y"), 'string nul'
    assert 'error' in conf_log(""), 'string empty'
    assert 'error' in conf_log({"path": "/tmp/x\0y"}), 'path nul'
    assert 'error' in conf_log({"path": ""}), 'path empty'

    # Valid values are still accepted.
    assert 'success' in conf_log(f'{temp_dir}/access.log'), 'string valid'
    assert 'success' in conf_log(
        {"path": f'{temp_dir}/access.log'}
    ), 'path valid'


@pytest.mark.skip('not yet, unsafe')
def test_listeners_empty():
    assert 'error' in client.conf({"*:8080": {}}, 'listeners'), 'listener empty'


def test_listeners_no_app():
    assert 'error' in client.conf(
        {"*:8080": {"pass": "applications/app"}}, 'listeners'
    ), 'listeners no app'


def test_listeners_unix_abstract(system):
    if system != 'Linux':
        assert 'error' in try_addr("unix:@sock"), 'abstract at'

    pytest.skip('not yet')

    assert 'error' in try_addr("unix:\0soc"), 'abstract \0'
    assert 'error' in try_addr("unix:\u0000soc"), 'abstract \0 unicode'


def test_listeners_addr():
    assert 'success' in try_addr("*:8080"), 'wildcard'
    assert 'success' in try_addr("127.0.0.1:8081"), 'explicit'
    assert 'success' in try_addr("[::1]:8082"), 'explicit ipv6'


def test_listeners_addr_error():
    assert 'error' in try_addr("127.0.0.1"), 'no port'


def test_listeners_addr_error_2(skip_alert):
    skip_alert(r'bind.*failed', r'failed to apply new conf')

    assert 'error' in try_addr("[f607:7403:1e4b:6c66:33b2:843f:2517:da27]:8080")


def test_listeners_port_release():
    for _ in range(10):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

            client.conf(
                {
                    "listeners": {"127.0.0.1:8080": {"pass": "routes"}},
                    "routes": [],
                }
            )

            resp = client.conf({"listeners": {}, "applications": {}})

            # The router closes the listening descriptor before it
            # acknowledges the removal, but close(2) only drops the closing
            # thread's reference to the open file.  Every other router
            # thread that had the descriptor armed for accept(2) may still
            # hold a transient kernel reference to it, and the address
            # leaves the bind hash only when the last one is dropped, which
            # happens asynchronously.  A client therefore has to retry, so
            # the test retries too; a listener that is genuinely never
            # closed still fails here.
            deadline = time.monotonic() + 5

            while True:
                try:
                    s.bind(('127.0.0.1', 8080))
                    s.listen()
                    break

                except OSError:
                    if time.monotonic() >= deadline:
                        pytest.fail('cannot bind or listen to the address')

                    time.sleep(0.01)

            assert 'success' in resp, 'port release'


def test_listeners_close_before_reply(findall):
    assert 'success' in client.conf(
        {
            "listeners": {"127.0.0.1:8080": {"pass": "routes"}},
            "routes": [],
        }
    ), 'listener added'

    # The markers below are all nxt_debug(), so a release build cannot
    # observe this at all.  There is no build flag in option.available,
    # so ask the log: skipping is the only honest outcome here, passing
    # would be a test that cannot fail.
    if not findall(r'\[debug\]'):
        pytest.skip('needs a build configured with --debug')

    # Remember where the log ends before the removal.  The tail already
    # holds a "controller conn write" for the request above, and the
    # ordering is read off positions in a single read of what follows:
    # two independent waits could each succeed with the order between
    # them wrong.
    pos = len(Log.read())

    assert 'success' in client.conf(
        {"listeners": {}, "applications": {}}
    ), 'listener removed'

    # The reply is the barrier that makes the tail complete rather than
    # something the assertions below compare against: it is written only
    # after every engine has acknowledged, and every engine writes its
    # own records before it acknowledges.
    deadline = time.monotonic() + 15

    while True:
        tail = Log.read()[pos:]

        if 'controller conn write complete' in tail:
            break

        if time.monotonic() >= deadline:
            pytest.fail('the controller did not reply')

        time.sleep(0.01)

    # One record per engine, so with listen_threads > 1 there are
    # several of each.  Comparing the last of one with the last of the
    # other is what makes the assertion independent of how the engines
    # interleave: an engine acknowledges after it has finished closing,
    # so the latest acknowledgement is later than every close; an engine
    # that acknowledged first would put the latest close after every
    # acknowledgement instead.
    finish = list(
        re.finditer(
            r'\[debug\] (\d+)#\d+ .*listen socket close finish: (\d+)',
            tail,
        )
    )
    ack = list(re.finditer(r'listen socket close acknowledged', tail))

    assert finish, 'listener closed'
    assert ack, 'removal acknowledged'

    assert finish[-1].start() < ack[-1].start(), 'phase 2 before ack'

    # The one that has to hold: "close finish" is written on entry to
    # nxt_router_listen_socket_close_finish(), before the descriptor is
    # released, so the check above cannot see an acknowledgement moved
    # back above nxt_router_listen_socket_release() inside that function
    # -- the position #276 moved it out of.  The close(2) of the
    # descriptor is written by the same engine that then acknowledges,
    # so anchoring on it covers both positions.
    #
    # Match the router's pid: the controller writes "socket close(N)"
    # for its own connection after every reply, and fd numbers collide
    # across processes.
    pid, fd = finish[-1].group(1), finish[-1].group(2)
    closed = re.search(
        rf'\[debug\] {pid}#\d+ (\*\d+ )?socket close\({fd}\)', tail
    )

    assert closed is not None, 'descriptor closed'

    assert closed.start() < ack[-1].start(), 'closed before ack'


def test_json_application_name_large():
    name = "X" * 1024 * 1024

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": f"applications/{name}"}},
            "applications": {
                name: {
                    "type": "python",
                    "processes": {"spare": 0},
                    "path": "/app",
                    "module": "wsgi",
                }
            },
        }
    )


@pytest.mark.skip('not yet')
def test_json_application_many():
    apps = 999

    conf = {
        "applications": {
            f"app-{a}": {
                "type": "python",
                "processes": {"spare": 0},
                "path": "/app",
                "module": "wsgi",
            }
            for a in range(apps)
        },
        "listeners": {
            f"*:{(7000 + a)}": {"pass": f"applications/app-{a}"}
            for a in range(apps)
        },
    }

    assert 'success' in client.conf(conf)


def test_json_application_python_prefix():
    conf = {
        "applications": {
            "sub-app": {
                "type": "python",
                "processes": {"spare": 0},
                "path": "/app",
                "module": "wsgi",
                "prefix": "/app",
            }
        },
        "listeners": {"*:8080": {"pass": "routes"}},
        "routes": [
            {
                "match": {"uri": "/app/*"},
                "action": {"pass": "applications/sub-app"},
            }
        ],
    }

    assert 'success' in client.conf(conf)


def test_json_application_prefix_target():
    conf = {
        "applications": {
            "sub-app": {
                "type": "python",
                "processes": {"spare": 0},
                "path": "/app",
                "targets": {
                    "foo": {"module": "foo.wsgi", "prefix": "/app"},
                    "bar": {
                        "module": "bar.wsgi",
                        "callable": "bar",
                        "prefix": "/api",
                    },
                },
            }
        },
        "listeners": {"*:8080": {"pass": "routes"}},
        "routes": [
            {
                "match": {"uri": "/app/*"},
                "action": {"pass": "applications/sub-app/foo"},
            },
            {
                "match": {"uri": "/api/*"},
                "action": {"pass": "applications/sub-app/bar"},
            },
        ],
    }

    assert 'success' in client.conf(conf)


def test_json_application_invalid_python_prefix():
    conf = {
        "applications": {
            "sub-app": {
                "type": "python",
                "processes": {"spare": 0},
                "path": "/app",
                "module": "wsgi",
                "prefix": "app",
            }
        },
        "listeners": {"*:8080": {"pass": "applications/sub-app"}},
    }

    assert 'error' in client.conf(conf)


def test_json_application_empty_python_prefix():
    conf = {
        "applications": {
            "sub-app": {
                "type": "python",
                "processes": {"spare": 0},
                "path": "/app",
                "module": "wsgi",
                "prefix": "",
            }
        },
        "listeners": {"*:8080": {"pass": "applications/sub-app"}},
    }

    assert 'error' in client.conf(conf)


def test_json_application_many2():
    conf = {
        "applications": {
            f"app-{a}": {
                "type": "python",
                "processes": {"spare": 0},
                "path": "/app",
                "module": "wsgi",
            }
            # Larger number of applications can cause test fail with default
            # open files limit due to the lack of file descriptors.
            for a in range(100)
        },
        "listeners": {"*:8080": {"pass": "applications/app-1"}},
    }

    assert 'success' in client.conf(conf)


def test_unprivileged_user_error(require, skip_alert):
    require({'privileged_user': False})

    skip_alert(r'cannot set user "root"', r'failed to apply new conf')

    assert 'error' in client.conf(
        {
            "app": {
                "type": "external",
                "processes": 1,
                "executable": "/app",
                "user": "root",
            }
        },
        'applications',
    ), 'setting user'


def test_json_deep_nesting():
    # The controller caps JSON nesting depth so a pathologically deep
    # payload cannot exhaust its stack.  Well past the cap it must be
    # rejected cleanly while the control socket keeps serving.
    depth = 10000
    payload = '[' * depth + ']' * depth

    assert 'error' in client.conf(payload), 'deep nesting rejected'

    # Controller survived and still applies a valid configuration.
    assert 'success' in client.conf(
        {"listeners": {}, "routes": [], "applications": {}}
    ), 'controller alive after deep nesting'


def test_json_too_many_array_elements():
    # Per-array element count is capped to bound heap usage on a
    # malformed payload; just over the cap must be rejected without
    # taking the controller down.
    payload = '[' + ','.join(['0'] * 100001) + ']'

    assert 'error' in client.conf(payload), 'too many array elements rejected'

    assert 'success' in client.conf(
        {"listeners": {}, "routes": [], "applications": {}}
    ), 'controller alive after large array'


def test_json_too_many_object_members():
    # Same cap applies to object members.
    members = ','.join(f'"k{i}":0' for i in range(100001))
    payload = '{' + members + '}'

    assert 'error' in client.conf(payload), 'too many object members rejected'

    assert 'success' in client.conf(
        {"listeners": {}, "routes": [], "applications": {}}
    ), 'controller alive after large object'
