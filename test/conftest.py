import atexit
import fcntl
import inspect
import json
import os
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from multiprocessing import Process
from pathlib import Path

import pytest

from unit.check.check_prerequisites import check_prerequisites
from unit.check.discover_available import discover_available
from unit.http import HTTP1
from unit.log import Log
from unit.log import print_log_on_assert
from unit.option import option
from unit.status import Status
from unit.utils import check_findmnt
from unit.utils import public_dir
from unit.utils import waitforfiles
from unit.utils import waitforunmount


def pytest_addoption(parser):
    parser.addoption(
        "--detailed",
        default=False,
        action="store_true",
        help="Detailed output for tests",
    )
    parser.addoption(
        "--print-log",
        default=False,
        action="store_true",
        help="Print unit.log to stdout in case of errors",
    )
    parser.addoption(
        "--save-log",
        default=False,
        action="store_true",
        help="Save unit.log after the test execution",
    )
    parser.addoption(
        "--unsafe",
        default=False,
        action="store_true",
        help="Run unsafe tests",
    )
    parser.addoption(
        "--user",
        type=str,
        help="Default user for non-privileged processes of unitd",
    )
    parser.addoption(
        "--fds-threshold",
        type=int,
        default=0,
        help="File descriptors threshold",
    )
    parser.addoption(
        "--restart",
        default=False,
        action="store_true",
        help="Force Unit to restart after every test",
    )


unit_instance = {}
_processes = []
# Every unitd process-group id (== leader pid, see start_new_session in
# unit_run) we have spawned this session, mapped to its temp dir so BOTH kill
# contracts — the in-memory id and the on-disk unitd.pgid file — can be
# retired together once the group is confirmed gone.  Reaped best-effort at
# interpreter exit so an error path that bypassed unit_stop can never leak a
# unitd tree.
_pgids = {}
_fds_info = {
    'main': {'fds': 0, 'skip': False},
    'router': {'name': 'unit: router', 'pid': -1, 'fds': 0, 'skip': False},
    'controller': {
        'name': 'unit: controller',
        'pid': -1,
        'fds': 0,
        'skip': False,
    },
}
http = HTTP1()
is_findmnt = check_findmnt()


def pytest_configure(config):
    option.config = config.option

    option.detailed = config.option.detailed
    option.fds_threshold = config.option.fds_threshold
    option.print_log = config.option.print_log
    option.save_log = config.option.save_log
    option.unsafe = config.option.unsafe
    option.user = config.option.user
    option.restart = config.option.restart

    option.generated_tests = {}
    option.current_dir = os.path.abspath(
        os.path.join(os.path.dirname(__file__), os.pardir)
    )
    option.test_dir = f'{option.current_dir}/test'

    option.cache_dir = tempfile.mkdtemp(prefix='unit-test-cache-')
    public_dir(option.cache_dir)

    # set stdout to non-blocking

    if option.detailed or option.print_log:
        fcntl.fcntl(sys.stdout.fileno(), fcntl.F_SETFL, 0)


def pytest_generate_tests(metafunc):
    module = metafunc.module
    if (
        not hasattr(module, 'client')
        or not hasattr(module.client, 'application_type')
        or module.client.application_type is None
        or module.client.application_type == 'external'
    ):
        return

    app_type = module.client.application_type

    def generate_tests(versions):
        if not versions:
            pytest.skip('no available module versions')

        metafunc.fixturenames.append('tmp_ct')
        metafunc.parametrize('tmp_ct', versions)

        for version in versions:
            option.generated_tests[
                f'{metafunc.function.__name__} [{version}]'
            ] = f'{app_type} {version}'

    # take available module from option and generate tests for each version

    available_modules = option.available['modules']

    for module, version in metafunc.module.prerequisites['modules'].items():
        if module in available_modules and available_modules[module]:
            available_versions = available_modules[module]

            if version == 'all':
                generate_tests(available_versions)

            elif version == 'any':
                option.generated_tests[
                    metafunc.function.__name__
                ] = f'{app_type} {available_versions[0]}'
            elif callable(version):
                generate_tests(list(filter(version, available_versions)))

            else:
                raise ValueError(
                    f'''
Unexpected prerequisite version "{version}" for module "{module}".
'all', 'any' or callable expected.'''
                )


def pytest_sessionstart():
    unit = unit_run()

    discover_available(unit)

    _clear_conf()

    unit_stop()

    Log.check_alerts()

    if option.restart:
        shutil.rmtree(unit['temp_dir'])
    else:
        _clear_temp_dir()


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item):
    # execute all other hooks to obtain the report object
    outcome = yield
    rep = outcome.get_result()

    # set a report attribute for each phase of a call, which can
    # be "setup", "call", "teardown"

    setattr(item, f'rep_{rep.when}', rep)


@pytest.fixture(scope='module', autouse=True)
def check_prerequisites_module(request):
    if hasattr(request.module, 'prerequisites'):
        check_prerequisites(request.module.prerequisites)


@pytest.fixture(autouse=True)
def run(request):
    unit = unit_run()

    option.skip_alerts = [
        r'read signalfd\(4\) failed',
        r'sendmsg.+failed',
        r'recvmsg.+failed',
    ]
    option.skip_sanitizer = False

    _fds_info['main']['skip'] = False
    _fds_info['router']['skip'] = False
    _fds_info['controller']['skip'] = False

    # Re-capture baseline immediately before the test body runs so that any
    # lazy initialization (e.g. OTel tokio runtime) that occurred after
    # unit_run() captured the initial baseline is not counted as a leak.
    if not option.restart:
        _fds_info['main']['fds'] = _count_fds(unit_instance['pid'])
        router = _fds_info['router']
        router['fds'] = _count_fds(router['pid'])
        controller = _fds_info['controller']
        controller['fds'] = _count_fds(controller['pid'])

    yield

    # stop unit

    error_stop_unit = unit_stop()
    error_stop_processes = stop_processes()

    # prepare log

    with Log.open() as f:
        log = f.read()
        Log.set_pos(f.tell())

    if not option.save_log and option.restart:
        shutil.rmtree(unit['temp_dir'])
        Log.set_pos(0)

    # clean temp_dir before the next test

    if not option.restart:
        _clear_conf(log=log)
        _clear_temp_dir()

    # check descriptors

    _check_fds(log=log)

    # check processes id's and amount

    _check_processes()

    # print unit.log in case of error

    if hasattr(request.node, 'rep_call') and request.node.rep_call.failed:
        Log.print_log(log)

    if error_stop_unit or error_stop_processes:
        Log.print_log(log)

    # check unit.log for errors

    assert error_stop_unit is None, 'stop unit'
    assert error_stop_processes is None, 'stop processes'

    Log.check_alerts(log=log)


def _write_pgid(temp_dir, pgid):
    # The pgid file on disk is the contract external sweepers (the D4 wrapper)
    # consume to reap a unitd tree orphaned by a hard-killed runner.  Written
    # eagerly at spawn, before we risk a startup timeout.
    try:
        Path(f'{temp_dir}/unitd.pgid').write_text(f'{pgid}\n', encoding='utf-8')
    except OSError:
        pass


def _register_pgid(pgid, temp_dir=None):
    if pgid and pgid > 1:
        _pgids[pgid] = temp_dir


def _forget_pgid(p, pgid):
    # Retire BOTH kill contracts for a pgid once its tree is confirmed gone:
    # the in-memory id (the kernel reuses pid/pgid numbers, and sweeping a
    # stale entry at interpreter exit could TERM/KILL an unrelated process
    # group) and the on-disk unitd.pgid file (--save-log and aborted runs
    # retain temp dirs, and external sweepers are documented to consume it).
    if pgid and not _group_alive(p, pgid):
        temp_dir = _pgids.pop(pgid, None)

        if temp_dir is not None:
            try:
                Path(f'{temp_dir}/unitd.pgid').unlink(missing_ok=True)
            except OSError:
                pass


def _signal_group(pgid, sig):
    # Guard hard: never signal group 0 (our OWN process group) or a
    # negative/empty/reserved id.  With start_new_session the leader pid equals
    # the group id, so this is always a group we created — never the pytest
    # runner's group and never an unrelated unitd on the box.
    if not pgid or pgid <= 1:
        return
    try:
        os.killpg(pgid, sig)
    except (ProcessLookupError, PermissionError):
        pass


def _group_alive(p, pgid):
    # True only if a NON-zombie process still remains in the group.  Our group
    # leader is a direct child (Popen), so once signalled it lingers as a zombie
    # until reaped; reap it via poll() first, and skip any 'Z' state below, so a
    # zombie never keeps the group looking alive forever.
    if p is not None:
        p.poll()

    if not pgid or pgid <= 1:
        return False

    # Portable fast path first: signal 0 probes group membership on every
    # POSIX platform the suite supports.  ESRCH == nothing left at all.
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # someone in the group is alive but not ours to signal — treat as
        # alive; the ladder's killpg will hit the same wall and time out
        return True

    # killpg(0) succeeds even when only zombies remain (they are still group
    # members), so on Linux refine via /proc to avoid waiting a full ladder
    # timeout on an already-dead tree.  No subprocess per 0.2 s poll tick
    # (this loop exists for slow builders), and no pgrep failed-vs-empty
    # ambiguity.
    try:
        entries = os.listdir('/proc')
    except OSError:
        # No procfs (macOS, some BSD setups): killpg succeeded, so report
        # alive.  Worst case is a zombie-only group riding out one ladder
        # timeout; our own leader zombie is already reaped by poll() above.
        return True

    for entry in entries:
        if not entry.isdigit():
            continue
        try:
            stat = Path(f'/proc/{entry}/stat').read_text(
                encoding='utf-8', errors='ignore'
            )
            # comm (field 2) may contain spaces and ')'; parse from the
            # LAST ')'.  After it: state, ppid, pgrp, ...
            fields = stat[stat.rfind(')') + 1 :].split()
            if int(fields[2]) == pgid and not fields[0].startswith('Z'):
                return True
        except FileNotFoundError:
            # vanished between listdir and read: a routine race, keep going
            continue
        except (OSError, IndexError, ValueError):
            # unrecognized procfs layout (non-Linux): trust the killpg verdict
            return True

    return False


def _reap_group(p, pgid, timeout):
    # Escalation ladder scoped to a process group WE created (pgid == leader
    # pid): SIGTERM the group, poll up to `timeout` s, then SIGKILL the group.
    # Keyed on the GROUP, not the main pid, so it reaps the router/controller/
    # app workers even when main is already dead but its children survive.
    if not pgid or pgid <= 1:
        return

    if not _group_alive(p, pgid):
        return

    _signal_group(pgid, signal.SIGTERM)

    deadline = time.time() + timeout
    while time.time() < deadline:
        if not _group_alive(p, pgid):
            return
        time.sleep(0.2)

    _signal_group(pgid, signal.SIGKILL)

    deadline = time.time() + 5
    while time.time() < deadline:
        if not _group_alive(p, pgid):
            return
        time.sleep(0.2)


# The reap handlers below must run ONLY in the pytest runner itself: with the
# fork start method (the multiprocessing default on Linux through 3.13) the
# helper children spawned by run_process inherit both the SIGTERM disposition
# and _pgids, and stop_processes terminates those helpers with SIGTERM — a
# helper running the sweep would TERM/KILL the still-active Unit tree
# mid-session.
_main_pid = os.getpid()


@atexit.register
def _reap_all_pgids():
    # Best-effort safety net for any group we spawned that is somehow still
    # alive at interpreter exit (an exception path that bypassed unit_stop).
    # NOTE: a SIGBUS/SIGKILL of the runner itself defeats every in-process
    # handler — external containment is the D4 wrapper's job — but the
    # <temp_dir>/unitd.pgid file left on disk is the contract those external
    # sweepers consume.
    if os.getpid() != _main_pid:
        return
    for pgid in list(_pgids):
        try:
            _reap_group(None, pgid, timeout=3)
        except Exception:
            pass
        # Retire the on-disk kill contract too: this safety net runs exactly
        # in the aborted-session cases where temp dirs stay behind, and a
        # stale unitd.pgid there could point an external sweeper at a reused
        # process group.
        _forget_pgid(None, pgid)


def _sigterm_reap(signum, frame):
    # SIGTERM does not run atexit handlers, so drive the same best-effort reap
    # here, then restore the default disposition and re-raise so the runner
    # still dies from the signal.  SIGINT is already covered by pytest's
    # KeyboardInterrupt path (see unit_stop).  Forked children re-raise
    # without reaping (see _main_pid above).
    if os.getpid() == _main_pid:
        _reap_all_pgids()
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    os.kill(os.getpid(), signal.SIGTERM)


# Install once at import, and only if nothing else already owns SIGTERM, so we
# never stomp a host harness's handler.
if signal.getsignal(signal.SIGTERM) in (signal.SIG_DFL, None):
    signal.signal(signal.SIGTERM, _sigterm_reap)


def unit_run(state_dir=None):
    global unit_instance

    if not option.restart and 'unitd' in unit_instance:
        return unit_instance

    builddir = f'{option.current_dir}/build'
    libdir = f'{builddir}/lib'
    modulesdir = f'{libdir}/unit/modules'
    sbindir = f'{builddir}/sbin'
    unitd = f'{sbindir}/unitd'

    if not Path(unitd).is_file():
        sys.exit('Could not find unit')

    temporary_dir = tempfile.mkdtemp(prefix='unit-test-')
    option.temp_dir = temporary_dir
    public_dir(temporary_dir)

    # Every start gets a fresh temp dir and therefore a fresh, empty
    # unit.log, so the read offsets recorded against the previous one are
    # meaningless here.  Teardown saves that offset unconditionally and only
    # resets it when it removes the temp dir, which --save-log stops it from
    # doing: without this, a --save-log --restart run seeks each test's reads
    # to the size of the PREVIOUS test's log.  Everything log-based then sees
    # a tail slice or nothing at all -- Log.wait_for_record misses records
    # that are present, and, worse silently, the teardown Log.check_alerts()
    # and _check_fds() stop examining most of what they are given.
    Log.pos.clear()

    if oct(stat.S_IMODE(Path(builddir).stat().st_mode)) != '0o777':
        public_dir(builddir)

    statedir = f'{temporary_dir}/state' if state_dir is None else state_dir
    Path(statedir).mkdir(exist_ok=True)

    control_sock = f'{temporary_dir}/control.unit.sock'

    unitd_args = [
        unitd,
        '--no-daemon',
        '--modulesdir',
        modulesdir,
        '--statedir',
        statedir,
        '--pid',
        f'{temporary_dir}/unit.pid',
        '--log',
        f'{temporary_dir}/unit.log',
        '--control',
        f'unix:{temporary_dir}/control.unit.sock',
        '--tmpdir',
        temporary_dir,
    ]

    if option.user:
        unitd_args.extend(['--user', option.user])

    with open(f'{temporary_dir}/unit.log', 'w', encoding='utf-8') as log:
        # start_new_session=True makes the child a session/process-group leader,
        # so its pgid == pid and the whole unitd family (main, controller,
        # router, app workers) shares one group we can signal as a unit — and
        # which can never be confused with the pytest runner's own group or an
        # unrelated unitd already running on the box.
        # UNIT_PYTHONHOME pins the embedded interpreter's stdlib to the
        # prefix unit's python module was built against.  It cannot be passed
        # as PYTHONHOME from outside: unitd inherits this process's
        # environment, and PYTHONHOME here would also rebind pytest's own
        # interpreter, dropping dist-packages (where pytest itself lives) from
        # its sys.path.  Needed when unit is built against a non-system
        # interpreter whose minor version matches the system one, because
        # CPython then resolves its prefix by finding "python3" on PATH.
        unitd_env = os.environ.copy()
        pythonhome = unitd_env.pop('UNIT_PYTHONHOME', None)
        if pythonhome:
            unitd_env['PYTHONHOME'] = pythonhome

        p = subprocess.Popen(
            unitd_args, stderr=log, start_new_session=True, env=unitd_env
        )
        unit_instance['process'] = p

    # Record the group id (== leader pid) immediately, on disk and in-memory,
    # before we risk a startup timeout below.
    unit_instance['pgid'] = p.pid
    _register_pgid(p.pid, temporary_dir)
    _write_pgid(temporary_dir, p.pid)

    # Start budget is env-tunable for slow/32-bit builders; the default of 5 s
    # (waitforfiles counts in 0.1 s steps) preserves the previous behavior.
    start_timeout = int(os.environ.get('UNIT_TEST_START_TIMEOUT', 5))
    if not waitforfiles(control_sock, timeout=start_timeout * 10):
        # Reap the group we just spawned before bailing: a unitd that never
        # opened its control socket would otherwise leak its whole tree.
        _reap_group(p, p.pid, timeout=5)
        Log.print_log()
        sys.exit('Could not start unit')

    unit_instance['temp_dir'] = temporary_dir
    unit_instance['control_sock'] = control_sock
    unit_instance['unitd'] = unitd

    unit_instance['pid'] = (
        Path(f'{temporary_dir}/unit.pid').read_text(encoding='utf-8').rstrip()
    )

    if state_dir is None:
        _clear_conf()

    _fds_info['main']['fds'] = _count_fds(unit_instance['pid'])

    router = _fds_info['router']
    router['pid'] = pid_by_name(router['name'])
    router['fds'] = _count_fds(router['pid'])

    controller = _fds_info['controller']
    controller['pid'] = pid_by_name(controller['name'])
    controller['fds'] = _count_fds(controller['pid'])

    Status._check_zeros()

    return unit_instance


def unit_stop():
    if not option.restart:
        if inspect.stack()[1].function.startswith('test_'):
            pytest.skip('no restart mode')

        return

    # Startup may have failed before the process/pid were recorded; nothing to
    # stop then, and reads of unit_instance['pid'] must not KeyError.
    p = unit_instance.get('process')
    if p is None:
        return

    pgid = unit_instance.get('pgid')
    main_pid = unit_instance.get('pid')

    # check zombies (only once startup got far enough to record the main pid)

    if main_pid is not None:
        # A child that just exited can briefly remain a zombie until main reaps
        # it; under load (notably sanitizer builds) that window can outlast a
        # single sample and flake this check.  Poll so a transient
        # reap-in-progress is not mistaken for a genuinely leaked zombie -- a
        # real leak persists, a reap race clears within a few ms.  Sample after
        # each sleep (including the last one) so a reap that lands in the final
        # interval is still observed before the assertion.
        z_ppids = []
        for i in range(41):
            if i:
                time.sleep(0.05)
            out = subprocess.check_output(
                ['ps', 'ax', '-o', 'state', '-o', 'ppid']
            ).decode()
            z_ppids = re.findall(r'Z\s*(\d+)', out)
            if main_pid not in z_ppids:
                break
        assert main_pid not in z_ppids, 'no zombies'

    # terminate unit

    if p.poll() is not None:
        # Main already exited — make sure no group member (router, controller,
        # app worker) lingers behind it.
        _reap_group(p, pgid, timeout=5)
        _forget_pgid(p, pgid)
        return

    # Graceful shutdown first: SIGQUIT asks main to quit cleanly and reap its
    # own children.  STOP_TIMEOUT is env-tunable because a slow or 32-bit CI
    # box can take far longer than the 15 s default to drain — set
    # UNIT_TEST_STOP_TIMEOUT=60 (or more) there.
    stop_timeout = int(os.environ.get('UNIT_TEST_STOP_TIMEOUT', 15))

    p.send_signal(signal.SIGQUIT)

    try:
        retcode = p.wait(stop_timeout)
        if retcode:
            # Abnormal graceful shutdown can leave router/controller/app
            # workers behind in the group; reap them before reporting.
            _reap_group(p, pgid, timeout=5)
            _forget_pgid(p, pgid)
            return f'Child process terminated with code {retcode}'

    except KeyboardInterrupt:
        # Ctrl-C mid-shutdown: reap the whole group before re-raising so we
        # never leak the unitd tree.
        _reap_group(p, pgid, timeout=5)
        _forget_pgid(p, pgid)
        raise

    except subprocess.TimeoutExpired:
        # Graceful quit overran STOP_TIMEOUT: escalate to the process GROUP
        # (SIGTERM, then SIGKILL) so the router/controller/app workers die even
        # when main itself is wedged.  A successful forced reap is still an
        # ERROR: SIGQUIT not completing in time is a graceful-shutdown hang
        # (a real bug class), and returning success here would mask it.
        _reap_group(p, pgid, timeout=5)
        if _group_alive(p, pgid):
            return 'Could not terminate unit'
        _forget_pgid(p, pgid)
        return (
            f'Unit did not exit within {stop_timeout}s after SIGQUIT '
            '(process group reaped forcibly)'
        )

    # A clean master exit does not prove the group is empty: a router/
    # controller/app worker can outlive it and would silently survive into
    # the next test (reparented, so _check_processes' ppid filter misses it).
    # _reap_group is a no-op when the group is already gone.
    _reap_group(p, pgid, timeout=5)
    _forget_pgid(p, pgid)


@print_log_on_assert
def _clear_conf(*, log=None):
    sock = unit_instance['control_sock']

    resp = http.put(
        url='/config',
        sock_type='unix',
        addr=sock,
        body=json.dumps({"listeners": {}, "applications": {}}),
    )['body']

    assert 'success' in resp, 'clear conf'

    def get(url):
        return http.get(url=url, sock_type='unix', addr=sock)['body']

    def delete(url):
        return http.delete(url=url, sock_type='unix', addr=sock)['body']

    if (
        'openssl' in option.available['modules']
        and option.available['modules']['openssl']
    ):
        try:
            certs = json.loads(get('/certificates')).keys()

        except json.JSONDecodeError:
            pytest.fail("Can't parse certificates list.")

        for cert in certs:
            assert 'success' in delete(f'/certificates/{cert}'), 'delete cert'

    if (
        'njs' in option.available['modules']
        and option.available['modules']['njs']
    ):
        try:
            scripts = json.loads(get('/js_modules')).keys()

        except json.JSONDecodeError:
            pytest.fail("Can't parse njs modules list.")

        for script in scripts:
            assert 'success' in delete(f'/js_modules/{script}'), 'delete script'


def _clear_temp_dir():
    temporary_dir = unit_instance['temp_dir']

    if is_findmnt and not waitforunmount(temporary_dir, timeout=600):
        Log.print_log()
        sys.exit(f'Could not unmount filesystems in tmpdir ({temporary_dir}).')

    for item in Path(temporary_dir).iterdir():
        if item.name not in [
            'control.unit.sock',
            'state',
            'unit.pid',
            'unit.log',
            # the on-disk pgid contract for external sweepers; deleting it
            # would leave a hard-killed runner's orphan tree unidentifiable
            'unitd.pgid',
        ]:

            public_dir(item)

            if item.is_file() or stat.S_ISSOCK(item.stat().st_mode):
                item.unlink()
            else:
                for _ in range(10):
                    try:
                        shutil.rmtree(item)
                        break
                    except OSError as err:
                        # OSError: [Errno 16] Device or resource busy
                        # OSError: [Errno 39] Directory not empty
                        if err.errno not in [16, 39]:
                            raise
                        time.sleep(1)


def _check_processes():
    router_pid = _fds_info['router']['pid']
    controller_pid = _fds_info['controller']['pid']
    main_pid = unit_instance['pid']

    for _ in range(600):
        out = (
            subprocess.check_output(
                ['ps', '-ax', '-o', 'pid', '-o', 'ppid', '-o', 'command']
            )
            .decode()
            .splitlines()
        )
        # match only the pid/ppid columns; a substring match against the
        # whole line can catch unrelated processes whose arguments contain
        # the same number (e.g. agetty's baud rates vs. pid 9600)
        out = [l for l in out if main_pid in l.split()[:2]]

        if len(out) <= 3:
            break

        time.sleep(0.1)

    if option.restart:
        assert len(out) == 0, 'all termimated'
        return

    assert len(out) == 3, 'main, router, and controller expected'

    out = [l for l in out if 'unit: main' not in l]
    assert len(out) == 2, 'one main'

    out = [
        l
        for l in out
        if re.search(fr'{router_pid}\s+{main_pid}.*unit: router', l) is None
    ]
    assert len(out) == 1, 'one router'

    out = [
        l
        for l in out
        if re.search(fr'{controller_pid}\s+{main_pid}.*unit: controller', l)
        is None
    ]
    assert len(out) == 0, 'one controller'


@print_log_on_assert
def _check_fds(*, log=None):
    def waitforfds(diff):
        for _ in range(600):
            fds_diff = diff()

            if fds_diff <= option.fds_threshold:
                break

            time.sleep(0.1)

        return fds_diff

    ps = _fds_info['main']
    if not ps['skip']:
        fds_diff = waitforfds(
            lambda: _count_fds(unit_instance['pid']) - ps['fds']
        )
        ps['fds'] += fds_diff

        assert fds_diff <= option.fds_threshold, 'descriptors leak main process'

    else:
        ps['fds'] = _count_fds(unit_instance['pid'])

    for name in ['controller', 'router']:
        ps = _fds_info[name]
        ps_pid = ps['pid']
        ps['pid'] = pid_by_name(ps['name'])

        if not ps['skip']:
            fds_diff = waitforfds(lambda: _count_fds(ps['pid']) - ps['fds'])
            ps['fds'] += fds_diff

            if not option.restart:
                assert ps['pid'] == ps_pid, f'same pid {name}'

            assert fds_diff <= option.fds_threshold, f'descriptors leak {name}'

        else:
            ps['fds'] = _count_fds(ps['pid'])


def _count_fds(pid):
    procfile = Path(f'/proc/{pid}/fd')
    if procfile.is_dir():
        return len(list(procfile.iterdir()))

    try:
        out = subprocess.check_output(
            ['procstat', '-f', pid],
            stderr=subprocess.STDOUT,
        ).decode()
        return len(out.splitlines())

    except (FileNotFoundError, TypeError, subprocess.CalledProcessError):
        pass

    try:
        out = subprocess.check_output(
            ['lsof', '-n', '-p', pid],
            stderr=subprocess.STDOUT,
        ).decode()
        return len(out.splitlines())

    except (FileNotFoundError, TypeError, subprocess.CalledProcessError):
        pass

    return 0


def run_process(target, *args):
    global _processes

    process = Process(target=target, args=args)
    process.start()

    _processes.append(process)


def stop_processes():
    if not _processes:
        return

    fail = False
    for process in _processes:
        if process.is_alive():
            process.terminate()
            process.join(timeout=15)

            if process.is_alive():
                fail = True

    if fail:
        return 'Fail to stop process(es)'


def pid_by_name(name):
    output = subprocess.check_output(['ps', 'ax', '-O', 'ppid']).decode()
    m = re.search(fr'\s*(\d+)\s*{unit_instance["pid"]}.*{name}', output)
    return None if m is None else m.group(1)


def find_proc(name, ps_output):
    return re.findall(f'{unit_instance["pid"]}.*{name}', ps_output)


def pytest_sessionfinish():
    if not option.restart and option.save_log:
        Log.print_path()

    option.restart = True

    unit_stop()

    public_dir(option.cache_dir)
    shutil.rmtree(option.cache_dir)

    if not option.save_log and Path(option.temp_dir).is_dir():
        public_dir(option.temp_dir)
        shutil.rmtree(option.temp_dir)


@pytest.fixture
def date_to_sec_epoch():
    def _date_to_sec_epoch(date, template='%a, %d %b %Y %X %Z'):
        return time.mktime(time.strptime(date, template))

    return _date_to_sec_epoch


@pytest.fixture
def findall():
    def _findall(*args, **kwargs):
        return Log.findall(*args, **kwargs)

    return _findall


@pytest.fixture
def is_su():
    return option.is_privileged


@pytest.fixture
def is_unsafe(request):
    return request.config.getoption("--unsafe")


@pytest.fixture
def require():
    return check_prerequisites


@pytest.fixture
def search_in_file():
    def _search_in_file(pattern, name='unit.log', flags=re.M):
        return re.search(pattern, Log.read(name), flags)

    return _search_in_file


@pytest.fixture
def sec_epoch():
    return time.mktime(time.gmtime())


@pytest.fixture()
def skip_alert():
    def _skip(*alerts):
        option.skip_alerts.extend(alerts)

    return _skip


@pytest.fixture()
def skip_fds_check():
    def _skip(main=False, router=False, controller=False):
        _fds_info['main']['skip'] = main
        _fds_info['router']['skip'] = router
        _fds_info['controller']['skip'] = controller

    return _skip


@pytest.fixture()
def system():
    return option.system


@pytest.fixture
def temp_dir():
    return unit_instance['temp_dir']


@pytest.fixture
def unit_pid():
    return unit_instance['process'].pid


@pytest.fixture
def wait_for_record():
    def _wait_for_record(*args, **kwargs):
        return Log.wait_for_record(*args, **kwargs)

    return _wait_for_record
