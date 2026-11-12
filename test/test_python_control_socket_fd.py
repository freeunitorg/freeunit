"""Only main and the controller may hold the control listen socket.

main creates the control socket before it forks the other processes.
SOCK_CLOEXEC closes it on execve() only, and an embedded module never
calls execve().  This test lists /proc/<pid>/fd of the router, the
prototype and one application worker and looks for the socket inode of
the listening control socket.  It does the same after main starts a new
controller and a new router, and checks that the control API answers.
It prints what it saw ("control-fd:" lines, run with -s), then asserts.
"""

import os
import signal
import time
from pathlib import Path

import pytest

from unit.applications.lang.python import ApplicationPython
from unit.option import option

prerequisites = {'modules': {'python': 'any'}}

pytestmark = pytest.mark.skipif(
    option.system != 'Linux', reason='/proc is Linux only'
)

client = ApplicationPython()

SO_ACCEPTCON = 0x10000


def ppid_of(pid):
    stat = Path(f'/proc/{pid}/stat').read_text(
        encoding='utf-8', errors='backslashreplace'
    )
    # The command name is in parentheses and can hold spaces.
    return int(stat.rsplit(')', 1)[1].split()[1])


def title_of(pid):
    """The title of pid, or None when the process has exited."""
    try:
        raw = Path(f'/proc/{pid}/cmdline').read_bytes()
    except OSError:
        return None

    return raw.replace(b'\0', b' ').decode('utf-8', 'replace').strip()


def children_of(ppid):
    found = []

    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue

        try:
            if ppid_of(entry) == ppid:
                found.append(int(entry))
        except (OSError, ValueError, IndexError):
            pass

    return found


def socket_inodes(pid):
    inodes = {}

    for fd in Path(f'/proc/{pid}/fd').iterdir():
        try:
            target = os.readlink(fd)
        except OSError:
            continue

        if target.startswith('socket:['):
            inodes[target[len('socket:[') : -1]] = int(fd.name)

    return inodes


def listen_inode(pid, path):
    """The inode of the listening unix socket bound to path, as the
    network namespace of pid sees it.  nxt_listen_socket_create() binds
    "<path>.tmp" and renames it, and /proc/net/unix keeps the bind name."""
    text = Path(f'/proc/{pid}/net/unix').read_text(
        encoding='utf-8', errors='backslashreplace'
    )

    for line in text.splitlines()[1:]:
        f = line.split(maxsplit=7)

        if 'control.unit.sock' in line:
            print(f'control-fd: net/unix row: {line}')

        if (
            len(f) == 8
            and f[7] in (path, f'{path}.tmp')
            and int(f[3], 16) & SO_ACCEPTCON
        ):
            return f[6]

    return None


def find_procs(main):
    procs = {'main': main}

    for pid in children_of(main):
        title = title_of(pid)

        if title is None:
            continue

        if 'controller' in title:
            procs['controller'] = pid
        elif 'router' in title:
            procs['router'] = pid
        elif 'prototype' in title:
            procs['prototype'] = pid

    assert 'prototype' in procs, f'processes {procs}'

    workers = [
        pid
        for pid in children_of(procs['prototype'])
        if 'application' in (title_of(pid) or '')
    ]
    assert workers, 'one application worker'
    procs['worker'] = workers[0]

    return procs


def holders(procs, inode):
    held = {}

    for name, pid in procs.items():
        fd = socket_inodes(pid).get(inode)
        held[name] = fd
        print(f'control-fd: {name} pid={pid} title="{title_of(pid)}" fd={fd}')

        for entry in sorted(
            Path(f'/proc/{pid}/fd').iterdir(), key=lambda p: int(p.name)
        ):
            try:
                target = os.readlink(entry)
                print(f'control-fd:   {name} {entry.name} -> {target}')
            except OSError:
                pass

    return held


def control_inode(main, temp_dir):
    sock = f'{temp_dir}/control.unit.sock'
    inode = listen_inode(main, sock)
    print(f'control-fd: control socket path: {sock}')
    print(f'control-fd: control listen inode: {inode}')

    # The test sees the socket where it must be.
    assert inode is not None, 'control listen socket in /proc/net/unix'

    return inode


def check_holders(procs, inode):
    held = holders(procs, inode)

    assert held['main'] is not None, 'main holds the socket'
    assert held['controller'] is not None, 'controller holds the socket'

    for name in ('router', 'prototype', 'worker'):
        assert held[name] is None, f'{name} holds the control listen socket'


def control_api_answers():
    # A controller that main has just started can need some time.  The
    # controller holds a configuration request until the router is ready.
    for _ in range(50):
        r = client.conf('1', 'applications/empty/processes')

        if 'success' in r:
            break

        time.sleep(0.1)

    assert 'success' in r
    assert client.conf_get('applications/empty/processes') == 1
    assert client.get()['status'] == 200


def wait_for_child(main, pattern, old):
    for _ in range(50):
        for pid in children_of(main):
            if pid != old and pattern in (title_of(pid) or ''):
                return pid

        time.sleep(0.1)

    return None


def test_python_control_socket_fd(unit_pid, temp_dir):
    client.load('empty', processes={"spare": 1})
    assert client.get()['status'] == 200

    inode = control_inode(unit_pid, temp_dir)
    check_holders(find_procs(unit_pid), inode)

    control_api_answers()


def test_python_control_socket_fd_respawn(
    unit_pid, temp_dir, skip_alert, skip_fds_check
):
    """A controller that main starts again gets the socket and answers.
    A router that main starts again does not get it."""
    skip_fds_check(router=True, controller=True)

    client.load('empty', processes={"spare": 1})
    assert client.get()['status'] == 200

    inode = control_inode(unit_pid, temp_dir)

    for name in ('controller', 'router'):
        old = find_procs(unit_pid)[name]
        os.kill(old, signal.SIGKILL)
        skip_alert(fr'process {old} exited on signal 9')

        new = wait_for_child(unit_pid, f'unit: {name}', old)
        assert new is not None, f'{name} started again'
        print(f'control-fd: {name} {old} killed, {new} started')

        control_api_answers()

    check_holders(find_procs(unit_pid), inode)


APP = '''
import os
import socket


def name_of(s):
    name = s.getsockname()

    if isinstance(name, bytes):
        # An abstract name starts with a zero byte.
        return '@' + name[1:].decode('utf-8', 'backslashreplace')

    return name


def application(environ, start_response):
    # A listening socket of the application itself shows that the scan
    # below sees listening unix sockets.
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    probe.bind(f'\\0unit-test-probe-{os.getpid()}')
    probe.listen(1)

    found = []

    for name in sorted(os.listdir('/proc/self/fd'), key=int):
        try:
            # The descriptor of the listdir() scan is closed already.
            dup = os.dup(int(name))
        except OSError:
            continue

        try:
            s = socket.socket(fileno=dup)
        except OSError:
            os.close(dup)
            continue

        try:
            if s.family == socket.AF_UNIX and s.getsockopt(
                socket.SOL_SOCKET, socket.SO_ACCEPTCONN
            ):
                found.append(name_of(s))
        except OSError:
            pass
        finally:
            s.close()

    probe.close()

    body = '\\n'.join(found).encode()
    start_response(
        '200',
        [
            ('Content-Length', str(len(body))),
            ('X-Probe', f'@unit-test-probe-{os.getpid()}'),
        ],
    )
    return [body]
'''


def test_python_control_socket_seen_by_app(temp_dir):
    app_dir = Path(f'{temp_dir}/listen_app')
    app_dir.mkdir()
    (app_dir / 'wsgi.py').write_text(APP, encoding='utf-8')

    client.load(str(app_dir), name='listen_app')

    resp = client.get()
    assert resp['status'] == 200

    found = resp['body'].split('\n') if resp['body'] else []
    print(f'control-fd: listening unix sockets of the application: {found}')

    probe = resp['headers']['X-Probe']
    assert probe in found, 'the scan sees the own listening socket'

    others = [name for name in found if name != probe]
    assert not others, 'no other listening unix socket'
