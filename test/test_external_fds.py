"""Descriptors that an external application inherits through execve().

The application process is forked from the prototype process and then
execs the executable.  Unit hands it the descriptors named in
NXT_UNIT_INIT, and stderr is the log.  Everything else is close-on-exec.
Before that, the program also got the epoll, signalfd and eventfd
descriptors of the event engine, the control socket and a second copy of
the log file.

The executable is a Python script.  It writes its open descriptors to its
stderr, the Unit log, and then execs build/unit_app_test, the bare libunit
application from src/test/nxt_unit_app_test.c, so that the application
starts as usual.

The script runs with -I: unitd may carry PYTHONHOME for the Python
module (UNIT_PYTHONHOME in CI), and the external application inherits
the environment, so a PYTHONHOME of another Python version breaks the
interpreter that runs the test ("No module named 'encodings'").
"""

import json
import os
import sys
from pathlib import Path

import pytest

from unit.applications.proto import ApplicationProto
from unit.option import option

client = ApplicationProto()

SCRIPT = '''#!{python} -I
import fcntl, json, os, sys

fds = {{}}

for fd in range(1024):
    try:
        fcntl.fcntl(fd, fcntl.F_GETFD)
    except OSError:
        continue

    try:
        target = os.readlink('/proc/self/fd/%d' % fd)
    except OSError:
        target = '?'

    fds[fd] = target

sys.stderr.write('external fds: %s\\n' % json.dumps(fds))
sys.stderr.write('external init: %s\\n' % os.environ.get('NXT_UNIT_INIT'))
sys.stderr.flush()

os.execv(sys.argv[1], sys.argv[1:])
'''


def _executable():
    return f'{option.current_dir}/build/unit_app_test'


@pytest.fixture(autouse=True)
def setup_method_fixture():
    if not Path(_executable()).is_file():
        pytest.skip('build/unit_app_test not built (configure --tests)')

    if not Path('/proc/self/fd').is_dir():
        pytest.skip('no /proc/self/fd')


def _init_fds(init):
    """The descriptors that NXT_UNIT_INIT names, as nxt_external_start()
    writes them: "version;stream;pid,id,fd;pid,id,fd;pid,id,fd,fd;
    fd,fd;log_fd,shm_limit,request_limit"."""

    f = init.split(';')
    assert len(f) == 7, f'NXT_UNIT_INIT "{init}"'

    fds = [
        f[2].split(',')[2],
        f[3].split(',')[2],
        *f[4].split(',')[2:4],
        *f[5].split(',')[0:2],
        f[6].split(',')[0],
    ]

    return {int(fd) for fd in fds if int(fd) != -1}


def test_external_fds(wait_for_record):
    script = Path(option.temp_dir) / 'list_fds.py'
    script.write_text(SCRIPT.format(python=sys.executable), encoding='utf-8')
    os.chmod(script, 0o755)

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "applications/fds"}},
            "applications": {
                "fds": {
                    "type": "external",
                    "processes": 1,
                    "working_directory": option.temp_dir,
                    "executable": str(script),
                    "arguments": [_executable()],
                }
            },
        }
    )

    assert client.get()['status'] == 200

    listed = wait_for_record(r'external fds: (.*)$').group(1)
    init = wait_for_record(r'external init: (.*)$').group(1)

    named = _init_fds(init)
    held = json.loads(listed)

    stray = {fd: t for fd, t in held.items() if int(fd) not in {0, 1} | named}

    assert not stray, f'stray descriptors {stray}, NXT_UNIT_INIT "{init}"'
    assert {str(fd) for fd in named} <= set(held), (
        f'NXT_UNIT_INIT "{init}" names a closed descriptor: {listed}'
    )
