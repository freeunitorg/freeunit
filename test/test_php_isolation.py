from unit.applications.lang.php import ApplicationPHP

prerequisites = {'modules': {'php': 'any'}, 'features': {'isolation': True}}

client = ApplicationPHP()


def test_php_isolation_rootfs(is_su, require, temp_dir):
    isolation = {'rootfs': temp_dir}

    if not is_su:
        require(
            {
                'features': {
                    'isolation': [
                        'unprivileged_userns_clone',
                        'user',
                        'mnt',
                        'pid',
                    ]
                }
            }
        )

        isolation['namespaces'] = {
            'mount': True,
            'credential': True,
            'pid': True,
        }

    client.load('phpinfo', isolation=isolation)

    assert 'success' in client.conf(
        '"/app/php/phpinfo"', 'applications/phpinfo/root'
    )
    assert 'success' in client.conf(
        '"/app/php/phpinfo"', 'applications/phpinfo/working_directory'
    )

    assert client.get()['status'] == 200, 'empty rootfs'


def test_php_isolation_rootfs_extensions(is_su, require, temp_dir):
    isolation = {'rootfs': temp_dir}

    if not is_su:
        require(
            {
                'features': {
                    'isolation': [
                        'unprivileged_userns_clone',
                        'user',
                        'mnt',
                        'pid',
                    ]
                }
            }
        )

        isolation['namespaces'] = {
            'mount': True,
            'credential': True,
            'pid': True,
        }

    client.load('list-extensions', isolation=isolation)

    assert 'success' in client.conf(
        '"/app/php/list-extensions"', 'applications/list-extensions/root'
    )

    assert 'success' in client.conf(
        {'file': '/php/list-extensions/php.ini'},
        'applications/list-extensions/options',
    )

    assert 'success' in client.conf(
        '"/app/php/list-extensions"',
        'applications/list-extensions/working_directory',
    )

    extensions = client.getjson()['body']

    assert 'json' in extensions, 'json in extensions list'
    assert 'unit' in extensions, 'unit in extensions list'


def test_php_isolation_pid_namespace(require):
    """A prototype in its own pid namespace still starts workers.

    The router's START_PROCESS reaches such a prototype from an ancestor pid
    namespace, so the credential the kernel attaches to it is 0 rather than
    the router's pid: the prototype has no pid for the router at all.  The
    sender check in nxt_proto_start_process_handler() (issue #270) has to
    know that, and a version of it that simply compared against the router's
    global pid would refuse every start here -- with the application never
    coming up, which is what this asks about.

    Unlike the rootfs cases above, the pid namespace is requested whether or
    not the suite runs as root: as root is exactly where the other tests do
    not ask for one.
    """
    require({'features': {'isolation': ['pid']}})

    client.load('phpinfo', isolation={'namespaces': {'pid': True}})

    assert client.get()['status'] == 200, 'pid-isolated prototype'
