import ssl
import time

import pytest

from unit.applications.tls import ApplicationTLS
from unit.option import option
from unit import port as port_map

prerequisites = {'modules': {'openssl': 'any'}}

client = ApplicationTLS()

# Session ticket keys: 16 bytes of name, an AES key and an HMAC key.
KEY1 = 'U1oDTh11mMxODuw12gS0EXX1E/PkZG13cJNQ6m5+6BGlfPTjNlIEw7PSVU3X1gTE'
KEY2 = '5AV0DSYIYbZWZQB7fCnTHZmMxtotb/aXjam+n2XS79lTvX3Tq9xGqpC8XKNEF2lt'

VERSIONS = [ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3]

# The windows of a key by its age, to the millisecond, are checked with a
# test clock in src/test/nxt_openssl_server_init_test.c.  A test here either
# waits for at least some time or rotates the keys by a reconfiguration, so
# a slow machine cannot fail it.


def conf(listeners):
    return {
        "listeners": {
            f"*:{port}": {
                "pass": "routes",
                "tls": {"certificate": "default", "session": session},
            }
            for port, session in listeners.items()
        },
        "routes": [{"action": {"return": 200}}],
        "applications": {},
    }


def configure(listeners):
    assert 'success' in client.conf(conf(listeners)), 'configure listeners'


def rotation(tickets, lifetime):
    return {"tickets": tickets, "ticket_lifetime": lifetime}


@pytest.fixture(autouse=True)
def setup_method_fixture():
    client.certificate()


def test_tls_ticket_rotation():
    # A key is dropped two lifetimes after it was created: its tickets then
    # get a full handshake, not an alert.  "timeout" is at its default of
    # 300 seconds, so it is the key that ends the tickets here.

    configure({port_map.port(8080): rotation(True, 1)})

    sessions = []

    for version in VERSIONS:
        ctx = client.context(version)

        sess, reused = client.get_session(ctx)
        assert not reused, f'{version.name} new'
        assert sess.has_ticket, f'{version.name} ticket issued'

        sessions.append((version, ctx, sess))

    time.sleep(2.2)

    for version, ctx, sess in sessions:
        _, reused = client.get_session(ctx, sess)
        assert not reused, f'{version.name} full handshake, the key is gone'


@pytest.mark.parametrize('version', VERSIONS, ids=lambda v: v.name)
def test_tls_ticket_rotation_reconfigure(version):
    # A reconfiguration builds a new TLS context.  The keys belong to the
    # listener, so the tickets survive it; the key that OpenSSL generates
    # for "tickets": true without "ticket_lifetime" does not.

    configure({port_map.port(8080): rotation(True, 300)})

    ctx = client.context(version)

    sess, reused = client.get_session(ctx)
    assert not reused, 'new'

    assert 'success' in client.conf('[{"action": {"return": 200}}]', 'routes')

    _, reused = client.get_session(ctx, sess)
    assert reused, 'resumed after a reconfiguration'

    assert 'success' in client.conf(
        '600', 'listeners/*:8080/tls/session/ticket_lifetime'
    )

    _, reused = client.get_session(ctx, sess)
    assert reused, 'resumed after a change of the lifetime'


@pytest.mark.parametrize('version', VERSIONS, ids=lambda v: v.name)
def test_tls_ticket_rotation_configured_keys(version):
    # A key from the configuration is replaced.  8080 rotates; 8081 and 8082
    # have one static key each and tell which key encrypted a ticket.

    configure(
        {
            port_map.port(8080): rotation(KEY1, 300),
            port_map.port(8081): {"tickets": KEY1},
            port_map.port(8082): {"tickets": KEY2},
        }
    )

    ctx = client.context(version)

    old, reused = client.get_session(ctx)
    assert not reused, 'new'

    _, reused = client.get_session(ctx, old, port=8081)
    assert reused, 'ticket of the configured key'

    assert 'success' in client.conf(
        f'"{KEY2}"', 'listeners/*:8080/tls/session/tickets'
    ), 'replace the key'

    new, reused = client.get_session(ctx)
    assert not reused, 'new after the replacement'

    _, reused = client.get_session(ctx, new, port=8082)
    assert reused, 'the new key issues'

    _, reused = client.get_session(ctx, old)
    assert reused, 'the key that left the configuration still decrypts'


def test_tls_ticket_rotation_configured_key_stale(wait_for_record):
    # A configured key that nobody replaces stops issuing after one lifetime
    # like any other key.  A generated key takes over, with a warning.

    configure(
        {
            port_map.port(8080): rotation(KEY1, 1),
            port_map.port(8081): {"tickets": KEY1},
        }
    )

    time.sleep(1.2)

    ctx = client.context(ssl.TLSVersion.TLSv1_3)

    sess, _ = client.get_session(ctx)

    # Keep the generated key for the checks below.
    assert 'success' in client.conf(
        '300', 'listeners/*:8080/tls/session/ticket_lifetime'
    )

    _, reused = client.get_session(ctx, sess, port=8081)
    assert not reused, 'not a ticket of the configured key'

    _, reused = client.get_session(ctx, sess)
    assert reused, 'a ticket of the generated key'

    assert (
        wait_for_record(r'\[warn\].+key is older than "ticket_lifetime"')
        is not None
    ), 'the stale key is logged'


def test_tls_ticket_rotation_expired_configured_key():
    # A configured key that expired is not taken back by a reconfiguration
    # that still lists it: its old tickets would work again.

    configure(
        {
            port_map.port(8080): rotation(KEY1, 1),
            port_map.port(8081): {"tickets": KEY1},
        }
    )

    ctx = client.context(ssl.TLSVersion.TLSv1_3)

    old, _ = client.get_session(ctx)

    time.sleep(2.2)

    assert 'success' in client.conf('[{"action": {"return": 200}}]', 'routes')

    _, reused = client.get_session(ctx, old)
    assert not reused, 'a ticket of the expired key after a reconfiguration'

    # A longer lifetime does not bring the key back either.
    assert 'success' in client.conf(
        '300', 'listeners/*:8080/tls/session/ticket_lifetime'
    )

    _, reused = client.get_session(ctx, old)
    assert not reused, 'a ticket of the expired key after a longer lifetime'

    new, _ = client.get_session(ctx)

    _, reused = client.get_session(ctx, new, port=8081)
    assert not reused, 'the expired key does not issue'


def test_tls_ticket_rotation_rejected_configuration(skip_alert):
    # The router rejects a configuration after it has set up the TLS
    # listeners: here the access log cannot be opened.  The key of that
    # configuration must not reach the keys of the running one.

    skip_alert(r'failed to apply new conf')

    configure(
        {
            port_map.port(8080): rotation(KEY1, 300),
            port_map.port(8081): {"tickets": KEY2},
        }
    )

    ctx = client.context(ssl.TLSVersion.TLSv1_3)

    old, _ = client.get_session(ctx)

    rejected = conf(
        {
            port_map.port(8080): rotation(KEY2, 1),
            port_map.port(8081): {"tickets": KEY2},
        }
    )
    rejected["access_log"] = f'{option.temp_dir}/blah/access.log'

    assert 'error' in client.conf(rejected), 'the configuration is rejected'

    new, reused = client.get_session(ctx)
    assert not reused, 'new'

    _, reused = client.get_session(ctx, new, port=8081)
    assert not reused, 'the key of the rejected configuration does not issue'

    _, reused = client.get_session(ctx, old)
    assert reused, 'a ticket of the running configuration still resumes'


def test_tls_ticket_rotation_invalid():
    configure({port_map.port(8080): {"tickets": True}})

    def check(session):
        assert 'error' in client.conf(
            session, 'listeners/*:8080/tls/session'
        ), session

    check(rotation(True, 0))
    check(rotation(True, -1))
    check(rotation(True, 1.5))
    check(rotation(True, "60"))
    check(rotation(True, 2**31))

    # Nothing to rotate.
    check({"ticket_lifetime": 60})
    check(rotation(False, 60))
    check(rotation([], 60))

    # More keys than a rotating listener keeps a record of.
    check(rotation([KEY1] * 9, 60))

    assert 'success' in client.conf(
        rotation([KEY1, KEY2], 60), 'listeners/*:8080/tls/session'
    )
