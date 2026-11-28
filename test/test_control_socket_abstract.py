"""The control socket can be an abstract UNIX socket: "--control unix:@name".

nxt_sockaddr_parse() accepted the address, and nxt_listen_socket_create()
then ran chmod() and chown() on the socket path.  An abstract socket has no
path, so unitd failed to start with 'chmod("") failed'.

An abstract socket has no file permissions either: every local process in
the network namespace can connect to it.  The peer credential check of the
controller is the only access control, and the last two tests are about it.

Each test starts a unitd of its own, as test_control_socket_peer.py does.
"""

import os
import platform
import pwd
import shutil
import signal
import socket
import stat
import subprocess
import tempfile
import time
import uuid
from pathlib import Path

import pytest

from unit.option import option

pytestmark = pytest.mark.skipif(
    platform.system() != 'Linux', reason='abstract sockets are Linux only'
)

EXIT_TIMEOUT = 30


def _get(name, user=None):
    """The status line of "GET /config" on the abstract socket "name".

    With a user, a child with the credentials of that user sends the
    request.  Returns '' when unitd closed the connection with no response,
    or the connect() error.
    """

    def request():
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(5)

            try:
                s.connect(f'\0{name}')
            except OSError as e:
                return f'connect: {e.strerror}'

            data = b''

            # A rejected peer is closed right after accept(), so the send
            # can fail with EPIPE or ECONNRESET.  That is the same result
            # as a close with no response.
            try:
                s.sendall(
                    b'GET /config HTTP/1.1\r\nHost: localhost\r\n'
                    b'Connection: close\r\n\r\n'
                )

                while chunk := s.recv(4096):
                    data += chunk

            except OSError:
                pass

            return data.split(b'\r\n', 1)[0].decode()

    if user is None:
        return request()

    r, w = os.pipe()
    pid = os.fork()

    if pid == 0:
        os.close(r)

        try:
            os.setgroups([])
            os.setgid(user.pw_gid)
            os.setuid(user.pw_uid)

            result = request()

        except Exception as e:  # pylint: disable=broad-except
            result = f'error: {e}'

        os.write(w, result.encode())
        os._exit(0)

    os.close(w)

    with os.fdopen(r, 'rb') as pipe:
        result = pipe.read().decode()

    os.waitpid(pid, 0)

    return result


def _nobody():
    try:
        return pwd.getpwnam('nobody')
    except KeyError:
        pytest.skip('requires the "nobody" user')


@pytest.fixture
def unitd(request):
    binary = f'{option.current_dir}/build/sbin/unitd'

    if not Path(binary).is_file():
        pytest.skip('could not find unitd')

    started = []

    def start(*args, name=None, ready=True):
        tmp = tempfile.mkdtemp(prefix='unit-ctl-abstract-test-')
        Path(f'{tmp}/state').mkdir()

        if name is None:
            # Abstract names are global in the network namespace.
            name = f'unit-test-{os.getpid()}-{uuid.uuid4().hex[:8]}'

        # Before the log file is open, unitd writes to stderr.
        with open(f'{tmp}/stderr.log', 'w', encoding='utf-8') as stderr:
            proc = subprocess.Popen(
                [
                    binary,
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
                    f'unix:@{name}',
                    *args,
                ],
                stdout=subprocess.DEVNULL,
                stderr=stderr,
                start_new_session=True,
            )

        started.append((proc, tmp))

        if ready:
            deadline = time.monotonic() + EXIT_TIMEOUT

            while not _get(name).startswith('HTTP/1.1 200'):
                assert proc.poll() is None, 'unitd exited'
                assert time.monotonic() < deadline, 'control socket'

                time.sleep(0.1)

        return name, proc, tmp

    yield start

    for proc, tmp in started:
        if proc.poll() is None:
            proc.send_signal(signal.SIGQUIT)

            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                # unitd has child processes: kill the whole group.
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

                proc.wait()

        # Keep the logs of this unitd when the test failed.
        rep = getattr(request.node, 'rep_call', None)

        if rep is not None and rep.failed:
            for log in ('stderr.log', 'unit.log'):
                try:
                    text = Path(f'{tmp}/{log}').read_text(encoding='utf-8')
                    print(f'--- {tmp}/{log} ---\n{text}')
                except OSError:
                    pass

        shutil.rmtree(tmp, ignore_errors=True)


def _alerts(tmp):
    lines = []

    for log in ('stderr.log', 'unit.log'):
        try:
            text = Path(f'{tmp}/{log}').read_text(encoding='utf-8')
        except OSError:
            continue

        lines += [line for line in text.splitlines() if '[alert]' in line]

    return lines


# --control-mode does not apply to an abstract socket: with it the start
# must not try the chmod() either.
@pytest.mark.parametrize(
    'args', [(), ('--control-mode', '0666')], ids=['default', 'mode']
)
def test_control_socket_abstract(unitd, args):
    name, proc, tmp = unitd(*args)

    assert _get(name).startswith('HTTP/1.1 200'), 'request'

    # The socket is in the abstract namespace: no file is made for it.
    sockets = [
        entry.name
        for entry in os.scandir(tmp)
        if stat.S_ISSOCK(entry.stat(follow_symlinks=False).st_mode)
    ]

    assert sockets == [], 'no socket file'

    proc.send_signal(signal.SIGQUIT)

    assert proc.wait(timeout=EXIT_TIMEOUT) == 0, 'exit status'

    # Neither the chmod() and chown() at the start nor the unlink() at
    # exit may be tried on the empty path of the socket.
    assert _alerts(tmp) == [], 'no alerts'


def test_control_socket_abstract_in_use(unitd):
    name, _, _ = unitd()

    # The name is taken.  The second unitd fails to start and takes nothing
    # away from the first one.
    _, proc, tmp = unitd(name=name, ready=False)

    assert proc.wait(timeout=EXIT_TIMEOUT) == 1, 'exit status'

    alerts = _alerts(tmp)

    assert len(alerts) == 1, 'one alert'
    assert 'Address already in use' in alerts[0], 'reason'

    assert _get(name).startswith('HTTP/1.1 200'), 'first unitd'


def test_control_socket_abstract_peer_rejected(unitd, is_su):
    if not is_su:
        pytest.skip('requires root')

    user = _nobody()
    name, _, tmp = unitd()

    # No file permissions stop the connect().  The peer credential check
    # closes the connection before the request is read.
    assert _get(name, user) == '', 'unrelated user rejected'

    rejected = [
        line
        for line in _alerts(tmp)
        if f'rejecting connection from uid {user.pw_uid}' in line
    ]

    assert len(rejected) == 1, 'rejection logged'

    # There are no socket permissions to set here.  The alert has to name
    # what does accept a peer.
    assert '--control-user' in rejected[0], 'guidance, user'
    assert '--control-group' in rejected[0], 'guidance, group'

    assert _get(name).startswith('HTTP/1.1 200'), 'root'


def test_control_socket_abstract_peer_user(unitd, is_su):
    if not is_su:
        pytest.skip('requires root')

    user = _nobody()

    # There is no file to chown: --control-user only adds the user to the
    # peers that the controller accepts.
    name, _, _ = unitd('--control-user', user.pw_name)

    assert _get(name, user).startswith('HTTP/1.1 200'), 'control user'
    assert _get(name).startswith('HTTP/1.1 200'), 'root'
