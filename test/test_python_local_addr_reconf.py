"""The local address of a keep-alive connection after a reconfiguration.

A listener on one address (not a wildcard) gives the application the
address from the configuration.  A keep-alive connection holds no
reference to the configuration between two requests, so an update can
free the old configuration while the connection is idle.  The next request
on the connection must still give the right SERVER_ADDR and SERVER_PORT.
On an AddressSanitizer build the router must not read freed memory.  A
build without a sanitizer can keep the old bytes and pass on the old code.
"""

import time

from unit.applications.lang.python import ApplicationPython
from unit.option import option
from unit import port as port_map

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

DESTROYED = r'old router conf is destroyed'


def conf(idle_timeout):
    return {
        "listeners": {"127.0.0.1:8080": {"pass": "applications/addr"}},
        "applications": {
            "addr": {
                "type": client.get_application_type(),
                "processes": {"spare": 0},
                "path": f'{option.test_dir}/python/server_addr',
                "working_directory": f'{option.test_dir}/python/server_addr',
                "module": "wsgi",
            }
        },
        "settings": {"http": {"idle_timeout": idle_timeout}},
    }


def test_python_local_addr_after_reconf(findall):
    assert 'success' in client.conf(conf(30)), 'configuration'

    resp, sock = client.get(
        headers={'Host': 'localhost', 'Connection': 'keep-alive'},
        start=True,
        read_timeout=1,
    )
    first = (
        resp['status'],
        resp['headers'].get('Server-Addr'),
        resp['headers'].get('Server-Port'),
    )
    assert first == (200, '127.0.0.1', str(port_map.port(8080)))

    destroyed = len(findall(DESTROYED))

    assert 'success' in client.conf(conf(31)), 'update of an unrelated key'

    # A debug build logs the release of the old configuration.  Wait for
    # it.  Other builds do not log it; then wait a fixed time.

    if findall(r'\[debug\]'):
        for _ in range(100):
            if len(findall(DESTROYED)) > destroyed:
                break

            time.sleep(0.1)

        assert len(findall(DESTROYED)) > destroyed, 'old conf destroyed'

    else:
        time.sleep(1)

    resp = client.get(
        sock=sock,
        headers={'Host': 'localhost', 'Connection': 'close'},
        read_timeout=5,
    )
    second = (
        resp.get('status'),
        resp.get('headers', {}).get('Server-Addr'),
        resp.get('headers', {}).get('Server-Port'),
    )
    assert second == first, 'local address after the update'
