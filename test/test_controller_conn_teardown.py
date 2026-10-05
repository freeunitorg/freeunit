import os
import pwd
import socket

import pytest

from unit.control import Control
from unit.option import option

prerequisites = {}

client = Control()


def _control_addr():
    return f'{option.temp_dir}/control.unit.sock'


def test_controller_body_alloc_failure(skip_alert):
    # A control request advertising a body far larger than the initial buffer
    # forces nxt_buf_mem_alloc() in the read handler to fail.  That path now
    # tears the connection down through nxt_controller_conn_close(), which
    # closes c->socket.fd, instead of leaking it via nxt_controller_conn_free().
    # Keep the client socket open and assert Unit closes its side (recv -> EOF):
    # on the leaking implementation the server-side fd is orphaned open and the
    # client never sees EOF, so this recv() times out and the test fails.
    sock = client.http(
        b'PUT /config HTTP/1.1\r\n'
        b'Host: localhost\r\n'
        b'Content-Length: 9223372036854775807\r\n'
        b'Connection: close\r\n\r\n',
        raw=True,
        no_recv=True,
        sock_type='unix',
        addr=_control_addr(),
    )

    sock.settimeout(5)
    assert sock.recv(1) == b'', 'controller closed the connection (no fd leak)'
    sock.close()

    assert client.conf_get(), 'controller still serves requests'


def test_controller_early_close_peer_cred(is_su, skip_alert):
    # Early-teardown coverage for the four nxt_controller_conn_init() failure
    # paths (which also switched from free to close): a peer-credential reject
    # tears the conn down via nxt_controller_conn_close() before the read is
    # armed.  As above, assert Unit closes its side (recv -> EOF) so the
    # free-without-close leak is caught on the early path too.  Needs root
    # (unitd runs as uid 0) plus a control socket an unprivileged uid can reach;
    # skips otherwise.
    if not is_su:
        pytest.skip('needs root to create a control-socket uid mismatch')

    skip_alert(r'controller: rejecting connection from uid')

    addr = _control_addr()

    try:
        nobody = pwd.getpwnam('nobody')
    except KeyError:
        pytest.skip('nobody user not found')

    results = []

    for _ in range(3):
        rfd, wfd = os.pipe()
        pid = os.fork()

        if pid == 0:
            os.close(rfd)
            rc = 1  # 1: could not reach the controller as an unprivileged uid

            try:
                os.setgroups([])
                os.setgid(nobody.pw_gid)
                os.setuid(nobody.pw_uid)

                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(addr)

                s.settimeout(1)
                try:
                    rc = 0 if s.recv(1) == b'' else 2  # 0: EOF, 2: no EOF (leak)
                except (OSError, socket.timeout):
                    rc = 2
                finally:
                    s.close()

            except OSError:
                rc = 1

            os.write(wfd, bytes([rc]))
            os._exit(0)

        os.close(wfd)

        res = os.read(rfd, 1)
        os.close(rfd)
        _, status = os.waitpid(pid, 0)

        assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, (
            'credential-test child exited unexpectedly'
        )
        assert res != b'', 'credential-test child produced no result'

        results.append(res)

    reached = [r for r in results if r in (b'\x00', b'\x02')]
    if not reached:
        pytest.skip('control socket not reachable by an unprivileged uid')

    assert all(r == b'\x00' for r in reached), (
        'controller closed the rejected connection (no fd leak)'
    )
    assert client.conf_get(), 'controller still alive after peer-cred rejects'
