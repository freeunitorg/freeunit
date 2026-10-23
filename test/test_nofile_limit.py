import resource
import socket
import subprocess
import time
from pathlib import Path

import pytest

from unit.applications.lang.python import ApplicationPython
from unit.option import option

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

# The router and the controller raise the soft RLIMIT_NOFILE to the hard
# limit, but not above this value (NXT_PROCESS_NOFILE_MAX).
NOFILE_MAX = 1 << 20

# A container often starts unitd with this soft limit.
NOFILE_LOW = 1024


def processes():
    out = subprocess.check_output(
        ['ps', '-e', '-o', 'pid=,ppid=,args=']
    ).decode()

    procs = []

    for line in out.splitlines():
        pid, ppid, args = line.split(None, 2)
        procs.append((int(pid), int(ppid), args))

    return procs


def wait_pid(title, ppid=None, exclude=None):
    for _ in range(50):
        for pid, parent, args in processes():
            if (
                title in args
                and (ppid is None or parent == ppid)
                and pid != exclude
            ):
                return pid

        time.sleep(0.1)

    return None


def wait_control():
    # The control client fails the test on a refused connect, so wait for
    # the restarted controller to accept on the control socket first.

    addr = f'{option.temp_dir}/control.unit.sock'

    for _ in range(50):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)

        try:
            sock.connect(addr)
            return True

        except (ConnectionRefusedError, FileNotFoundError):
            time.sleep(0.1)

        finally:
            sock.close()

    return False


def soft_limit(pid):
    limits = Path(f'/proc/{pid}/limits').read_text()

    for line in limits.splitlines():
        if line.startswith('Max open files'):
            soft = line.split()[3]
            return resource.RLIM_INFINITY if soft == 'unlimited' else int(soft)

    return None


def test_nofile_limit(skip_alert, skip_fds_check, unit_pid, temp_dir):
    if not Path(f'/proc/{unit_pid}/limits').exists():
        pytest.skip('requires /proc/PID/limits')

    old = resource.prlimit(unit_pid, resource.RLIMIT_NOFILE)
    hard = old[1]

    if hard != resource.RLIM_INFINITY and hard <= NOFILE_LOW:
        pytest.skip(f'the hard limit {hard} is not above {NOFILE_LOW}')

    expected = NOFILE_MAX if hard == resource.RLIM_INFINITY else hard
    expected = min(expected, NOFILE_MAX)

    # Main does not raise its own limit.  A router and a controller forked
    # after this call start with NOFILE_LOW, as in a container.

    resource.prlimit(unit_pid, resource.RLIMIT_NOFILE, (NOFILE_LOW, hard))

    try:
        skip_fds_check(router=True, controller=True)

        for title in ['unit: router', 'unit: controller']:
            pid = wait_pid(title, unit_pid)
            assert pid is not None, title

            subprocess.call(['kill', '-9', str(pid)])
            skip_alert(fr'process {pid} exited on signal 9')

            new = wait_pid(title, unit_pid, exclude=pid)
            assert new is not None, f'{title} did not restart'

        name = f'nofile-{temp_dir.split("/")[-1]}'

        assert wait_control(), 'controller did not accept'

        client.load('empty', name)

        assert client.get()['status'] == 200

        router = wait_pid('unit: router', unit_pid)
        controller = wait_pid('unit: controller', unit_pid)
        prototype = wait_pid(f'"{name}" prototype', unit_pid)
        app = wait_pid(f'"{name}" application', prototype)

        assert soft_limit(unit_pid) == NOFILE_LOW, 'main'
        assert soft_limit(router) == expected, 'router'
        assert soft_limit(controller) == expected, 'controller'
        assert soft_limit(prototype) == NOFILE_LOW, 'prototype'
        assert soft_limit(app) == NOFILE_LOW, 'application'

    finally:
        resource.prlimit(unit_pid, resource.RLIMIT_NOFILE, old)
