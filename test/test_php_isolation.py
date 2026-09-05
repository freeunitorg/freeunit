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

    # With every applicable automount disabled nothing is mounted: the
    # rootfs switch falls back to chroot(2), which needs only
    # CAP_SYS_CHROOT in the new user namespace.  This works, so the
    # validator must keep accepting it.
    client.load(
        'phpinfo',
        isolation={
            'rootfs': temp_dir,
            'namespaces': {'credential': True},
            'automount': {'procfs': False, 'tmpfs': False},
        },
    )

    assert 'success' in client.conf(
        '"/app/php/phpinfo"', 'applications/phpinfo/root'
    )
    assert 'success' in client.conf(
        '"/app/php/phpinfo"', 'applications/phpinfo/working_directory'
    )

    assert client.get()['status'] == 200, 'no automount serves'
