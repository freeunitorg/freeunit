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


def test_php_isolation_rootfs_credential_without_mount(is_su, require,
                                                       temp_dir):
    if not is_su:
        require(
            {'features': {'isolation': ['unprivileged_userns_clone', 'user']}}
        )
    else:
        require({'features': {'isolation': ['user']}})

    client.load('phpinfo')

    # A new user namespace without a new mount namespace cannot mount
    # anything: the child stays in the parent's mount namespace, owned by
    # the initial user namespace.  The automounts default to on, so this
    # config could never start; it must be refused at configuration time.
    resp = client.conf(
        {'rootfs': temp_dir, 'namespaces': {'credential': True}},
        'applications/phpinfo/isolation',
    )

    assert 'error' in resp, 'credential without mount rejected'

    detail = resp.get('detail', '')

    assert 'rootfs' in detail, 'detail names rootfs'
    assert 'credential' in detail, 'detail names credential'
    assert 'mount' in detail, 'detail names mount'
    assert 'automount' in detail, 'detail names automount'

    # The remedy must name the knobs to turn off, not just "the automount
    # options".  "procfs" and "tmpfs" are unconditional builtins, so both
    # arms fire here whatever the module declares.
    assert '"procfs": false' in detail, 'detail names the procfs knob'
    assert '"tmpfs": false' in detail, 'detail names the tmpfs knob'

    # An explicit "mount": false is the same thing.
    resp = client.conf(
        {
            'rootfs': temp_dir,
            'namespaces': {'credential': True, 'mount': False},
        },
        'applications/phpinfo/isolation',
    )

    assert 'error' in resp, 'explicit mount false rejected'

    # Without "credential" the config is fine -- the prototype keeps the
    # main process' privileges and mounts in the parent mount namespace.
    assert 'success' in client.conf(
        {'rootfs': temp_dir, 'namespaces': {'credential': False}},
        'applications/phpinfo/isolation',
    ), 'no credential accepted'


def test_php_isolation_rootfs_credential_with_mount(is_su, require, temp_dir):
    require(
        {
            'features': {
                'isolation': (
                    ['user', 'mnt', 'pid']
                    if is_su
                    else ['unprivileged_userns_clone', 'user', 'mnt', 'pid']
                )
            }
        }
    )

    client.load(
        'phpinfo',
        isolation={
            'rootfs': temp_dir,
            'namespaces': {'credential': True, 'mount': True, 'pid': True},
        },
    )

    assert 'success' in client.conf(
        '"/app/php/phpinfo"', 'applications/phpinfo/root'
    )
    assert 'success' in client.conf(
        '"/app/php/phpinfo"', 'applications/phpinfo/working_directory'
    )

    assert client.get()['status'] == 200, 'credential + mount serves'


def test_php_isolation_rootfs_credential_no_automount(is_su, require,
                                                      temp_dir):
    require(
        {
            'features': {
                'isolation': (
                    ['user']
                    if is_su
                    else ['unprivileged_userns_clone', 'user']
                )
            }
        }
    )

    # With every automount disabled nothing is mounted: the rootfs switch
    # falls back to chroot(2), which needs only CAP_SYS_CHROOT in the new
    # user namespace.  This works, so the validator must keep accepting it.
    #
    # "language_deps" is off here too, so the case stays valid whether or
    # not the PHP module declares dependency mounts of its own.
    client.load(
        'phpinfo',
        isolation={
            'rootfs': temp_dir,
            'namespaces': {'credential': True},
            'automount': {
                'procfs': False,
                'tmpfs': False,
                'language_deps': False,
            },
        },
    )

    assert 'success' in client.conf(
        '"/app/php/phpinfo"', 'applications/phpinfo/root'
    )
    assert 'success' in client.conf(
        '"/app/php/phpinfo"', 'applications/phpinfo/working_directory'
    )

    assert client.get()['status'] == 200, 'no automount serves'


def test_php_isolation_pid_namespace(is_su, require):
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
    not ask for one.  Unprivileged, unshare(CLONE_NEWPID) needs CAP_SYS_ADMIN,
    so a user namespace has to come with it -- the same shape as
    test_go_isolation.py::test_isolation_pid.
    """
    require({'features': {'isolation': ['pid']}})

    if not is_su:
        require(
            {
                'features': {
                    'isolation': [
                        'unprivileged_userns_clone',
                        'user',
                        'mnt',
                    ]
                }
            }
        )

    isolation = {'namespaces': {'pid': True}}

    if not is_su:
        isolation['namespaces']['mount'] = True
        isolation['namespaces']['credential'] = True

    client.load('phpinfo', isolation=isolation)

    assert client.get()['status'] == 200, 'pid-isolated prototype'
