import grp
import os
import pwd
import shutil
import signal
import socket
import subprocess
import tempfile

import pytest

from unit.option import option
from unit.utils import waitforfiles

prerequisites = {'privileged_user': True}


def _peer_ids():
    try:
        user = pwd.getpwnam('nobody')
    except KeyError:
        pytest.skip('requires the "nobody" user')

    for name in ('nogroup', 'nobody'):
        try:
            return user, grp.getgrnam(name)
        except KeyError:
            pass

    pytest.skip('requires the "nogroup" or "nobody" group')


def _get(sock_path, uid=None, gid=None, groups=None):
    """GET / on the control socket from a child with the given credentials.

    Returns the status line, '' when the server closed the connection
    without a response, or the connect() error name.
    """
    r, w = os.pipe()
    pid = os.fork()

    if pid == 0:
        os.close(r)
        result = b''

        try:
            if groups is not None:
                os.setgroups(groups)
            if gid is not None:
                os.setgid(gid)
            if uid is not None:
                os.setuid(uid)

            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(5)

            try:
                s.connect(sock_path)
            except OSError as e:
                result = f'connect: {e.strerror}'.encode()
            else:
                s.sendall(
                    b'GET / HTTP/1.1\r\nHost: localhost\r\n'
                    b'Connection: close\r\n\r\n'
                )
                data = b''

                try:
                    while True:
                        chunk = s.recv(4096)
                        if not chunk:
                            break
                        data += chunk
                except OSError:
                    pass

                result = data.split(b'\r\n', 1)[0]

            s.close()

        except Exception as e:  # pylint: disable=broad-except
            result = f'error: {e}'.encode()

        os.write(w, result)
        os._exit(0)

    os.close(w)
    data = b''

    while True:
        chunk = os.read(r, 4096)
        if not chunk:
            break
        data += chunk

    os.close(r)
    os.waitpid(pid, 0)

    return data.decode()


@pytest.fixture
def control_unitd():
    instances = []

    def run(*args):
        tmp = tempfile.mkdtemp(prefix='unit-ctl-test-')
        os.chmod(tmp, 0o755)

        sock = f'{tmp}/control.unit.sock'
        unitd = f'{option.current_dir}/build/sbin/unitd'

        with open(f'{tmp}/unit.log', 'w', encoding='utf-8') as log:
            proc = subprocess.Popen(
                [
                    unitd,
                    '--no-daemon',
                    '--modulesdir',
                    f'{tmp}/modules',
                    '--statedir',
                    f'{tmp}/state',
                    '--pid',
                    f'{tmp}/unit.pid',
                    '--log',
                    f'{tmp}/unit.log',
                    '--tmpdir',
                    tmp,
                    '--control',
                    f'unix:{sock}',
                    *args,
                ],
                stderr=log,
            )

        instances.append((proc, tmp))

        assert waitforfiles(sock), 'control socket'

        return sock

    yield run

    for proc, tmp in instances:
        proc.send_signal(signal.SIGQUIT)

        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

        shutil.rmtree(tmp, ignore_errors=True)


def test_control_socket_peer_default(control_unitd):
    user, _ = _peer_ids()

    # World-writable socket without delegation: the kernel allows the
    # connect(), but the peer credential check must still reject it.
    sock = control_unitd('--control-mode', '0666')

    assert _get(sock).startswith('HTTP/1.1 200'), 'root'
    assert (
        _get(sock, uid=user.pw_uid, gid=user.pw_gid, groups=[]) == ''
    ), 'unrelated user rejected'


def test_control_socket_peer_user(control_unitd):
    user, _ = _peer_ids()

    sock = control_unitd(
        '--control-user', user.pw_name, '--control-mode', '0600'
    )

    assert _get(sock).startswith('HTTP/1.1 200'), 'root'
    assert _get(
        sock, uid=user.pw_uid, gid=user.pw_gid, groups=[]
    ).startswith('HTTP/1.1 200'), 'control user'


def test_control_socket_peer_group(control_unitd):
    user, group = _peer_ids()

    sock = control_unitd(
        '--control-group', group.gr_name, '--control-mode', '0660'
    )

    other_gid = 12345 if group.gr_gid != 12345 else 12346

    assert _get(
        sock, uid=user.pw_uid, gid=group.gr_gid, groups=[]
    ).startswith('HTTP/1.1 200'), 'primary group'

    # Supplementary groups come from SO_PEERGROUPS (Linux 4.13+).
    if os.uname().sysname == 'Linux':
        assert _get(
            sock, uid=user.pw_uid, gid=other_gid, groups=[group.gr_gid]
        ).startswith('HTTP/1.1 200'), 'supplementary group'

    assert _get(
        sock, uid=user.pw_uid, gid=other_gid, groups=[]
    ).startswith('connect:'), 'not in group'
