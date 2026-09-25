"""The read-only status socket ("--status").

The second control socket serves "GET /status" and its subpaths only.  It
exists so that a process which must not change the configuration, such as
a web application worker, can still read /status.  Every other method is
refused with 405 and every other path with 404, and the control socket
keeps working as before.

conftest's shared unitd has no way to take extra options, so these tests
start a unitd of their own.
"""

import json
import os
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pytest

from unit.control import Control
from unit.option import option
from unit.utils import public_dir, waitforfiles

client = Control()


class Unitd:
    def __init__(self, *extra):
        self.unitd = f'{option.current_dir}/build/sbin/unitd'

        if not Path(self.unitd).is_file():
            pytest.skip('could not find unitd')

        self.dir = tempfile.mkdtemp(prefix='unit-test-status-')
        public_dir(self.dir)

        Path(f'{self.dir}/state').mkdir()

        self.control = f'{self.dir}/control.unit.sock'
        self.status = f'{self.dir}/status.unit.sock'
        self.log = f'{self.dir}/unit.log'

        env = os.environ.copy()
        pythonhome = env.pop('UNIT_PYTHONHOME', None)
        if pythonhome:
            env['PYTHONHOME'] = pythonhome

        self.log_file = open(self.log, 'w', encoding='utf-8')

        self.process = subprocess.Popen(
            [
                self.unitd,
                '--no-daemon',
                '--modulesdir',
                f'{option.current_dir}/build/lib/unit/modules',
                '--statedir',
                f'{self.dir}/state',
                '--pid',
                f'{self.dir}/unit.pid',
                '--log',
                self.log,
                '--control',
                f'unix:{self.control}',
                '--tmpdir',
                self.dir,
                *[a.replace('@', self.dir) for a in extra],
            ],
            stdout=subprocess.DEVNULL,
            stderr=self.log_file,
            start_new_session=True,
            env=env,
        )

        assert waitforfiles(self.control), 'unitd did not start'

    def stop(self):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGQUIT)

            try:
                self.process.wait(timeout=10)

            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=10)

        self.log_file.close()

    def cleanup(self):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait(timeout=10)

        if not self.log_file.closed:
            self.log_file.close()

        shutil.rmtree(self.dir, ignore_errors=True)


@pytest.fixture
def unitd():
    started = []

    def factory(*extra):
        instance = Unitd(*extra)
        started.append(instance)
        return instance

    yield factory

    for instance in started:
        instance.cleanup()


def req(method, url, addr, body=None, headers=None, sock_type='unix',
        port=None):
    args = {
        'url': url,
        'sock_type': sock_type,
        'addr': addr,
    }

    if port is not None:
        args['port'] = port

    if body is not None:
        args['body'] = body

    if headers is not None:
        args['headers'] = headers

    return getattr(client, method)(**args)


def status_unit(unitd, *extra):
    unit = unitd('--status', 'unix:@/status.unit.sock', *extra)

    assert waitforfiles(unit.status), 'no status socket'

    return unit


def test_status_socket_get_status(unitd):
    unit = status_unit(unitd)

    resp = req('get', '/status', unit.status)
    assert resp['status'] == 200, 'GET /status'

    body = json.loads(resp['body'])
    assert 'connections' in body, 'status body'
    assert 'requests' in body, 'status body requests'

    resp = req('get', '/status/connections', unit.status)
    assert resp['status'] == 200, 'GET /status/connections'
    assert 'accepted' in json.loads(resp['body']), 'connections body'

    resp = req('get', '/status/', unit.status)
    assert resp['status'] == 200, 'GET /status/'

    # The path is normalized before the check.
    resp = req('get', '/config/../status', unit.status)
    assert resp['status'] == 200, 'GET /config/../status'

    resp = req('get', '/status/nonexistent', unit.status)
    assert resp['status'] == 404, 'GET /status/nonexistent'


def test_status_socket_refuses_the_rest(unitd):
    unit = status_unit(unitd)

    conf = '{"listeners":{},"routes":[],"applications":{}}'

    for method, url, body, code in [
        ('put', '/config', conf, 405),
        ('post', '/config/routes', '{}', 405),
        ('delete', '/config', None, 405),
        ('delete', '/status', None, 405),
        ('put', '/status', '{}', 405),
        ('get', '/config', None, 404),
        ('get', '/', None, 404),
        ('get', '/certificates', None, 404),
        ('get', '/js_modules', None, 404),
        ('get', '/control/applications/x/restart', None, 404),
        ('get', '/statusx', None, 404),
        ('get', '/status/../config', None, 404),
        ('get', '/%63onfig', None, 404),
    ]:
        resp = req(method, url, unit.status, body=body)
        assert resp['status'] == code, f'{method.upper()} {url}'

    # The configuration did not change.

    resp = req('get', '/config', unit.control)
    assert resp['status'] == 200, 'control GET /config'
    assert json.loads(resp['body']) == {
        'listeners': {},
        'routes': [],
        'applications': {},
    }, 'config unchanged'


def test_status_socket_no_body_read(unitd):
    """A PUT with a huge Content-Length is refused at once: the status
    socket never reads a request body."""
    unit = status_unit(unitd)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect(unit.status)
    sock.sendall(
        b'PUT /config HTTP/1.1\r\nHost: localhost\r\n'
        b'Content-Length: 1000000000\r\n\r\n{'
    )

    data = b''
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk

    sock.close()

    assert data.startswith(b'HTTP/1.1 405 '), 'refused without the body'


def test_status_socket_control_unchanged(unitd):
    unit = status_unit(unitd)

    conf = {
        'listeners': {},
        'routes': [{'action': {'return': 204}}],
        'applications': {},
    }

    resp = req('put', '/config', unit.control, body=json.dumps(conf))
    assert resp['status'] == 200, 'control PUT /config'

    resp = req('get', '/config', unit.control)
    assert json.loads(resp['body']) == conf, 'control GET /config'

    resp = req('get', '/status', unit.control)
    assert resp['status'] == 200, 'control GET /status'

    resp = req('get', '/', unit.control)
    assert resp['status'] == 200, 'control GET /'

    resp = req('delete', '/config/routes', unit.control)
    assert resp['status'] == 200, 'control DELETE'


def test_status_socket_absent_by_default(unitd):
    unit = unitd()

    resp = req('get', '/status', unit.control)
    assert resp['status'] == 200, 'control GET /status'

    assert not Path(unit.status).exists(), 'no status socket'
    assert not list(Path(unit.dir).glob('status*')), 'no status files'


def test_status_socket_mode(unitd):
    unit = status_unit(unitd)

    mode = stat.S_IMODE(os.stat(unit.status).st_mode)
    assert mode == 0o600, 'default mode'

    mode = stat.S_IMODE(os.stat(unit.control).st_mode)
    assert mode == 0o600, 'control default mode'

    unit.cleanup()

    unit = status_unit(unitd, '--status-mode', '0660')

    mode = stat.S_IMODE(os.stat(unit.status).st_mode)
    assert mode == 0o660, 'status mode'

    mode = stat.S_IMODE(os.stat(unit.control).st_mode)
    assert mode == 0o600, 'control mode is not affected'


def test_status_socket_user_group(unitd):
    if os.geteuid() != 0:
        pytest.skip('requires root')

    unit = status_unit(
        unitd, '--status-user', 'nobody', '--status-group', 'nogroup'
    )

    import grp
    import pwd

    st = os.stat(unit.status)
    assert st.st_uid == pwd.getpwnam('nobody').pw_uid, 'status user'
    assert st.st_gid == grp.getgrnam('nogroup').gr_gid, 'status group'

    st = os.stat(unit.control)
    assert st.st_uid == 0, 'control user is not affected'
    assert st.st_gid == 0, 'control group is not affected'


def as_nobody(path, request):
    """Send a request from uid "nobody" and return the raw response."""
    script = (
        'import socket, sys\n'
        's = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n'
        's.settimeout(5)\n'
        's.connect(sys.argv[1])\n'
        's.sendall(sys.argv[2].encode())\n'
        'd = b""\n'
        'while True:\n'
        '    c = s.recv(4096)\n'
        '    if not c:\n'
        '        break\n'
        '    d += c\n'
        'sys.stdout.write(d.decode(errors="replace"))\n'
    )

    out = subprocess.run(
        [sys.executable, '-c', script, path, request],
        user='nobody',
        capture_output=True,
        timeout=10,
        check=False,
    )

    return out.stdout.decode()


def test_status_socket_other_user(unitd):
    """A peer with another uid may read /status through the status
    socket.  The control socket still rejects it, even when its file
    mode would let it connect."""
    if os.geteuid() != 0:
        pytest.skip('requires root')

    unit = status_unit(unitd, '--status-mode', '0666')

    os.chmod(unit.control, 0o666)

    get = 'GET /status HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'

    resp = as_nobody(unit.status, get)
    assert resp.startswith('HTTP/1.1 200 '), 'nobody reads status'

    put = (
        'PUT /config/routes HTTP/1.1\r\nHost: localhost\r\n'
        'Connection: close\r\nContent-Length: 2\r\n\r\n[]'
    )

    resp = as_nobody(unit.status, put)
    assert resp.startswith('HTTP/1.1 405 '), 'nobody cannot PUT'

    resp = as_nobody(unit.control, get)
    assert resp == '', 'control socket rejects nobody'


def test_status_socket_tcp(unitd):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()

    unit = unitd('--status', f'127.0.0.1:{port}')

    for _ in range(50):
        try:
            socket.create_connection(('127.0.0.1', port), timeout=1).close()
            break

        except OSError:
            time.sleep(0.1)

    resp = req('get', '/status', '127.0.0.1', sock_type='ipv4', port=port)
    assert resp['status'] == 200, 'tcp GET /status'

    resp = req('get', '/config', '127.0.0.1', sock_type='ipv4', port=port)
    assert resp['status'] == 404, 'tcp GET /config'

    resp = req(
        'put', '/config', '127.0.0.1', sock_type='ipv4', port=port, body='{}'
    )
    assert resp['status'] == 405, 'tcp PUT /config'


def test_status_socket_same_as_control(unitd):
    """The status socket may not take over the control socket."""
    unit = unitd('--status', 'unix:@/control.unit.sock')

    unit.process.wait(timeout=10)
    assert unit.process.returncode != 0, 'unitd refused to start'


def test_status_socket_removed_on_exit(unitd):
    unit = status_unit(unitd)

    unit.stop()

    assert not Path(unit.status).exists(), 'status socket removed'
    assert not Path(unit.control).exists(), 'control socket removed'
