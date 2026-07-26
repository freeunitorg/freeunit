import socket
from pathlib import Path

import pytest

from unit.applications.proto import ApplicationProto
from unit.status import Status

client = ApplicationProto()

BODY = '0123456789'


@pytest.fixture(autouse=True)
def setup_method_fixture(temp_dir, skip_fds_check):
    # Thread-count changes intentionally add and remove each engine's epoll,
    # signalfd, and eventfd descriptors, so the generic router FD baseline is
    # not meaningful for this test.  The recycler itself owns no descriptors.
    skip_fds_check(router=True)

    Path(f'{temp_dir}/assets').mkdir(parents=True)
    Path(f'{temp_dir}/assets/index.html').write_text(BODY, encoding='utf-8')

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"share": f'{temp_dir}/assets$uri'}}],
            "settings": {"listen_threads": 4},
        }
    )

    yield

    # The no-restart suite preserves /settings between tests.
    assert 'success' in client.conf_delete('settings/listen_threads')


def _churn(n):
    # Each short-lived connection is a nxt_conn_create (freelist pop) +
    # nxt_conn_free (freelist push) cycle; spread across the listen_threads
    # engines this fills each engine's connection freelist.  A corrupted
    # recycle returns a wrong/truncated body or, under the sanitizer, crashes
    # the worker.
    for i in range(n):
        sock = socket.create_connection(('127.0.0.1', 8080))
        sock.settimeout(5)
        try:
            sock.sendall(
                b'GET / HTTP/1.1\r\nHost: localhost\r\n'
                b'Connection: close\r\n\r\n'
            )
            data = b''
            while BODY.encode() not in data:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
            assert BODY.encode() in data, f'body on connection {i}'
        finally:
            sock.close()


def test_conn_recycle_across_thread_churn():
    # Populate the per-engine connection freelists.
    _churn(120)

    # Lowering listen_threads destroys worker engines, freeing their populated
    # connection freelists (engine->mem_pool teardown); raising it recreates
    # them.  Repeat so structs are recycled, then torn down with the engine.
    for _ in range(2):
        assert 'success' in client.conf('1', 'settings/listen_threads')
        _churn(60)
        assert 'success' in client.conf('4', 'settings/listen_threads')
        _churn(60)


def test_conn_recycle_connection_accounting():
    # The body-integrity churn above passes identically whether structs are
    # recycled, parked forever, or never pushed at all, so it cannot see the
    # recycler stop working -- notably pending_connections growing without
    # bound because some struct never settles.  Pin the counters the recycler
    # now sits beside instead: every churned connection must be accounted for
    # and none may be left behind once the churn is over.
    Status.init()

    _churn(60)

    assert Status.get('/connections/accepted') == 60, 'all accepted'
    assert Status.get('/connections/active') == 0, 'none left active'
    assert Status.get('/connections/idle') == 0, 'none left idle'
    assert Status.get('/connections/closed') == 60, 'all closed'
