"""unitd ends its start when it cannot create the pid file.

A failed open() of the pid file was not logged, and the main process then
stayed for ever: nxt_runtime_quit() leaves the end of the exit of main to
the SIGCHLD handler, and before the first fork() no SIGCHLD comes.  The
process kept the control socket bound with no controller behind it.  A
container with a read-only root file system and the pid file at its default
path was "running" and served nothing, with no reason in the log.

Each test starts a unitd of its own: the shared one of conftest has a pid
file that works.
"""

import os
import shutil
import signal
import stat
import subprocess
import tempfile
from pathlib import Path

import pytest

from unit.option import option

# Well above the time of a start, well below the time of a wait for ever.
EXIT_TIMEOUT = 30


@pytest.fixture
def unitd(request):
    binary = f'{option.current_dir}/build/sbin/unitd'

    if not Path(binary).is_file():
        pytest.skip('could not find unitd')

    tmp = tempfile.mkdtemp(prefix='unit-pid-test-')
    Path(f'{tmp}/state').mkdir()

    started = []

    def start(pid_file):
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
                    pid_file,
                    '--log',
                    f'{tmp}/unit.log',
                    '--tmpdir',
                    tmp,
                    '--control',
                    f'unix:{tmp}/control.unit.sock',
                ],
                stdout=subprocess.DEVNULL,
                stderr=stderr,
                start_new_session=True,
            )

        started.append(proc)

        return proc

    yield tmp, start

    for proc in started:
        if proc.poll() is None:
            # unitd can have child processes: kill the whole group.
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

            proc.wait()

    # Keep the logs of this unitd when the test failed.
    rep = getattr(request.node, 'rep_call', None)

    if rep is not None and rep.failed:
        for name in ('stderr.log', 'unit.log'):
            try:
                text = Path(f'{tmp}/{name}').read_text(encoding='utf-8')
                print(f'--- {tmp}/{name} ---\n{text}')
            except OSError:
                pass

    shutil.rmtree(tmp, ignore_errors=True)


def _exit_status(proc):
    try:
        return proc.wait(timeout=EXIT_TIMEOUT)
    except subprocess.TimeoutExpired:
        pytest.fail('unitd did not exit')


def _alerts(tmp):
    log = Path(f'{tmp}/unit.log').read_text(encoding='utf-8')

    return [line for line in log.splitlines() if '[alert]' in line]


def test_pid_file_cannot_be_created(unitd):
    tmp, start = unitd

    # A regular file where the directory should be: open() fails with
    # ENOTDIR, for root too.
    Path(f'{tmp}/file').touch()
    pid_file = f'{tmp}/file/unit.pid'

    proc = start(pid_file)

    assert _exit_status(proc) == 1, 'exit status'

    # One alert, with the reason.  A second one would be the unlink() at
    # exit of a pid file that was never created.
    alerts = _alerts(tmp)

    assert len(alerts) == 1, 'one alert'
    assert (
        f'unable to create the pid file: open("{pid_file}") failed'
        in alerts[0]
    ), 'reason'
    assert 'Not a directory' in alerts[0], 'errno'

    # The control socket is made before the pid file, and removed at exit.
    assert not Path(f'{tmp}/control.unit.sock').exists(), 'control socket'


def test_pid_file_not_created_is_kept(unitd):
    tmp, start = unitd

    # A FIFO with no reader at the pid file path.  nxt_file_open() always
    # adds O_NONBLOCK, and open(O_WRONLY | O_NONBLOCK) of such a FIFO fails
    # with ENXIO, for root too.
    #
    # The directory is writable, so unitd could unlink() what it has failed
    # to open.  It did not create it, so it must not.
    pid_file = f'{tmp}/unit.pid'
    os.mkfifo(pid_file)

    proc = start(pid_file)

    assert _exit_status(proc) == 1, 'exit status'

    alerts = _alerts(tmp)

    assert len(alerts) == 1, 'one alert'
    assert 'No such device or address' in alerts[0], 'errno'

    assert os.path.lexists(pid_file), 'kept'
    assert stat.S_ISFIFO(os.lstat(pid_file).st_mode), 'still the FIFO'


def test_pid_file_of_another_process_is_kept(unitd, is_su):
    if is_su:
        pytest.skip('root can open a read-only file for writing')

    tmp, start = unitd

    # The same with the pid file of another unitd, which this user cannot
    # write: the case the FIFO above stands for.
    pid_file = f'{tmp}/unit.pid'
    Path(pid_file).write_text('1\n', encoding='utf-8')
    os.chmod(pid_file, 0o444)

    proc = start(pid_file)

    assert _exit_status(proc) == 1, 'exit status'

    alerts = _alerts(tmp)

    assert len(alerts) == 1, 'one alert'
    assert 'Permission denied' in alerts[0], 'errno'

    assert Path(pid_file).is_file(), 'kept'
    assert Path(pid_file).read_text(encoding='utf-8') == '1\n', 'content'
