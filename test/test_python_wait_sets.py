import os
import platform
import re
import subprocess
import time

import pytest

from unit.applications.lang.python import ApplicationPython

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()

EPOLLEXCLUSIVE = 1 << 28


def app_pids():
    output = subprocess.check_output(['ps', 'ax']).decode()

    return [
        int(m.group(1))
        for m in re.finditer(
            r'^\s*(\d+).*unit: "empty" application', output, re.M
        )
    ]


def exclusive_sets(pid):
    """The epoll sets of the process that have an EPOLLEXCLUSIVE entry.

    libunit makes one for each context that waits on the shared port
    (nxt_unit_wait_set_wait()).  The other epoll sets of a process have no
    EPOLLEXCLUSIVE entry.
    """

    n = 0

    for fd in os.listdir(f'/proc/{pid}/fd'):
        try:
            if os.readlink(f'/proc/{pid}/fd/{fd}') != 'anon_inode:[eventpoll]':
                continue

            with open(f'/proc/{pid}/fdinfo/{fd}') as f:
                info = f.read()

        except OSError:
            continue

        for events in re.findall(r'^tfd:.*events:\s*([0-9a-f]+)', info, re.M):
            if int(events, 16) & EPOLLEXCLUSIVE:
                n += 1
                break

    return n


def sets_per_process(processes, expect_pids):
    client.load('empty', processes=processes)

    assert client.get()['status'] == 200

    # The workers wait again after the response.
    time.sleep(0.5)

    pids = app_pids()
    assert len(pids) == expect_pids, 'application processes'

    return [exclusive_sets(pid) for pid in pids]


def kernel_uses_sets():
    """The release check of nxt_unit_epoll_release(), without the 5.4.41
    and 5.6.13 cases: those kernels skip the cases that need a set."""

    m = re.match(r'(\d+)\.(\d+)', platform.release())

    return m is not None and (int(m.group(1)), int(m.group(2))) >= (5, 7)


@pytest.fixture(autouse=True)
def setup_method_fixture():
    if platform.system() != 'Linux':
        pytest.skip('epoll sets are made on Linux only')


def test_python_wait_sets_two_processes():
    assert sets_per_process(2, 2) == [0, 0], 'no set with 2 processes'


def test_python_wait_sets_four_processes():
    if not kernel_uses_sets():
        pytest.skip('the kernel release does not use epoll sets')

    assert sets_per_process(4, 4) == [1, 1, 1, 1], 'one set per process'


def test_python_wait_sets_max_processes():
    if not kernel_uses_sets():
        pytest.skip('the kernel release does not use epoll sets')

    # "max" counts, not the processes that run.
    assert sets_per_process({"spare": 0, "max": 4}, 1) == [1], 'max 4'
