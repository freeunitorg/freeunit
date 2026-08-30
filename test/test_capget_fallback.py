"""Unit keeps running when a syscall filter denies capget().

A hardened deployment -- a systemd "SystemCallFilter=" allowlist, a
hand-written seccomp profile -- can make capget(2) fail for a non-root
unitd.  Since 'capability: keep running when capget() is filtered' that
is a warning rather than an outage for the two errnos such a filter
conventionally returns, EPERM and ENOSYS; every other errno still
aborts, because a privilege downgrade caused by a bug in Unit is worse
than a hard failure.

The same shim also watches capset(2), which is how this file covers
nxt_capability_drop() -- the drop every forked process performs at the
tail of nxt_process_apply_creds().  Its effect cannot be tested here:
the suite runs as an ordinary user with no capabilities, so "the
application has none either" is true with or without the code.  What
can be tested, and cannot pass by accident, is *which processes drop*.
Main must not, because it binds listening sockets on every
reconfiguration; the prototype must, because it is the ancestor of
every worker; and the worker itself must not, because it inherits.
A run with real capabilities to lose needs root, and lives in the
freeunit-harness smoke test instead.

Reproducing a seccomp filter in a test would need privileges the suite
does not have, so the errno is injected instead: src/nxt_capability.c
calls the kernel through the nxt_capget() macro, which expands to
glibc's syscall() wrapper, and that is an ordinary interposable symbol.
test/capget_filter.c preloads a definition of it -- see that file for
what it does and does not touch.

Inheritance is accepted rather than papered over.  Unit's language
modules are fork()ed, never exec()ed, so the application process
inherits the shim already mapped and already armed -- the errno was
read by the shim's constructor in the main process, long before the
fork.  Clearing LD_PRELOAD from the application's "environment" would
therefore disarm nothing; it would only hide the fact.

That inheritance costs nothing, and the test proves it rather than
asserting it: the shim appends the pid of every process whose capget()
it failed to a record file, and the test requires that set to be
exactly the unitd main process.  It can be, because
nxt_capability_set() has a single caller, nxt_runtime_conf_init(),
which runs in the main process before anything is forked.  For that
record to be evidence and not an empty file, the shim keeps a copy of
the record path rather than the getenv() pointer -- unitd overwrites
the argv+environ block with its process title, so the pointer alone
would leave every child unable to record anything at all.  See
capget_filter.c.

/proc/<pid>/environ deliberately plays no part in that proof.  It
cannot: nxt_process_arguments() takes the contiguous argv+environ
string area for the process title, so every Unit process has an
environ region full of title text and NUL padding while the real
environment lives on the heap.  Measured here, the main process shows
the tail of its own argv and the application shows 4807 NUL bytes --
LD_PRELOAD is invisible in both, in a run where the shim demonstrably
ran in the main process.  What the application's /proc/<pid>/maps
shows instead is the shim object itself, mapped, which is the thing
that would actually matter.
"""

import errno
import json
import os
import re
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

from unit.http import HTTP1
from unit.option import option
from unit.utils import public_dir, waitforfiles

client = HTTP1()

WARNING = r'\[warn\].+capget\(\) failed.+capabilities are unknown'
ALERT = r'\[alert\].+failed to get process capabilities'

# The two errnos a syscall filter conventionally returns for a denied
# call.  nxt_capability.c accepts both, so both belong in the test.
FILTERED = [errno.EPERM, errno.ENOSYS]


@pytest.fixture(scope='module', autouse=True)
def requirements():
    if option.system != 'Linux':
        pytest.skip('capget() is Linux only')

    if option.is_privileged:
        pytest.skip('a root unitd never calls capget()')


@pytest.fixture(scope='module')
def shim(requirements):
    """Build test/capget_filter.c into a preloadable object.

    The suite has no convention for compiling C helpers -- nothing under
    test/ builds any -- so rather than invent a build system this
    compiles at test time with cc(1) and skips when there is none.

    Depends on requirements() rather than trusting fixture ordering:
    both are module-scoped, and the link needs -ldl, so on a platform
    that should skip the build would otherwise fail first and turn the
    skip into an error.
    """
    compiler = shutil.which('cc') or shutil.which('gcc')

    if compiler is None:
        pytest.skip('requires a C compiler')

    source = f'{option.test_dir}/capget_filter.c'
    outdir = tempfile.mkdtemp(prefix='unit-test-capget-')
    library = f'{outdir}/capget_filter.so'

    build = subprocess.run(
        [compiler, '-shared', '-fPIC', '-O1', '-o', library, source, '-ldl'],
        check=False,
        capture_output=True,
    )

    assert build.returncode == 0, (
        f'failed to build the capget shim:\n{build.stderr.decode()}'
    )

    yield library

    shutil.rmtree(outdir, ignore_errors=True)


class Unitd:
    """A unitd of our own, since conftest's shared one has no way to
    take an environment."""

    def __init__(self, shim, errno):
        builddir = f'{option.current_dir}/build'
        self.unitd = f'{builddir}/sbin/unitd'

        if not Path(self.unitd).is_file():
            pytest.skip('could not find unitd')

        self.dir = tempfile.mkdtemp(prefix='unit-test-capget-')
        public_dir(self.dir)

        Path(f'{self.dir}/state').mkdir()

        self.log = f'{self.dir}/unit.log'
        self.stderr = f'{self.dir}/stderr.log'
        self.record = f'{self.dir}/capget.pids'
        self.capset_record = f'{self.dir}/capset.pids'
        self.control = f'{self.dir}/control.unit.sock'
        self.listener = f'{self.dir}/app.sock'
        self.pidfile = f'{self.dir}/unit.pid'

        env = os.environ.copy()

        # Same UNIT_PYTHONHOME dance as conftest.unit_run(): PYTHONHOME
        # must not be set for the pytest interpreter itself.
        pythonhome = env.pop('UNIT_PYTHONHOME', None)
        if pythonhome:
            env['PYTHONHOME'] = pythonhome

        env['LD_PRELOAD'] = shim
        env['NXT_TEST_CAPGET_ERRNO'] = str(errno)
        env['NXT_TEST_CAPGET_RECORD'] = self.record
        env['NXT_TEST_CAPSET_RECORD'] = self.capset_record

        # unitd's stderr is kept apart from unit.log on purpose.  The
        # first warning is written from nxt_runtime_conf_init(), before
        # nxt_runtime_log_files_create() dup2()s the log file over fd 2,
        # so it can only land here; unit.log then holds exactly the copy
        # nxt_runtime_start() repeats once the file exists.  Merging the
        # two -- which is what conftest does -- would make it impossible
        # to tell which of the two emissions a match came from.
        self.stderr_file = open(self.stderr, 'w', encoding='utf-8')

        self.process = subprocess.Popen(
            [
                self.unitd,
                '--no-daemon',
                '--modulesdir',
                f'{builddir}/lib/unit/modules',
                '--statedir',
                f'{self.dir}/state',
                '--pid',
                self.pidfile,
                '--log',
                self.log,
                '--control',
                f'unix:{self.control}',
                '--tmpdir',
                self.dir,
            ],
            stdout=subprocess.DEVNULL,
            stderr=self.stderr_file,
            start_new_session=True,
            env=env,
        )

    def stop(self):
        if self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

        self.process.wait(timeout=30)
        self.stderr_file.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def read(self, path):
        return Path(path).read_text(encoding='utf-8', errors='ignore')

    def filtered_pids(self):
        return self.recorded_pids(self.record)

    def capset_pids(self):
        return self.recorded_pids(self.capset_record)

    def recorded_pids(self, path):
        if not Path(path).exists():
            return set()

        return {int(p) for p in self.read(path).split()}


@pytest.fixture()
def unitd_factory(shim):
    started = []

    def factory(errno):
        instance = Unitd(shim, errno)
        started.append(instance)
        return instance

    yield factory

    for instance in started:
        instance.stop()


def app_conf(appdir):
    """A one-line application that answers with its own pid, so the
    /proc lookups below cannot land on the wrong process."""
    modules = option.available['modules']

    if 'python' in modules:
        Path(f'{appdir}/wsgi_pid.py').write_text(
            'import os\n'
            '\n'
            '\n'
            'def application(environ, start_response):\n'
            '    body = str(os.getpid()).encode()\n'
            "    start_response('200 OK',"
            " [('Content-Length', str(len(body)))])\n"
            '    return [body]\n',
            encoding='utf-8',
        )

        return {
            "type": "python",
            "processes": {"spare": 0},
            "path": appdir,
            "module": "wsgi_pid",
        }

    if 'php' in modules:
        Path(f'{appdir}/index.php').write_text(
            '<?php echo getmypid();\n', encoding='utf-8'
        )

        return {
            "type": "php",
            "processes": {"spare": 0},
            "root": appdir,
            "index": "index.php",
        }

    pytest.skip('requires the python or php module')


def wait_for(path, pattern, timeout=150):
    for _ in range(timeout):
        if Path(path).exists():
            found = re.search(pattern, Path(path).read_text(
                encoding='utf-8', errors='ignore'), re.M)

            if found is not None:
                return found

        time.sleep(0.1)

    return None


@pytest.mark.parametrize('filtered_errno', FILTERED, ids=errno.errorcode.get)
def test_capget_filtered_keeps_running(shim, unitd_factory, filtered_errno):
    unitd = unitd_factory(filtered_errno)

    assert waitforfiles(unitd.control), (
        f'unitd started; its stderr was:\n{unitd.read(unitd.stderr)}'
    )

    # The early copy, written before the log file exists.
    assert re.search(WARNING, unitd.read(unitd.stderr), re.M), 'warn on stderr'

    # The copy nxt_runtime_start() repeats into the log file itself.
    # A daemonised unitd keeps no other trace of it.
    assert wait_for(unitd.log, WARNING) is not None, 'warn in unit.log'
    assert wait_for(unitd.log, r'router started') is not None, 'router started'

    appdir = f'{unitd.dir}/app'
    Path(appdir).mkdir()
    conf = {
        "listeners": {f'unix:{unitd.listener}': {"pass": "applications/app"}},
        "applications": {"app": app_conf(appdir)},
    }
    public_dir(appdir)

    resp = client.put(
        url='/config',
        sock_type='unix',
        addr=unitd.control,
        body=json.dumps(conf),
    )
    assert 'success' in resp['body'], 'configure'

    resp = client.get(sock_type='unix', addr=unitd.listener)
    assert resp['status'] == 200, 'application answers'

    app_pid = int(resp['body'].strip())

    # Capabilities were unknown, so setid stayed clear and the runtime
    # never resolved credentials: the application runs as unitd's uid.
    status = Path(f'/proc/{app_pid}/status').read_text(encoding='utf-8')
    uid = re.search(r'^Uid:\s+(\d+)\s+(\d+)', status, re.M)
    assert uid is not None, 'app Uid'
    assert int(uid.group(2)) == os.geteuid(), 'app runs as unitd uid'

    # The application holds no capabilities at all.  On a suite that
    # runs without any to begin with this cannot fail -- which is why
    # it is not the real assertion; see the capset() bookkeeping below.
    # It is still worth stating, because it is the property the change
    # exists to guarantee and the harness smoke test asserts the same
    # thing on a unitd that demonstrably did hold capabilities.
    for name in ('CapInh', 'CapPrm', 'CapEff', 'CapAmb'):
        found = re.search(rf'^{name}:\s+([0-9a-f]+)', status, re.M)

        if found is None:
            continue  # CapAmb needs Linux 4.3.

        assert int(found.group(1), 16) == 0, f'app {name}'

    # The real assertion: nxt_capability_drop() ran, and ran in exactly
    # the processes it is supposed to.  Unlike the zeroes above, none of
    # this can pass by accident -- on the commit before this one the
    # file does not exist at all.
    main_pid = int(unitd.read(unitd.pidfile).strip())
    proto_pid = int(
        re.search(r'^PPid:\s+(\d+)', status, re.M).group(1)
    )
    dropped = unitd.capset_pids()

    # Main must never drop: it binds listening sockets in
    # nxt_main_listening_socket() on every reconfiguration and so needs
    # CAP_NET_BIND_SERVICE for as long as it lives.
    assert main_pid not in dropped, 'main keeps its capabilities'

    # The prototype must, since it is the ancestor of every worker.
    assert proto_pid in dropped, 'the prototype drops'

    # The worker must not: nxt_app_setup() calls init->start directly
    # without going through nxt_process_apply_creds(), so it inherits
    # sets the prototype already emptied.  If that ever changes this
    # assertion is the thing that says so.
    assert app_pid not in dropped, 'the worker inherits rather than drops'

    # The application inherited the shim -- fork() carries the mapping,
    # armed, whatever the environment says afterwards ...
    maps = Path(f'/proc/{app_pid}/maps').read_text(encoding='utf-8')
    assert shim in maps, 'shim inherited by the application'

    # ... and still only the main process ever had a capget() filtered,
    # which is what makes the inheritance harmless.  Two entries: the
    # version probe and the call that matters.
    main_pid = int(unitd.read(unitd.pidfile).strip())
    assert unitd.filtered_pids() == {main_pid}, 'only unitd asked'


def test_capget_einval_still_fatal(unitd_factory):
    unitd = unitd_factory(errno.EINVAL)

    unitd.process.wait(timeout=30)
    assert unitd.process.returncode != 0, 'unitd exits'

    # nxt_runtime_conf_init() fails before the log file is created, so
    # stderr is the only place this can be recorded.
    assert not Path(unitd.log).exists(), 'no unit.log'
    assert re.search(ALERT, unitd.read(unitd.stderr), re.M), 'alert on stderr'
