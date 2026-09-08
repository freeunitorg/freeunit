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

The shim can deny capset(2) as well, which is the deployment a
"SystemCallFilter=" allowlist actually produces -- a profile that omits
capget() rarely keeps capset().  Unit answers that per process: the
prototype refuses to start rather than fork workers that would inherit
what it could not drop, while the router, controller and discovery warn
and carry on, because they run no application code and killing them
would be a respawn loop or a silent death.  Both halves are asserted in
test_capset_filtered_refuses_the_application().

The refusal is conditional on there being something to refuse over, and
that is the harder half to test here, because a suite running as an
ordinary user holds nothing.  So the shim can also make capget() report
a set it does not have -- which is what lets the refusal be reached at
all -- and the opposite case, a denied capset() with genuinely empty
sets, gets its own test: it must serve, through either of the two
readers nxt_capability_still_held() can answer from.

One branch stays out of reach here, and is named rather than left to be
found: the /proc/self/status reader reporting a *non-empty* set.  The
refusal tests get their non-empty answer from the shim's fake capget(),
because the procfs reader reads the kernel and the kernel will not lie
for a suite that holds nothing; faking it would mean interposing open(),
read() and fstatfs() as well, and that fstatfs() check exists precisely
to stop a file being substituted for the real one.  What is covered
unprivileged is that reader answering "empty" rather than "cannot tell"
-- the parsing, the streaming and the fstatfs() check, all of which fail
closed into a refusal if they break.  Only the final comparison needs a
run with something genuinely held, under a filter that denies capget()
and capset() together, which is the harness smoke test's ground.

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
import stat
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

# nxt_capability_drop() when capset(2) is denied.  Deliberately matched on
# the syscall alone and not on the sentence around it: this is the test's
# proof that the injected failure reached Unit at all, and it has to hold on
# both sides of the change it is a control for.  Wording that only the fixed
# build emits would make the control fail for the wrong reason.
CAPSET_WARNING = r'\[warn\].+capset\(\) failed'

# The two lines the refusal writes: one from the prototype that would have
# forked the workers, one from the router that was waiting for it.
REFUSED = r'\[alert\].+refused to start: capset\(\) is denied'
START_FAILED = r"\[error\].+app '.+' start attempt produced no process"

# nxt_capability_still_held() giving up -- neither capget() nor
# /proc/self/status could say what the process holds.  Never expected in
# this suite; asserted absent so that a test which reaches the "serves"
# outcome cannot do so by way of a reader that silently broke.
UNDETERMINED = r'capset\(\) failed.+could not be determined'

# CAP_NET_BIND_SERVICE.  Handed to the shim so capget() reports a
# non-empty set: the suite runs with no capabilities of its own, so
# without this there is genuinely nothing to refuse over.  This
# particular bit because nxt_capability_specific_set() ignores it --
# unitd goes on behaving as the unprivileged process it really is.
HELD = 1 << 10

# The two errnos a syscall filter conventionally returns for a denied
# call.  nxt_capability.c accepts both, so both belong in the test.
FILTERED = [errno.EPERM, errno.ENOSYS]


@pytest.fixture(scope='module', autouse=True)
def requirements():
    if option.system != 'Linux':
        pytest.skip('capget() is Linux only')

    if option.is_privileged:
        pytest.skip('a root unitd never calls capget()')

    # Everything below reaches unitd through LD_PRELOAD, and everything
    # below reasons about capabilities this process can see.  A set-id bit
    # or a file capability on the binary breaks both at once: glibc treats
    # the exec as secure and drops LD_PRELOAD, so the shim never arms, and
    # the spawned unitd gets capabilities the pytest process does not have,
    # which would make the "nothing is held" premise a statement about the
    # wrong process.  Neither is true of an ordinary build, but "ordinary"
    # is the sort of thing worth checking rather than assuming.
    unitd = Path(f'{option.current_dir}/build/sbin/unitd')

    if unitd.is_file():
        if unitd.stat().st_mode & (stat.S_ISUID | stat.S_ISGID):
            pytest.skip('unitd is set-id; LD_PRELOAD would be dropped')

        try:
            os.getxattr(unitd, 'security.capability')

        except OSError:
            pass  # The ordinary case: no file capabilities.

        else:
            pytest.skip(
                'unitd has file capabilities; LD_PRELOAD would be dropped'
            )


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

    def __init__(self, shim, errno, capset_errno=0, capget_hold=0):
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

        # Only when asked, so the tests that do not deny capset() run
        # with exactly the environment they ran with before.
        if capset_errno:
            env['NXT_TEST_CAPSET_ERRNO'] = str(capset_errno)

        if capget_hold:
            env['NXT_TEST_CAPGET_HOLD'] = f'{capget_hold:x}'

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

    def factory(errno, capset_errno=0, capget_hold=0):
        instance = Unitd(shim, errno, capset_errno, capget_hold)
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


def caps_of(pid='self'):
    """The four capability sets of a process, as integers.

    CapAmb needs Linux 4.3 and CapInh has been there forever, but read
    both defensively: a set the file does not name reads as empty, which
    is the answer that cannot make a test pass by accident here.
    """
    status = Path(f'/proc/{pid}/status').read_text(encoding='utf-8')
    caps = {}

    for name in ('CapInh', 'CapPrm', 'CapEff', 'CapAmb'):
        found = re.search(rf'^{name}:\s+([0-9a-f]+)', status, re.M)
        caps[name] = int(found.group(1), 16) if found is not None else 0

    return caps


def count_matches(path, pattern):
    if not Path(path).exists():
        return 0

    text = Path(path).read_text(encoding='utf-8', errors='ignore')

    return len(re.findall(pattern, text, re.M))


def wait_for(path, pattern, timeout=150):
    for _ in range(timeout):
        if Path(path).exists():
            text = Path(path).read_text(encoding='utf-8', errors='ignore')
            found = re.search(pattern, text, re.M)

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

    assert 'success' in configure_app(unitd)['body'], 'configure'

    resp = client.get(sock_type='unix', addr=unitd.listener)
    assert resp['status'] == 200, 'application answers'

    app_pid = int(resp['body'].strip())

    # Capabilities were unknown, so setid stayed clear and the runtime
    # never resolved credentials: the application runs as unitd's uid.
    status = Path(f'/proc/{app_pid}/status').read_text(encoding='utf-8')
    uid = re.search(r'^Uid:\s+(\d+)\s+(\d+)', status, re.M)
    assert uid is not None, 'app Uid'
    assert int(uid.group(2)) == os.geteuid(), 'app runs as unitd uid'

    # The application holds no capabilities at all -- inheritable
    # included, which is the one set nxt_capability_still_held() does not
    # count but nxt_capability_drop() does still empty.  On a suite that
    # runs without any to begin with the other three cannot fail, which
    # is why they are not the real assertion; see the capset()
    # bookkeeping below.  CapInh is not in that position: this host may
    # well have one, and the drop is what clears it.
    for name, value in caps_of(app_pid).items():
        assert value == 0, f'app {name}'

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
    assert unitd.filtered_pids() == {main_pid}, 'only unitd asked'


def test_capget_einval_still_fatal(unitd_factory):
    unitd = unitd_factory(errno.EINVAL)

    unitd.process.wait(timeout=30)
    assert unitd.process.returncode != 0, 'unitd exits'

    # nxt_runtime_conf_init() fails before the log file is created, so
    # stderr is the only place this can be recorded.
    assert not Path(unitd.log).exists(), 'no unit.log'
    assert re.search(ALERT, unitd.read(unitd.stderr), re.M), 'alert on stderr'


def configure_app(unitd):
    """Load a one-process application and return the config response."""
    appdir = f'{unitd.dir}/app'
    Path(appdir).mkdir()
    public_dir(appdir)

    conf = {
        "listeners": {f'unix:{unitd.listener}': {"pass": "applications/app"}},
        "applications": {"app": app_conf(appdir)},
    }

    return client.put(
        url='/config',
        sock_type='unix',
        addr=unitd.control,
        body=json.dumps(conf),
    )


@pytest.mark.parametrize('filtered_errno', FILTERED, ids=errno.errorcode.get)
def test_capset_filtered_refuses_the_application(
    shim, unitd_factory, filtered_errno
):
    """A denied capset() with capabilities still held must refuse.

    This is the deployment the whole file is about, taken one step
    further: a "SystemCallFilter=" allowlist that omits capget() rarely
    keeps capset(), so the fallback that lets unitd start lands in a
    prototype that then cannot give up what it holds.  Warning and
    continuing there would hand unitd's capabilities to application
    code -- silently, since the operator asked for neither.

    The refusal is deliberately not made inside nxt_capability_drop().
    That function is also on the router's, the controller's and
    discovery's path through nxt_process_core_setup(), and those three
    run no application code: router and controller are re-forked
    immediately and without backoff, so refusing there would be a
    respawn loop, and discovery notifies nobody when it dies.  Both
    halves are asserted below -- unitd comes up and stays up, and only
    the application start fails.
    """
    unitd = unitd_factory(0, capset_errno=filtered_errno, capget_hold=HELD)

    assert waitforfiles(unitd.control), (
        f'unitd started; its stderr was:\n{unitd.read(unitd.stderr)}'
    )

    # The injected failure reached Unit, and nxt_capability_still_held()
    # found something to refuse over -- without both, the rest of this
    # test could pass against a shim that never fired.  And the core
    # processes met the same denial in nxt_process_core_setup() and
    # carried on: the controller is answering on the socket above and the
    # router logged its own start line.
    assert wait_for(unitd.log, CAPSET_WARNING) is not None, 'capset denied'
    assert wait_for(unitd.log, r'router started') is not None, 'router started'

    assert 'success' in configure_app(unitd)['body'], 'configure'

    # The application does not serve.  503 rather than a hang: the
    # prototype exits, main's SIGCHLD reaper notifies the router with the
    # start's stream still attached, and the router fails the requests
    # parked on the application instead of letting them time out.
    resp = client.get(sock_type='unix', addr=unitd.listener)
    assert resp['status'] == 503, 'application refused'

    # Both ends of that route say so above debug level, which is what an
    # operator sees in a default configuration: the prototype names the
    # syscall and the consequence, the router names the application.
    assert wait_for(unitd.log, REFUSED) is not None, 'prototype refuses'
    assert wait_for(unitd.log, START_FAILED) is not None, 'router reports'

    # No respawn loop.  Nothing in Unit re-arms a start on its own --
    # every nxt_router_start_app_process() call site is driven by a
    # request, a port becoming ready or a configuration change -- so
    # after the one attempt above the count must stay where it is.
    refusals = count_matches(unitd.log, REFUSED)
    assert refusals == 1, 'exactly one attempt'

    time.sleep(2)
    assert count_matches(unitd.log, REFUSED) == refusals, 'no retry on its own'

    # unitd itself is unharmed, which is the half that would have been
    # lost by refusing inside nxt_capability_drop().
    assert unitd.process.poll() is None, 'unitd still running'
    resp = client.get(url='/config', sock_type='unix', addr=unitd.control)
    assert resp['status'] == 200, 'controller still answering'

    # The failure is stable rather than latched: a second request makes a
    # second attempt, and that attempt fails the same way.  Only the 503
    # is asserted -- whether Unit remembers the refusal or repeats it is
    # an implementation choice this test does not fix.
    resp = client.get(sock_type='unix', addr=unitd.listener)
    assert resp['status'] == 503, 'still refused'

    # Main must still never appear among the processes that tried to
    # drop, filter or no filter.
    main_pid = int(unitd.read(unitd.pidfile).strip())
    tried = unitd.capset_pids()
    assert tried, 'somebody tried to drop'
    assert main_pid not in tried, 'main never tries'


# Which reader nxt_capability_still_held() has to answer from: capget()
# when the filter left it alone, /proc/self/status when it did not.
READERS = {'capget': 0, 'proc': errno.EPERM}


@pytest.mark.parametrize('reader', READERS)
def test_capset_filtered_serves_when_nothing_is_held(
    shim, unitd_factory, reader
):
    """A denied capset() with nothing to drop must not refuse anything.

    The refusal above is conditional on actually holding something, and
    that condition is not decoration.  Both of the commonest deployments
    reach nxt_capability_drop() with every set already empty: a root
    unitd, because switching away from uid 0 clears them, and an
    ordinary unprivileged unitd, because it was never granted anything.
    A filter that denies capset() in either of those denies a call that
    had nothing left to do, and refusing over it would take every
    application down for no security benefit whatsoever.

    Both readers are exercised, because they cover different filters and
    a broken one fails the same way in each: silently, by returning "I
    cannot tell", which is treated as a refusal.  That is why the
    absence of the "could not be determined" warning is asserted rather
    than assumed -- without it this test would still pass with both
    readers removed, on the strength of a refusal that never happened.
    """
    # The premise, stated rather than inherited from whatever the host
    # happens to be.  unitd is about to be forked from this process, so
    # "nothing is held" is a property of this process, and the reader is
    # going to read it back through whichever of the two legs is under
    # test.  Without this the test says only "the host agreed with us
    # today": on a runner with empty sets it passes for the right reason,
    # and on a host that does hold something it would fail with a 503 and
    # no clue as to why.
    caps = caps_of()
    held = {
        name: caps[name]
        for name in ('CapPrm', 'CapEff', 'CapAmb')
        if caps[name] != 0
    }

    if held:
        pytest.skip(f'the test process holds capabilities: {held}')

    # CapInh is deliberately not in that list, and this is the test that
    # says so.  An inheritable bit confers nothing without an execve() of
    # a file carrying a matching one, so nxt_capability_still_held() does
    # not count it -- and a desktop with pam_cap or a container runtime in
    # the picture commonly has one, which is exactly the host where
    # counting it would refuse every application for nothing.  This run is
    # only evidence of that when the bit is actually set, so record which
    # kind of host produced the result.
    print(f'\nCapInh of the test process: {caps["CapInh"]:016x}')

    unitd = unitd_factory(READERS[reader], capset_errno=errno.EPERM)

    assert waitforfiles(unitd.control), (
        f'unitd started; its stderr was:\n{unitd.read(unitd.stderr)}'
    )
    assert wait_for(unitd.log, r'router started') is not None, 'router started'

    assert 'success' in configure_app(unitd)['body'], 'configure'

    resp = client.get(sock_type='unix', addr=unitd.listener)
    assert resp['status'] == 200, 'application serves'

    log = unitd.read(unitd.log)
    assert re.search(UNDETERMINED, log, re.M) is None, 'the reader answered'
    assert re.search(REFUSED, log, re.M) is None, 'nothing was refused'

    # And the drop still happened where it always does, so the reader is
    # not being consulted in place of the capset() itself.
    proto_pid = int(
        re.search(
            r'^PPid:\s+(\d+)',
            Path(f'/proc/{int(resp["body"].strip())}/status').read_text(
                encoding='utf-8'
            ),
            re.M,
        ).group(1)
    )
    assert proto_pid in unitd.capset_pids(), 'the prototype still tried'
