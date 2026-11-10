import time

import pytest

from unit.applications.proto import ApplicationProto
from unit.status import Status

client = ApplicationProto()


@pytest.fixture(autouse=True)
def setup_method_fixture():
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    )


def clear_conf():
    assert 'success' in client.conf({"listeners": {}, "applications": {}})


def wait_for_request(timeout=50):
    # The listener's deletion closes a connection whose request the router
    # has not started to read: a connection in the kernel listen queue is
    # reset, and an accepted connection in the idle state gets a 408 and
    # a close (nxt_h1p_idle_io_read_handler()).  The router counts the
    # request when it starts it (nxt_http_request_create()), after the
    # first read, so wait for that count.
    for _ in range(timeout):
        if Status.get('/requests/total') == 1:
            return

        time.sleep(0.1)

    pytest.fail('The router did not start the request.')


def test_reconfigure():
    Status.init()

    sock = client.http(
        b"""GET / HTTP/1.1
""",
        raw=True,
        no_recv=True,
    )

    wait_for_request()

    clear_conf()

    resp = client.http(
        b"""Host: localhost
Connection: close

""",
        sock=sock,
        raw=True,
    )
    assert resp['status'] == 200, 'finish request'


def test_reconfigure_2():
    sock = client.http(b'', raw=True, no_recv=True)

    # Waiting for connection completion.
    # Delay should be more than TCP_DEFER_ACCEPT.
    time.sleep(1.5)

    clear_conf()

    assert client.get(sock=sock)['status'] == 408, 'request timeout'
