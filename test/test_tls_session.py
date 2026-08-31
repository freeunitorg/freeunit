import socket
import time

import pytest

pytest.importorskip('OpenSSL.SSL')
from OpenSSL.SSL import (
    TLSv1_2_METHOD,
    SESS_CACHE_CLIENT,
    OP_NO_TICKET,
    Context,
    Connection,
    _lib,
)
from unit.applications.tls import ApplicationTLS

prerequisites = {'modules': {'openssl': 'any'}}

client = ApplicationTLS()


@pytest.fixture(autouse=True)
def setup_method_fixture():
    client.certificate()

    assert 'success' in client.conf(
        {
            "listeners": {
                "*:8080": {
                    "pass": "routes",
                    "tls": {"certificate": "default", "session": {}},
                }
            },
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    ), 'load application configuration'


def add_session(cache_size=None, timeout=None):
    session = {}

    if cache_size is not None:
        session['cache_size'] = cache_size
    if timeout is not None:
        session['timeout'] = timeout

    return client.conf(session, 'listeners/*:8080/tls/session')


def connect(ctx=None, session=None):
    sock = socket.create_connection(('127.0.0.1', 8080))

    if ctx is None:
        ctx = Context(TLSv1_2_METHOD)
        ctx.set_session_cache_mode(SESS_CACHE_CLIENT)
        ctx.set_options(OP_NO_TICKET)

    conn = Connection(ctx, sock)
    conn.set_connect_state()

    if session is not None:
        conn.set_session(session)

    conn.do_handshake()
    conn.shutdown()

    return (
        conn,
        conn.get_session(),
        ctx,
        _lib.SSL_session_reused(conn._ssl),
    )


def resume(ctx, session, tries=5, delay=0.1):
    """Reconnect until the session is served from the cache.

    OpenSSL adds a session to the server cache after flushing its Finished
    message, so a resume attempted in the same instant the handshake
    completed can arrive before the insert.  With more than one router
    engine the reconnect may be accepted by an engine that has not seen it
    yet, and a full handshake results.

    Unit does not promise that a session is resumable in the instant it is
    established -- only that an unexpired one resumes -- so poll for the
    property that actually holds instead of racing the one that does not.
    A miss on the first attempt is the whole of the effect in practice;
    the retries are bounded low because every full handshake inserts a
    session of its own and would otherwise churn a small cache.
    """
    for _ in range(tries):
        _, _, _, reused = connect(ctx, session)

        if reused:
            return True

        time.sleep(delay)

    return False


@pytest.mark.skipif(
    not hasattr(_lib, 'SSL_session_reused'),
    reason='session reuse is not supported',
)
def test_tls_session():
    _, sess, ctx, reused = connect()
    assert not reused, 'new connection'

    _, _, _, reused = connect(ctx, sess)
    assert not reused, 'no cache'

    assert 'success' in add_session(cache_size=2)

    _, sess, ctx, reused = connect()
    assert not reused, 'new connection cache'

    assert resume(ctx, sess), 'cache'

    assert resume(ctx, sess), 'cache 2'

    # check that at least one session of four is not reused

    conns = [connect() for _ in range(4)]
    assert True not in [c[-1] for c in conns], 'cache small all new'

    conns_again = [connect(c[2], c[1]) for c in conns]
    assert False in [c[-1] for c in conns_again], 'cache small no reuse'

    # all four sessions are reused

    assert 'success' in add_session(cache_size=8)

    conns = [connect() for _ in range(4)]
    assert True not in [c[-1] for c in conns], 'cache big all new'

    assert all(resume(c[2], c[1]) for c in conns), 'cache big reuse'


@pytest.mark.skipif(
    not hasattr(_lib, 'SSL_session_reused'),
    reason='session reuse is not supported',
)
def test_tls_session_timeout():
    # The two halves are configured separately rather than fitted into one
    # window.  "Still cached" only needs a timeout no run can outlast, and
    # "evicted" only needs one every run outlasts; sharing a window makes
    # each assertion depend on the other's margin, which is what made this
    # flaky.  Slow machines make the eviction half more certain, not less.

    assert 'success' in add_session(cache_size=5, timeout=300)

    _, sess, ctx, reused = connect()
    assert not reused, 'new connection'

    assert resume(ctx, sess), 'no timeout'

    assert 'success' in add_session(cache_size=5, timeout=1)

    # The reconfiguration above rebuilds the TLS context and with it the
    # session cache, so the session that outlives the timeout has to be
    # established after it.
    _, sess, ctx, reused = connect()
    assert not reused, 'new connection timeout'

    time.sleep(3)

    _, _, _, reused = connect(ctx, sess)
    assert not reused, 'timeout'


def test_tls_session_invalid():
    assert 'error' in add_session(cache_size=-1)
    assert 'error' in add_session(cache_size={})
    assert 'error' in add_session(timeout=-1)
    assert 'error' in add_session(timeout={})
