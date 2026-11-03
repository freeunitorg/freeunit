import socket
import time

import pytest

from unit.applications.proto import ApplicationProto
from unit import port as port_map

client = ApplicationProto()

# A zone survives a reconfiguration by name, and an old configuration can
# still hold it when the next test starts: every test names its own.
ZONE = 'z'


@pytest.fixture(autouse=True)
def zone_name(request):
    global ZONE

    ZONE = request.node.name

# "client_ip" lets one test act as several clients: the limit key is the
# address the listener takes from X-Forwarded-For, when it trusts the peer.
CLIENT_IP = {'header': 'X-Forwarded-For', 'source': '127.0.0.1'}

PARTIAL_POST = (
    b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n'
)


def limit(zone=None, rate=1, burst=None, stage=None):
    lr = {'zone': ZONE if zone is None else zone, 'rate': rate}

    if burst is not None:
        lr['burst'] = burst

    if stage is not None:
        lr['stage'] = stage

    return lr


def limited(**kwargs):
    return [{'action': {'limit_req': limit(**kwargs), 'return': 200}}]


def config(routes, listener=None, settings=None):
    if listener is None:
        listener = {'pass': 'routes'}

    conf = {
        'listeners': {'*:8080': listener},
        'routes': routes,
        'applications': {},
    }

    if settings is not None:
        conf['settings'] = settings

    return conf


def load(routes, listener=None, settings=None):
    assert 'success' in client.conf(
        config(routes, listener, settings)
    ), 'limit_req configure'


def get_as(addr):
    return client.get(
        headers={
            'Host': 'localhost',
            'Connection': 'close',
            'X-Forwarded-For': addr,
        }
    )['status']


def counters(name=None):
    return client.conf_get('/status')['limit_req'][ZONE if name is None else name]


def test_limit_req_invalid():
    def check(conf, detail=None):
        resp = client.conf(conf)

        assert 'error' in resp, conf

        if detail is not None:
            assert resp['detail'] == detail, conf

    def action(lr):
        return [{'action': {'limit_req': lr, 'return': 200}}]

    check(config(action('z')))
    check(config(action({})))
    check(config(action({'zone': 'z', 'rate': '1'})))
    check(config(action({'zone': 'z', 'rate': 1.5})))
    check(config(action({'zone': 'z', 'rate': 1, 'foo': 1})))
    check(config(action({'zone': 1, 'rate': 1})))

    # "burst" without "rate"
    check(
        config(action({'zone': 'z', 'burst': 2})),
        'Required parameter "rate" is missing.',
    )

    check(
        config(action({'zone': 'z', 'rate': 0})),
        'The "rate" number must be equal to or greater than 1.',
    )
    check(
        config(action({'zone': 'z', 'rate': 1000001})),
        'The "rate" number must not exceed 1000000.',
    )
    check(
        config(action({'zone': 'z', 'rate': 1, 'burst': 0})),
        'The "burst" number must be equal to or greater than 1.',
    )
    check(
        config(action({'zone': 'z', 'rate': 1, 'burst': 65536})),
        'The "burst" number must not exceed 65535.',
    )
    check(
        config(action({'zone': ':10', 'rate': 1})),
        'The "zone" value ":10" must be "name" or "name:keys".',
    )
    check(
        config(action({'zone': 'z:0', 'rate': 1})),
        'The "zone" size in "z:0" must be an integer from 1 to 1048576.',
    )
    check(
        config(action({'zone': 'z:10k', 'rate': 1})),
        'The "zone" size in "z:10k" must be an integer from 1 to 1048576.',
    )
    check(
        config(action({'zone': 'z', 'rate': 1, 'stage': 'later'})),
        'The "stage" value must be "headers" or "body".',
    )

    # one zone, one set of parameters
    detail = (
        f'The "limit_req" zone "{ZONE}" is used with another size, "rate" '
        'or "burst" elsewhere.'
    )
    check(
        config(
            [
                {'match': {'uri': '/a'}, 'action': {'limit_req': limit(rate=1), 'return': 200}},
                {'match': {'uri': '/b'}, 'action': {'limit_req': limit(rate=2), 'return': 200}},
            ]
        ),
        detail,
    )
    check(
        config(
            {
                'a': [{'action': {'limit_req': limit(zone=f'{ZONE}:10'), 'return': 200}}],
                'b': [{'action': {'limit_req': limit(zone=f'{ZONE}:20'), 'return': 200}}],
            },
            {'pass': 'routes/a'},
        ),
        detail,
    )
    check(
        config(
            {
                'a': [{'action': {'limit_req': limit(burst=1), 'return': 200}}],
                'b': [{'action': {'limit_req': limit(burst=2), 'return': 200}}],
            },
            {'pass': 'routes/a'},
        ),
        detail,
    )


def test_limit_req_under_rate():
    load(limited(rate=100))

    for _ in range(5):
        assert client.get()['status'] == 200
        time.sleep(0.02)

    c = counters()
    assert c['passed'] == 5
    assert c['rejected'] == 0
    assert c['keys'] == 1


def test_limit_req_burst():
    load(limited(rate=1, burst=3))

    for _ in range(3):
        assert client.get()['status'] == 200

    resp = client.get()
    assert resp['status'] == 429
    assert resp['headers']['Retry-After'] == '1'
    assert 'Error 429' in resp['body']

    c = counters()
    assert c['passed'] == 3
    assert c['rejected'] == 1


def test_limit_req_retry_after():
    # One per second, 100 at once: the bucket is empty after 100 requests
    # and one request fits after one second, so "Retry-After" is 1.  With
    # 1 per 2 seconds being not expressible, the estimate cannot be tested
    # above 1 here; the C test checks the milliseconds.
    load(limited(rate=1, burst=100))

    for _ in range(100):
        assert client.get()['status'] == 200

    resp = client.get()
    assert resp['status'] == 429
    assert resp['headers']['Retry-After'] == '1'


def test_limit_req_refill():
    load(limited(rate=2))

    assert client.get()['status'] == 200
    assert client.get()['status'] == 429

    time.sleep(0.6)

    assert client.get()['status'] == 200
    assert client.get()['status'] == 429


def test_limit_req_per_client():
    load(limited(), {'pass': 'routes', 'client_ip': CLIENT_IP})

    assert get_as('10.0.0.1') == 200
    assert get_as('10.0.0.1') == 429
    assert get_as('10.0.0.2') == 200
    assert get_as('10.0.0.1') == 429

    # IPv6 clients are keyed by their /64
    assert get_as('2001:db8::1') == 200
    assert get_as('2001:db8::2') == 429
    assert get_as('2001:db8:1::1') == 200

    assert counters()['keys'] == 4


def test_limit_req_untrusted_xff():
    # Without "client_ip" the header is not looked at: one peer, one key.
    load(limited())

    assert get_as('10.0.0.1') == 200
    assert get_as('10.0.0.2') == 429
    assert client.get()['status'] == 429

    assert counters()['keys'] == 1

    # With "client_ip" from a source that is not the peer: the same.  A new
    # zone name: the one above is kept with its bucket by the same name.
    load(
        limited(zone=f'{ZONE}-2'),
        {
            'pass': 'routes',
            'client_ip': {'header': 'X-Forwarded-For', 'source': '10.0.0.0/8'},
        },
    )

    assert get_as('10.0.0.1') == 200
    assert get_as('10.0.0.2') == 429

    assert counters(f'{ZONE}-2')['keys'] == 1


def test_limit_req_headers_stage_post():
    load(limited(stage='headers'))

    assert client.get()['status'] == 200

    # The body never arrives; the 429 does, and the connection is closed
    # instead of waiting for the 100 bytes.
    resp = client.http(PARTIAL_POST, raw=True, read_timeout=5)

    assert resp['status'] == 429
    assert resp['headers']['Retry-After'] == '1'
    assert resp['headers']['Connection'] == 'close'

    # The default stage is "headers".
    load(limited())

    resp = client.http(PARTIAL_POST, raw=True, read_timeout=5)
    assert resp['status'] == 429


def test_limit_req_body_stage_post():
    load(limited(stage='body'))

    assert client.post(body='x' * 10)['status'] == 200

    # The body was read: nothing is left on the connection, it stays open.
    resp = client.post(body='x' * 10, connection_close=False, framed=True)
    assert resp['status'] == 429
    assert 'Connection' not in resp['headers']

    # The body is read first: a request without one waits.
    sock = client.http(PARTIAL_POST, raw=True, no_recv=True)

    assert client.recvall(sock, read_timeout=1) == b''

    sock.close()


def test_limit_req_keepalive():
    load(limited())

    # Without a body nothing is left on the connection: it stays open.
    resp, sock = client.get(start=True, connection_close=False, framed=True)
    assert resp['status'] == 200

    resp = client.get(sock=sock, connection_close=False, framed=True)
    assert resp['status'] == 429
    assert 'Connection' not in resp['headers']

    sock.close()


def test_limit_req_reload():
    load(limited())

    assert client.get()['status'] == 200
    assert client.get()['status'] == 429

    c = counters()
    assert c['passed'] == 1
    assert c['rejected'] == 1

    # A new configuration with the same zone name and size: the zone, its
    # bucket and its counters stay.
    load([{'match': {'uri': '/other'}, 'action': {'return': 204}}] + limited())

    assert counters() == {
        'keys': 1,
        'keys_max': 4096,
        'passed': 1,
        'rejected': 1,
        'evicted': 0,
    }

    assert client.get()['status'] == 429
    assert counters()['rejected'] == 2

    # A new "burst" applies to the bucket as it is: still empty.
    load(limited(burst=5))

    assert client.get()['status'] == 429
    assert counters()['rejected'] == 3

    # A new size is a new zone: the keys and the counters start again.
    load(limited(zone=f'{ZONE}:8', burst=5))

    assert counters() == {
        'keys': 0,
        'keys_max': 8,
        'passed': 0,
        'rejected': 0,
        'evicted': 0,
    }

    assert client.get()['status'] == 200

    # A zone that no configuration names any more is gone.
    load(limited(zone=f'{ZONE}-other'))

    assert ZONE not in client.conf_get('/status')['limit_req']

    load([{'action': {'return': 200}}])

    assert 'limit_req' not in client.conf_get('/status')


def test_limit_req_shared_zone():
    load(
        [
            {'match': {'uri': '/a'}, 'action': {'limit_req': limit(), 'return': 200}},
            {'match': {'uri': '/b'}, 'action': {'limit_req': limit(), 'return': 200}},
        ]
    )

    assert client.get(url='/a')['status'] == 200
    assert client.get(url='/b')['status'] == 429

    c = counters()
    assert c['passed'] == 1
    assert c['rejected'] == 1


def test_limit_req_pass_chain_counted_once():
    load(
        {
            'first': [{'action': {'pass': 'routes/second'}}],
            'second': limited(burst=2),
        },
        {'pass': 'routes/first'},
    )

    assert client.get()['status'] == 200
    assert client.get()['status'] == 200
    assert client.get()['status'] == 429

    c = counters()
    assert c['passed'] == 2
    assert c['rejected'] == 1

    # Behind a rewrite the headers stage cannot see the limit; the body
    # stage counts the request, once.  A new zone: the one above is kept.
    load(
        {
            'first': [
                {'action': {'rewrite': '/second', 'pass': 'routes/second'}}
            ],
            'second': limited(zone=f'{ZONE}-2', burst=2),
        },
        {'pass': 'routes/first'},
    )

    assert client.get()['status'] == 200
    assert client.get()['status'] == 200
    assert client.get()['status'] == 429


def test_limit_req_eviction():
    load(limited(zone=f'{ZONE}:1'), {'pass': 'routes', 'client_ip': CLIENT_IP})

    assert get_as('10.0.0.1') == 200
    assert get_as('10.0.0.1') == 429

    # The second client takes the one entry; the first one starts afresh.
    assert get_as('10.0.0.2') == 200
    assert get_as('10.0.0.1') == 200

    c = counters()
    assert c['keys'] == 1
    assert c['keys_max'] == 1
    assert c['evicted'] == 2
    assert c['passed'] == 3
    assert c['rejected'] == 1


def test_limit_req_threads():
    # Four router threads, one zone: sixteen connections at once from one
    # client pass "burst" requests between them, not "burst" per thread.
    load(limited(burst=4), settings={'listen_threads': 4})

    socks = []

    for _ in range(16):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(('127.0.0.1', port_map.port(8080)))
        socks.append(sock)

    for sock in socks:
        sock.sendall(
            b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'
        )

    statuses = []

    for sock in socks:
        resp = client.recvall(sock).decode()
        statuses.append(int(resp.split(' ')[1]))
        sock.close()

    # One more token can arrive while the batch runs, no more.
    assert 4 <= statuses.count(200) <= 5
    assert statuses.count(429) == 16 - statuses.count(200)

    c = counters()
    assert c['passed'] == statuses.count(200)
    assert c['rejected'] == statuses.count(429)
    assert c['keys'] == 1
