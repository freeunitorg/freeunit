import ssl

import pytest

from unit import port as port_map
from unit.applications.tls import ApplicationTLS

prerequisites = {'modules': {'openssl': 'any'}}

client = ApplicationTLS()

# A session ticket key: 16 bytes of name, an AES key and an HMAC key.
KEY = 'U1oDTh11mMxODuw12gS0EXX1E/PkZG13cJNQ6m5+6BGlfPTjNlIEw7PSVU3X1gTE'

VERSIONS = [ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3]


def configure(listeners):
    assert 'success' in client.conf(
        {
            "listeners": {
                f"*:{port}": {
                    "pass": "routes",
                    "tls": {"certificate": cert, "session": session},
                }
                for port, (cert, session) in listeners.items()
            },
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    ), 'configure listeners'


@pytest.fixture(autouse=True)
def setup_method_fixture():
    client.certificate()
    client.certificate('other')


@pytest.mark.parametrize('version', VERSIONS, ids=lambda v: v.name)
def test_tls_session_id_context(version):
    # The listeners share a ticket key, so each can decrypt the tickets of
    # the others.  8080 and 8082 serve the same certificate, 8081 another
    # one.  A resumed handshake has no certificate exchange: if 8081 resumed
    # a session of 8080, the client would go on believing it talks to
    # "default".

    session = {"tickets": KEY}

    configure(
        {
            port_map.port(8080): ('default', session),
            port_map.port(8081): ('other', session),
            port_map.port(8082): ('default', session),
        }
    )

    ctx = client.context(version)

    sess, reused = client.get_session(ctx)
    assert not reused, 'new'
    assert sess.has_ticket, 'ticket issued'

    _, reused = client.get_session(ctx, sess)
    assert reused, 'resumed by the listener that issued the ticket'

    _, reused = client.get_session(ctx, sess, port=8082)
    assert reused, 'resumed by a listener with the same certificate'

    _, reused = client.get_session(ctx, sess, port=8081)
    assert not reused, 'full handshake under another certificate'


@pytest.mark.parametrize('version', VERSIONS, ids=lambda v: v.name)
def test_tls_session_id_context_certificate_replaced(version):
    # The same listener and ticket key; the certificate is replaced under
    # its name, as a renewal does.

    configure({port_map.port(8080): ('default', {"tickets": KEY})})

    ctx = client.context(version)

    sess, _ = client.get_session(ctx)

    _, reused = client.get_session(ctx, sess)
    assert reused, 'resumed before the replacement'

    client.certificate('default', False)
    assert 'success' in client.certificate_load('default')

    _, reused = client.get_session(ctx, sess)
    assert not reused, 'full handshake after the replacement'


def test_tls_session_id_context_cache():
    # The session cache checks the context too.  Tickets are off: a TLS 1.2
    # client resumes by session id, and a TLS 1.3 client gets a ticket that
    # only names a cache entry.

    configure({port_map.port(8080): ('default', {"cache_size": 16})})

    for version, tickets in zip(VERSIONS, (False, True)):
        ctx = client.context(version, tickets=tickets)

        sess, reused = client.get_session(ctx)
        assert not reused, f'{version.name} new'

        _, reused = client.get_session(ctx, sess)
        assert reused, f'{version.name} resumed from the cache'
