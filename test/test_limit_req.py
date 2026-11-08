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


def quick(run, rate, setup=None, attempts=5):
    """
    Returns (attempt, run(attempt)) for the first attempt that takes less
    than 0.9 / rate seconds.  In that time no token comes into a bucket
    that is full at the first request, so the statuses are exact.  Every
    attempt must start with full buckets: setup() or run() takes a new
    zone or a new client address from the attempt number.  A slow attempt
    says nothing about the limiter, so it is done again; the test fails
    when all the attempts are slow.
    """
    window = 0.9 / rate

    for attempt in range(attempts):
        if setup is not None:
            setup(attempt)

        start = time.monotonic()
        result = run(attempt)

        if time.monotonic() - start < window:
            return attempt, result

    pytest.fail(f'all {attempts} attempts took {window} s or more')


def zone(attempt, name=None):
    return f'{ZONE if name is None else name}-{attempt}'


def statuses(n, **kwargs):
    return [client.get(**kwargs)['status'] for _ in range(n)]


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
    def setup(attempt):
        load(limited(zone=zone(attempt), burst=3))

    def run(attempt):
        return [client.get() for _ in range(4)]

    attempt, resps = quick(run, 1, setup)

    assert [resp['status'] for resp in resps] == [200, 200, 200, 429]
    assert resps[3]['headers']['Retry-After'] == '1'
    assert 'Error 429' in resps[3]['body']

    c = counters(zone(attempt))
    assert c['passed'] == 3
    assert c['rejected'] == 1


def test_limit_req_retry_after():
    # One per second, 10 at once: one request fits again after one
    # second, not after the 10 seconds that fill the bucket, so
    # "Retry-After" is 1.  With 1 per 2 seconds not expressible, the
    # estimate cannot be tested above 1 here; the C test checks it.
    def setup(attempt):
        load(limited(zone=zone(attempt), burst=10))

    def run(attempt):
        return statuses(10), client.get()

    _, (passed, resp) = quick(run, 1, setup)

    assert passed == [200] * 10
    assert resp['status'] == 429
    assert resp['headers']['Retry-After'] == '1'


def test_limit_req_refill():
    def setup(attempt):
        load(limited(zone=zone(attempt), rate=2))

    assert quick(lambda attempt: statuses(2), 2, setup)[1] == [200, 429]

    # 0.6 s brings in at least one token: the bucket of one is full again.
    def wait(attempt):
        time.sleep(0.6)

    assert quick(lambda attempt: statuses(2), 2, wait)[1] == [200, 429]


def test_limit_req_per_client():
    load(limited(), {'pass': 'routes', 'client_ip': CLIENT_IP})

    def run(attempt):
        a = f'10.{attempt}.0.1'
        b = f'10.{attempt}.0.2'

        return [get_as(a), get_as(a), get_as(b), get_as(a)]

    v4, result = quick(run, 1)
    assert result == [200, 429, 200, 429]

    # IPv6 clients are keyed by their /64
    def run6(attempt):
        return [
            get_as(f'2001:db8:{attempt}::1'),
            get_as(f'2001:db8:{attempt}::2'),
            get_as(f'2001:db8:{attempt}:1::1'),
        ]

    v6, result = quick(run6, 1)
    assert result == [200, 429, 200]

    # Two keys for each attempt of each family.
    assert counters()['keys'] == 2 * (v4 + 1) + 2 * (v6 + 1)


def test_limit_req_untrusted_xff():
    # Without "client_ip" the header is not looked at: one peer, one key.
    def setup(attempt):
        load(limited(zone=zone(attempt)))

    def run(attempt):
        return [get_as('10.0.0.1'), get_as('10.0.0.2'), client.get()['status']]

    attempt, result = quick(run, 1, setup)
    assert result == [200, 429, 429]
    assert counters(zone(attempt))['keys'] == 1

    # With "client_ip" from a source that is not the peer: the same.
    def setup2(attempt):
        load(
            limited(zone=zone(attempt, f'{ZONE}-2')),
            {
                'pass': 'routes',
                'client_ip': {
                    'header': 'X-Forwarded-For',
                    'source': '10.0.0.0/8',
                },
            },
        )

    def run2(attempt):
        return [get_as('10.0.0.1'), get_as('10.0.0.2')]

    attempt, result = quick(run2, 1, setup2)
    assert result == [200, 429]
    assert counters(zone(attempt, f'{ZONE}-2'))['keys'] == 1


def partial_post():
    # The body never arrives.  A 429 comes at once; a request that passed
    # waits for its body, and nothing comes.
    sock = client.http(PARTIAL_POST, raw=True, no_recv=True)
    resp = client.recvall(sock, read_timeout=5).decode()
    sock.close()

    return client._resp_to_dict(resp)


def test_limit_req_headers_stage_post():
    def setup(attempt):
        load(limited(zone=zone(attempt), stage='headers'))

    def run(attempt):
        return client.get()['status'], partial_post()

    _, (first, resp) = quick(run, 1, setup)

    # The 429 arrives, and the connection is closed instead of waiting for
    # the 100 bytes.
    assert first == 200
    assert resp['status'] == 429
    assert resp['headers']['Retry-After'] == '1'
    assert resp['headers']['Connection'] == 'close'

    # The default stage is "headers".
    def setup2(attempt):
        load(limited(zone=zone(attempt, f'{ZONE}-2')))

    _, (first, resp) = quick(run, 1, setup2)
    assert first == 200
    assert resp['status'] == 429


def test_limit_req_body_stage_post():
    def setup(attempt):
        load(limited(zone=zone(attempt), stage='body'))

    def run(attempt):
        first = client.post(body='x' * 10)['status']

        # The body was read: nothing is left on the connection, it stays
        # open.
        resp = client.post(body='x' * 10, connection_close=False, framed=True)

        return first, resp

    _, (first, resp) = quick(run, 1, setup)
    assert first == 200
    assert resp['status'] == 429
    assert 'Connection' not in resp['headers']

    # The body is read first: a request without one waits, whatever the
    # state of the bucket.
    sock = client.http(PARTIAL_POST, raw=True, no_recv=True)

    assert client.recvall(sock, read_timeout=1) == b''

    sock.close()


def test_limit_req_keepalive():
    def setup(attempt):
        load(limited(zone=zone(attempt)))

    def run(attempt):
        # Without a body nothing is left on the connection: it stays open.
        first, sock = client.get(start=True, connection_close=False, framed=True)
        resp = client.get(sock=sock, connection_close=False, framed=True)
        sock.close()

        return first, resp

    _, (first, resp) = quick(run, 1, setup)
    assert first['status'] == 200
    assert resp['status'] == 429
    assert 'Connection' not in resp['headers']


def test_limit_req_reload():
    def run(attempt):
        name = zone(attempt)
        load(limited(zone=name))

        result = statuses(2)
        result.append(counters(name))

        # A new configuration with the same zone name and size: the zone,
        # its bucket and its counters stay.
        load(
            [{'match': {'uri': '/other'}, 'action': {'return': 204}}]
            + limited(zone=name)
        )

        result.append(counters(name))
        result.append(client.get()['status'])
        result.append(counters(name)['rejected'])

        # A new "burst" applies to the bucket as it is: still empty.
        load(limited(zone=name, burst=5))

        result.append(client.get()['status'])
        result.append(counters(name)['rejected'])

        return result

    attempt, result = quick(run, 1)
    name = zone(attempt)

    assert result[0:2] == [200, 429]
    assert result[2]['passed'] == 1
    assert result[2]['rejected'] == 1
    assert result[3] == {
        'keys': 1,
        'keys_max': 4096,
        'passed': 1,
        'rejected': 1,
        'evicted': 0,
    }
    assert result[4:6] == [429, 2]
    assert result[6:8] == [429, 3]

    # A new size is a new zone: the keys and the counters start again.
    load(limited(zone=f'{name}:8', burst=5))

    assert counters(name) == {
        'keys': 0,
        'keys_max': 8,
        'passed': 0,
        'rejected': 0,
        'evicted': 0,
    }

    assert client.get()['status'] == 200

    # A zone that no configuration names any more is gone.
    load(limited(zone=f'{ZONE}-other'))

    assert name not in client.conf_get('/status')['limit_req']

    load([{'action': {'return': 200}}])

    assert 'limit_req' not in client.conf_get('/status')


def test_limit_req_shared_zone():
    def setup(attempt):
        name = zone(attempt)

        load(
            [
                {
                    'match': {'uri': '/a'},
                    'action': {'limit_req': limit(zone=name), 'return': 200},
                },
                {
                    'match': {'uri': '/b'},
                    'action': {'limit_req': limit(zone=name), 'return': 200},
                },
            ]
        )

    def run(attempt):
        return [client.get(url='/a')['status'], client.get(url='/b')['status']]

    attempt, result = quick(run, 1, setup)
    assert result == [200, 429]

    c = counters(zone(attempt))
    assert c['passed'] == 1
    assert c['rejected'] == 1


def test_limit_req_pass_chain_counted_once():
    def setup(attempt):
        load(
            {
                'first': [{'action': {'pass': 'routes/second'}}],
                'second': limited(zone=zone(attempt), burst=2),
            },
            {'pass': 'routes/first'},
        )

    attempt, result = quick(lambda attempt: statuses(3), 1, setup)
    assert result == [200, 200, 429]

    c = counters(zone(attempt))
    assert c['passed'] == 2
    assert c['rejected'] == 1

    # Behind a rewrite the headers stage cannot see the limit; the body
    # stage counts the request, once.
    def setup2(attempt):
        load(
            {
                'first': [
                    {'action': {'rewrite': '/second', 'pass': 'routes/second'}}
                ],
                'second': limited(zone=zone(attempt, f'{ZONE}-2'), burst=2),
            },
            {'pass': 'routes/first'},
        )

    assert quick(lambda attempt: statuses(3), 1, setup2)[1] == [200, 200, 429]


def test_limit_req_eviction():
    load(limited(zone=f'{ZONE}:1'), {'pass': 'routes', 'client_ip': CLIENT_IP})

    def run(attempt):
        a = f'10.{attempt}.0.1'
        b = f'10.{attempt}.0.2'

        # The second client takes the one entry; the first one starts
        # afresh.
        return [get_as(a), get_as(a), get_as(b), get_as(a)]

    attempt, result = quick(run, 1)
    assert result == [200, 429, 200, 200]

    # Every request of an attempt but the second takes the entry of
    # another client: the first one evicts the client of the attempt
    # before.
    c = counters()
    assert c['keys'] == 1
    assert c['keys_max'] == 1
    assert c['evicted'] == 3 * (attempt + 1) - 1
    assert c['passed'] == 3 * (attempt + 1)
    assert c['rejected'] == attempt + 1


def test_limit_req_threads():
    # Four router threads, one zone: sixteen connections at once from one
    # client pass "burst" requests between them, not "burst" per thread.
    def setup(attempt):
        load(limited(zone=zone(attempt), burst=4), settings={'listen_threads': 4})

    def run(attempt):
        socks = []

        for _ in range(16):
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(('127.0.0.1', port_map.port(8080)))
            socks.append(sock)

        for sock in socks:
            sock.sendall(
                b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'
            )

        result = []

        for sock in socks:
            resp = client.recvall(sock).decode()
            result.append(int(resp.split(' ')[1]))
            sock.close()

        return result

    attempt, result = quick(run, 1, setup)

    assert result.count(200) == 4
    assert result.count(429) == 12

    c = counters(zone(attempt))
    assert c['passed'] == 4
    assert c['rejected'] == 12
    assert c['keys'] == 1


def test_limit_req_route_matched_once(require):
    require({'modules': {'njs': 'any'}})

    # The condition is true when a request evaluates it for the first
    # time, and false after that: the njs state lives as long as the
    # request.  The route handler must take the step that the headers
    # stage matched and whose limit counted the request.  A second match
    # takes the other step.
    first = '`${(globalThis.n = (globalThis.n || 0) + 1) == 1}`'

    routes = [
        {'match': {'if': first}, 'action': {'return': 200}},
        {'action': {'return': 204}},
    ]

    # Without a limit the condition is evaluated once.
    load(routes)
    assert client.get()['status'] == 200

    routes[0]['action']['limit_req'] = limit(burst=10)
    load(routes)

    assert client.get()['status'] == 200
    assert counters()['passed'] == 1


def test_limit_req_route_matched_after_rewrite():
    # The headers stage matches "/a" in "main" and stops at the rewrite.
    # After the rewrite "main" is matched again, with the new URI: the
    # step of "/a" taken again would loop.
    load(
        {
            'main': [
                {
                    'match': {'uri': '/a'},
                    'action': {'rewrite': '/b', 'pass': 'routes/main'},
                },
                {
                    'match': {'uri': '/b'},
                    'action': {'limit_req': limit(burst=10), 'return': 200},
                },
            ],
        },
        {'pass': 'routes/main'},
    )

    assert client.get(url='/a')['status'] == 200
    assert counters()['passed'] == 1
