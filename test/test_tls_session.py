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

    # Complete one request before the session is used again.  OpenSSL
    # inserts a server session into the shared SSL_CTX cache from
    # tls_finish_handshake(), which runs after the Finished message it
    # flushed has already reached the client.  A handshake that has
    # returned on the client therefore proves nothing about the insert:
    # with more than one router engine the immediate reconnect can be
    # accepted by an engine that has not seen it yet, and a full
    # handshake results -- the flake tracked in issue #51.
    #
    # SSL_do_handshake() does not return on the server until the insert
    # has happened, and Unit does not read application data before it
    # returns (nxt_openssl_conn_handshake(), src/nxt_openssl.c), so a
    # response the client has read proves the session is in the cache.
    conn.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n')

    response = b''
    while b'\r\n\r\n' not in response:
        response += conn.recv(4096)

    assert response.startswith(b'HTTP/1.1 200'), 'request completed'

    conn.shutdown()

    return (
        conn,
        conn.get_session(),
        ctx,
        _lib.SSL_session_reused(conn._ssl),
    )


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

    _, _, _, reused = connect(ctx, sess)
    assert reused, 'cache'

    _, _, _, reused = connect(ctx, sess)
    assert reused, 'cache 2'

    # check that at least one session of four is not reused

    conns = [connect() for _ in range(4)]
    assert True not in [c[-1] for c in conns], 'cache small all new'

    conns_again = [connect(c[2], c[1]) for c in conns]
    assert False in [c[-1] for c in conns_again], 'cache small no reuse'

    # all four sessions are reused

    assert 'success' in add_session(cache_size=8)

    conns = [connect() for _ in range(4)]
    assert True not in [c[-1] for c in conns], 'cache big all new'

    conns_again = [connect(c[2], c[1]) for c in conns]
    assert False not in [c[-1] for c in conns_again], 'cache big reuse'


@pytest.mark.skipif(
    not hasattr(_lib, 'SSL_session_reused'),
    reason='session reuse is not supported',
)
def test_tls_session_timeout():
    # The two halves are configured separately rather than fitted into one
    # window.  "Still cached" only needs a timeout no run can outlast, and
    # "evicted" only needs one every run outlasts; sharing a window made
    # each assertion depend on the other's margin, and a slow machine
    # broke whichever half it reached first.  Configured apart, a slow
    # machine makes the eviction half more certain rather than less.

    assert 'success' in add_session(cache_size=5, timeout=300)

    _, sess, ctx, reused = connect()
    assert not reused, 'new connection'

    _, _, _, reused = connect(ctx, sess)
    assert reused, 'no timeout'

    assert 'success' in add_session(cache_size=5, timeout=1)

    # The reconfiguration above rebuilds the TLS context and with it the
    # session cache, so the session that has to outlive the timeout is
    # established after it, not before.
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
