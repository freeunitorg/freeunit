"""Application-process lifecycle churn, parametrized over every runtime.

Rationale
---------
The libunit port/fd lifecycle -- nxt_unit_add_port() and the add_port /
remove_port callbacks, nxt_unit_port_release()'s destroy path, and
nxt_unit_process_msg()'s `done:` descriptor cleanup -- lives in the
*application* process, and every runtime links the same src/nxt_unit.c.  A bug
there (a double close(), a leaked descriptor, a use-after-free of a port
struct) surfaces as an "[alert]" line written by the app process into unit.log
during port teardown, or as a slowly growing /proc/<app pid>/fd.

Before this module the suite exercised those paths deliberately in exactly one
runtime -- test_go_application.py's two churn tests, added alongside the Go
port-fd double-close fix.  Every other runtime got only incidental coverage
from the conftest `run` fixture calling Log.check_alerts() after tests that do
little more than "send a request, check the body", so an app-side double close
could reach CI as nothing but an intermittent alert in one runtime's log.

These tests deliberately churn the app-side port/fd lifecycle -- worker count
up and down, /control/.../restart storms, whole-application removal and re-add,
requests in flight across all of it -- and lean on the harness's existing
teardown assertions to fail the run:

  * conftest `run` fixture -> Log.check_alerts(): any "[alert]" from the app
    process (e.g. "close(N) failed: Bad file descriptor") fails the test, and
    any "Sanitizer" line fails it in the ASan/UBSan legs.
  * conftest `_check_fds()`: descriptor leaks in main/router/controller
    (with --fds-threshold=0, the CI default).

plus one assertion the harness makes nowhere else:

  * test_app_lifecycle_app_fds_stable(): descriptor growth inside the
    *application* process itself.

Runtime coverage is table-driven (RUNTIMES below); a runtime that is not built
into this unitd self-skips, so the same file can run in every per-language CI
leg with only that leg's parameter executing.  Adding a runtime is one entry.

CI wiring note: .github/workflows/build-test.yml derives a per-module test glob
(test/test_<module>*), so this file is named explicitly in that job's
`testpath` -- otherwise only the python/unit legs would ever run it.
"""

import os
import subprocess
import time
from pathlib import Path

import pytest

from unit.applications.lang.go import ApplicationGo
from unit.applications.lang.java import ApplicationJava
from unit.applications.lang.node import ApplicationNode
from unit.applications.lang.perl import ApplicationPerl
from unit.applications.lang.php import ApplicationPHP
from unit.applications.lang.python import ApplicationPython
from unit.applications.lang.ruby import ApplicationRuby
from unit.applications.lang.wasm_component import ApplicationWasmComponent
from unit.applications.proto import ApplicationProto
from unit.option import option

# Deliberately no module-level `client`: conftest.pytest_generate_tests() keys
# on it to fan a module out over language versions, which would collide with
# the per-runtime parametrization below.  Deliberately no module-level
# `prerequisites` either: availability is per parameter, checked in `app`.


class ApplicationLibunit(ApplicationProto):
    """The bare C libunit app, src/test/nxt_unit_app_test.c.

    Needs no language toolchain -- `./configure --tests && make tests` builds
    it -- and links libunit directly, so it exercises the port/fd lifecycle
    under test with nothing else in the way.  It answers with a "Request data:"
    dump that embeds the request body rather than mirroring it verbatim, hence
    mirror=False in the table below.
    """

    application_type = 'external'

    @staticmethod
    def executable():
        return f'{option.current_dir}/build/unit_app_test'

    def load(self, script, **kwargs):
        self._load_conf(
            {
                "listeners": {"*:8080": {"pass": f"applications/{script}"}},
                "applications": {
                    script: {
                        "type": "external",
                        "processes": {"spare": 0},
                        "working_directory": option.temp_dir,
                        "executable": ApplicationLibunit.executable(),
                    },
                },
            },
            **kwargs,
        )


# runtime -> (client factory, app name, required modules, required features,
#             body is mirrored back verbatim,
#             answers a body larger than one shared-memory segment)
#
# The last flag is false only for the libunit sample: it sizes its response
# buffer from the whole request buffer (src/test/nxt_unit_app_test.c), and
# nxt_unit_response_buf_alloc() refuses anything above PORT_MMAP_DATA_SIZE, so
# a request that fills a segment leaves it unable to build a response at all.
# That is a property of the sample app, not of the port handling under test.
RUNTIMES = {
    'libunit': (ApplicationLibunit, 'libunit', [], [], False, False),
    'go': (ApplicationGo, 'mirror', ['go'], [], True, True),
    'java': (ApplicationJava, 'mirror', ['java'], [], True, True),
    'node': (ApplicationNode, 'mirror', ['node'], [], True, True),
    # there is no test/perl/mirror; body_array answers a fixed 10-byte body,
    # which is enough to drive GET traffic through the churn
    'perl': (ApplicationPerl, 'body_array', ['perl'], [], False, True),
    'php': (ApplicationPHP, 'mirror', ['php'], [], True, True),
    'python': (ApplicationPython, 'mirror', ['python'], [], True, True),
    'ruby': (ApplicationRuby, 'mirror', ['ruby'], [], True, True),
    'wasm-wasi-component': (
        ApplicationWasmComponent,
        'hello_world',
        ['wasm-wasi-component'],
        ['cargo_component'],
        False,
        True,
    ),
}

BODY = '0123456789' * 200

# PORT_MMAP_DATA_SIZE (src/nxt_port_memory_int.h): the payload area of one
# shared-memory segment, and therefore the amount of request data the router
# can hand an application before it has to create another one.
SEGMENT = 10 * 1024 * 1024

KEEPALIVE = {'Host': 'localhost', 'Connection': 'keep-alive'}


class LifecycleApp:
    """Ties a runtime's client to its app name and request shape."""

    def __init__(self, runtime, client, name, mirror, oversized):
        self.runtime = runtime
        self.client = client
        self.name = name
        self.mirror = mirror
        self.oversized = oversized

    def load(self, **kwargs):
        self.client.load(self.name, **kwargs)

    def conf_processes(self, conf):
        assert 'success' in self.client.conf(
            conf, f'applications/{self.name}/processes'
        ), f'{self.runtime}: configure processes'

    def restart(self):
        assert 'success' in self.client.conf_get(
            f'/control/applications/{self.name}/restart'
        ), f'{self.runtime}: restart'

    def request(self, sock=None, keepalive=False, **kwargs):
        """One request that forces real request/response traffic.

        POST with a body where the runtime mirrors it back -- bodies travel
        through the shared-memory and spool-file paths that carry descriptors
        to the app -- plain GET otherwise.  Returns (response, socket); the
        socket is None unless the connection was kept alive.
        """
        if keepalive or sock is not None:
            # A COPY.  HTTP1.http() writes Content-Length into the dict it
            # is handed (test/unit/http.py), so passing the module-level
            # object would leave that header in it for good: the first
            # mirrored POST poisons every later keep-alive GET, which then
            # advertises a body it never sends and hangs until the read
            # timeout.  Only one language is built per CI leg, so today this
            # needs two runtimes in one session to bite -- which is exactly
            # what a local full run is.
            kwargs['headers'] = dict(KEEPALIVE)
            kwargs['start'] = True
            # the server will not close a keep-alive connection, so recvall()
            # must be allowed to stop on a short timeout instead of on EOF.
            #
            # Every keep-alive read therefore costs this timeout in full, and
            # the same timeout is all that separates "the response is
            # complete" from "the app has not answered yet".  Two seconds
            # buys margin over an ASan-instrumented worker on a loaded runner
            # without paying much for it; the startup race that would need
            # more than that is closed by waiting for the workers instead.
            kwargs['read_timeout'] = 2

            if sock is not None:
                kwargs['sock'] = sock

        if self.mirror:
            out = self.client.post(body=BODY, **kwargs)
        else:
            out = self.client.get(**kwargs)

        resp, sock = out if isinstance(out, tuple) else (out, None)

        # recvall() returns b'' rather than failing when a non-default
        # read_timeout expires, and an empty response parses to {} -- which
        # would surface as a KeyError instead of naming the runtime that went
        # quiet.
        assert 'status' in resp, f'{self.runtime}: no response before timeout'

        assert resp['status'] == 200, f'{self.runtime}: status'

        if self.mirror:
            assert resp['body'] == BODY, f'{self.runtime}: mirror'

        return resp, sock

    def oversized_request(self, pool):
        """One POST the router's `pool` existing segments cannot serve.

        nxt_router_prepare_msg() (src/nxt_router.c) copies the whole in-memory
        body into the router's outgoing segments for this application, filling
        a segment that has room and creating another when none has.  A body of
        `pool` whole segments is therefore always one chunk too large for the
        `pool` segments already there -- the request line and fields took one
        out of the first -- and forces exactly one to be created.

        The body has to reach the application through shared memory rather
        than as a spool file, which is what the caller's body_buffer_size is
        for; a spooled body travels as a descriptor and never touches the
        segments.

        Nothing is asserted about the echoed body: the size comes from the
        router's geometry, not from what a runtime is willing to hand back,
        and the claim here is that the router created a segment, which the
        caller checks directly.
        """

        resp = self.client.post(
            body='x' * (pool * SEGMENT), read_buffer_size=1024 * 1024
        )

        assert resp['status'] == 200, f'{self.runtime}: oversized status'

    def pids(self):
        # Worker pids come from the PROTOTYPE's children, not from the
        # workers' own process titles.
        #
        # `unit: "<name>" application` is set by nxt_process_title() in the
        # forked child (src/nxt_process.c), and for an embedded module that
        # is the end of it.  A `"type": "external"` application -- Go, Node,
        # and the libunit test app -- then execve()s the target binary
        # (src/nxt_external.c), which replaces argv[] wholesale, so the
        # worker shows up in ps as its own executable and the title is gone.
        # Matching on it finds nothing for exactly the runtimes this module
        # most wants to cover.
        #
        # The prototype does not execve, so `unit: "<name>" prototype`
        # (built in src/nxt_main_process.c) survives for every runtime, and
        # its children are the workers.  ppid is read from ps rather than
        # via `--ppid`, which is procps-specific.
        time.sleep(0.2)

        output = subprocess.check_output(
            ['ps', 'ax', '-o', 'pid=', '-o', 'ppid=', '-o', 'args=']
        ).decode()

        rows = []
        for line in output.splitlines():
            parts = line.split(None, 2)

            if len(parts) == 3:
                rows.append(parts)

        marker = f'unit: "{self.name}" prototype'
        prototypes = {pid for pid, _, args in rows if marker in args}

        if not prototypes:
            return set()

        return {pid for pid, ppid, _ in rows if ppid in prototypes}

    def wait_for_pids(self, count, timeout=15):
        # `timeout` is nominal: pids() sleeps 0.2s of its own per sample, so
        # the real ceiling is about twice this.  Left as-is rather than
        # tightened, because the margin is what absorbs a slow sanitizer
        # teardown; the name is the thing that lies, so it is documented.
        for _ in range(timeout * 5):
            pids = self.pids()

            if len(pids) == count:
                return pids

            time.sleep(0.2)

        return self.pids()

    def alive(self, pids):
        """Which of `pids` still exist, prototype or no prototype.

        pids() derives workers from the prototype's children, so a teardown
        that removes the prototype BEFORE its children leaves those children
        reparented and invisible to it -- and an absent prototype would then
        read as "no workers left", which is exactly the conclusion the
        teardown assertions must not reach by default.  Asking about
        specific pids keeps them honest: a worker that outlives its
        prototype still answers here.

        A pid the kernel recycled onto an unrelated process would count as
        alive.  Over the seconds these tests span that is remote, and it
        would fail an assertion rather than pass one, which is the safe
        direction to be wrong in.
        """

        if not pids:
            return set()

        output = subprocess.check_output(['ps', 'ax', '-o', 'pid=']).decode()
        running = {
            line.strip() for line in output.splitlines() if line.strip()
        }

        return set(pids) & running

    def wait_for_gone(self, pids, timeout=15):
        """Wait for every pid in `pids` to leave the process table."""

        for _ in range(timeout * 5):
            left = self.alive(pids)

            if not left:
                return set()

            time.sleep(0.2)

        return self.alive(pids)


def _skip_reason(runtime):
    _, _, modules, features, _, _ = RUNTIMES[runtime]

    if runtime == 'libunit':
        # built only by `./configure --tests && make tests`
        if not Path(ApplicationLibunit.executable()).is_file():
            return 'build/unit_app_test not built (configure --tests)'

        return None

    available = option.available['modules']
    missed = [m for m in modules if not available.get(m)]
    if missed:
        return f'no {", ".join(missed)} module(s)'

    available = option.available['features']
    missed = [f for f in features if not available.get(f)]
    if missed:
        return f'{", ".join(missed)} feature(s) not supported'

    return None


@pytest.fixture(params=list(RUNTIMES))
def app(request):
    runtime = request.param

    reason = _skip_reason(runtime)
    if reason is not None:
        pytest.skip(f'{runtime}: {reason}')

    factory, name, _, _, mirror, oversized = RUNTIMES[runtime]

    return LifecycleApp(runtime, factory(), name, mirror, oversized)


def count_fds(pid):
    """Descriptors of the kinds libunit hands an application process.

    A raw count of /proc/<pid>/fd cannot tell a libunit leak from the host
    runtime opening something of its own, and some runtimes legitimately do:
    a JVM may open a jar, a JIT artefact or an eventfd part-way through a run
    and keep it.  Counting everything made this test fail on java-26 for a
    single descriptor while java-17, 21 and 25 passed -- a difference in the
    JVM, not in the port handling this test exists to watch.

    So count only what the docstring of the test actually claims: sockets
    (ports), shared memory segments (incoming mmap), and regular files under
    the test's own temp dir (request spool files).  Anything the language
    runtime opens for its own purposes is deliberately out of scope, and a
    real leak of a port, a segment or a spool file still fails.
    """

    fds = Path(f'/proc/{pid}/fd')

    if not fds.is_dir():
        return None

    n = 0

    try:
        for fd in fds.iterdir():
            try:
                target = os.readlink(fd)

            except OSError:
                # the descriptor closed underneath us; it is not a leak
                continue

            if (
                target.startswith('socket:')
                or target.startswith('/memfd:')
                or target.startswith('/dev/shm/')
                or target.startswith(f'{option.temp_dir}/')
            ):
                n += 1

    except OSError:
        return None

    return n


def fd_targets(pid):
    """What the counted descriptors of `pid` point at, for a failure message.

    A bare count says growth happened; this says what grew, which is the
    difference between a report someone can act on and one that starts an
    investigation from nothing.
    """

    fds = Path(f'/proc/{pid}/fd')

    if not fds.is_dir():
        return set()

    targets = set()

    try:
        for fd in fds.iterdir():
            try:
                targets.add(os.readlink(fd))

            except OSError:
                continue

    except OSError:
        return set()

    return targets


def shm_segments(pid):
    """Shared-memory segments this process was handed by another one.

    The descriptor that carries a segment is the one the fd test is blind to
    by default: nxt_unit_incoming_mmap() (src/nxt_unit.c) maps it and
    nxt_unit_process_msg()'s `done:` closes it, so a healthy application keeps
    the mapping and no descriptor -- which is exactly why a *leak* of that
    descriptor is only visible in a window where a segment actually arrived.
    Counting the mappings is how the test knows a window contained one.

    Read from /proc/<pid>/maps, and narrowed three ways, because a process
    maps shared memory for several unrelated reasons.  Segment-sized, which
    drops the port queues (a few pages each).  Named `unit.`, which drops
    whatever the language runtime maps for itself.  Not named `unit.<pid>.`,
    which drops the segments this process created for its own responses --
    it opens and closes those itself (nxt_unit_new_mmap()), so they carry no
    incoming descriptor to leak.

    Returns the segment names rather than a count, so a failure can say which
    ones were there.
    """

    maps = Path(f'/proc/{pid}/maps')

    segments = set()

    try:
        lines = maps.read_text().splitlines()

    except OSError:
        return segments

    for line in lines:
        parts = line.split(None, 5)

        if len(parts) < 6:
            continue

        bounds, path = parts[0], parts[5]

        if not path.startswith('/memfd:') and not path.startswith('/dev/shm/'):
            continue

        if f'unit.{pid}.' in path or 'unit.' not in path:
            continue

        start, _, end = bounds.partition('-')

        if int(end, 16) - int(start, 16) < SEGMENT:
            continue

        segments.add(path)

    return segments


def test_app_lifecycle_process_churn(app):
    """Scale the worker pool up and down with traffic in between.

    A scale-up spawns workers that run the full libunit port handshake
    (NEW_PORT carrying a socket and a queue descriptor, the add_port callback,
    PORT_ACK); a scale-down tears them down (QUIT, REMOVE_PID, port destroy).
    Generalization of test_go_application.py's
    test_go_application_port_fd_churn to every runtime.

    The loop does NOT wait for each pool size to be reached, which is
    deliberate: conf_processes() returns on configuration success, not on
    spawn, so an un-awaited rewrite races the next teardown against workers
    that are still starting -- the overlap this test exists to put under a
    sanitizer.  That also means a given iteration may spawn fewer than four
    workers, so the count is not asserted per round.  One awaited cycle runs
    first, to establish that scaling works at all before the racing starts;
    without it a run in which no worker was ever spawned would look the same
    as a run in which every one of them was.
    """
    app.load()

    app.conf_processes('4')
    assert len(app.wait_for_pids(4)) == 4, f'{app.runtime}: scaled up to 4'

    app.conf_processes('1')
    assert len(app.wait_for_pids(1)) == 1, f'{app.runtime}: scaled down to 1'

    for _ in range(10):
        app.conf_processes('4')
        app.request()

        app.conf_processes('1')
        app.request()


def test_app_lifecycle_restart_control(app):
    """/control/applications/<name>/restart storm.

    A control restart replaces every worker: the outgoing generation is sent
    QUIT and drops its ports while the incoming one announces fresh ports to
    the same router.  The pid-rotation assertion keeps a no-op restart from
    making this pass vacuously.
    """
    app.load(processes=2)

    pids = app.wait_for_pids(2)
    assert len(pids) == 2, f'{app.runtime}: two workers'

    for _ in range(5):
        app.restart()

        # Wait for the OLD generation to go before sampling the new one.
        # nxt_router_app_restart_handler() (src/nxt_router.c) sends QUIT to
        # the old prototype and replies RPC_READY_LAST in the same handler,
        # so restart() returns while the old prototype and its workers are
        # normally still alive.  Sampling first would take the first set of
        # two it sees -- which can be the two OLD workers, or one of each
        # while both prototypes coexist -- and the disjointness assertion
        # below would then fail a perfectly healthy restart.  Under the
        # sanitizer, where teardown is slow, that is the likely outcome
        # rather than the unlucky one.
        assert (
            app.wait_for_gone(pids) == set()
        ), f'{app.runtime}: workers from the previous generation still alive'

        new_pids = app.wait_for_pids(2)

        assert len(new_pids) == 2, f'{app.runtime}: two workers after restart'

        # Kept as the pid-reuse guard: with the old generation already gone,
        # an overlap here would mean the kernel handed a new worker a pid we
        # had just watched die.
        assert not new_pids & pids, f'{app.runtime}: all workers replaced'

        app.request()

        pids = new_pids


def test_app_lifecycle_reconfigure_storm(app):
    """Remove and re-add the whole application repeatedly.

    Clearing `applications` makes the router QUIT every worker and drop the
    app's ports; the app process then runs nxt_unit_quit() ->
    ctx_free/lib_release, which is where the port destroy path closes the
    descriptors libunit owns.  Re-adding runs the whole spawn/handshake again.

    The configuration is captured once and replayed, because several runtime
    clients stage their application into the temp dir on load() and cannot
    stage it twice.
    """
    app.load(processes=2)
    app.request()

    conf = app.client.conf_get('/config')

    for _ in range(5):
        # Not a bare pids() snapshot.  Starting the pool is asynchronous and
        # app.request() only proves ONE worker is ready, so an unvalidated
        # sample can hold one of the two while the second is still spawning.
        # A worker missing from `before` is one wait_for_gone() will never
        # wait for, and wait_for_pids(0) would then be satisfied by the
        # absent prototype again -- putting the hole straight back.
        #
        # "processes": 2 here is a fixed count, not a spare/max range, so the
        # expected number is exact.
        before = app.wait_for_pids(2)

        assert len(before) == 2, (
            f'{app.runtime}: expected 2 workers before removal, '
            f'saw {sorted(before)}'
        )

        assert 'success' in app.client.conf(
            {"listeners": {}, "applications": {}}
        ), f'{app.runtime}: clear conf'

        # Both checks are needed.  wait_for_gone() names the processes that
        # were serving a moment ago and waits for those to die, which an
        # early prototype exit cannot fake; wait_for_pids(0) then confirms
        # nothing was started to replace them.
        assert (
            app.wait_for_gone(before) == set()
        ), f'{app.runtime}: pre-removal workers still alive'

        assert app.wait_for_pids(0) == set(), f'{app.runtime}: workers gone'

        assert 'success' in app.client.conf(conf), f'{app.runtime}: restore'

        app.request()


def test_app_lifecycle_churn_under_keepalive(app):
    """Churn the worker pool while keep-alive connections stay open.

    Requests arriving on connections that outlive a worker generation are the
    case where a router-side response port and an app-side port lifecycle
    overlap -- the interleaving in which a stale descriptor is most likely to
    be closed a second time.
    """
    app.load(processes=2)

    # not decoration: the first keep-alive read is the one racing application
    # startup, and it gives up after read_timeout with whatever it has rather
    # than failing, so a worker that is not up yet reads as an empty response.
    app.wait_for_pids(2)

    socks = []
    try:
        for _ in range(3):
            _, sock = app.request(keepalive=True)
            socks.append(sock)

        for i in range(4):
            app.conf_processes('4' if i % 2 else '1')

            for j, sock in enumerate(socks):
                _, socks[j] = app.request(sock=sock)

    finally:
        for sock in socks:
            if sock is not None:
                sock.close()


def test_app_lifecycle_app_fds_stable(app):
    """No per-request descriptor growth inside the application process.

    conftest's _check_fds() only watches main/router/controller.  The libunit
    paths that hand descriptors to an app -- incoming mmap segments, request
    spool files, port sockets -- are only observable here, so a leak of one
    descriptor per request or per port announcement is invisible to the rest
    of the suite.

    Measured across TWO identical bursts rather than before-and-after one.
    A single burst cannot tell a leak from lazy one-time allocation, and the
    app process does allocate lazily: a further incoming mmap segment or an
    additional router port arrives when it is first needed, not during the
    warm-up.  Counting one window made this fail for a single descriptor on
    java-26 and again on python, both times for growth that never repeated.
    A leak is growth that keeps happening, so the claim is about the SECOND
    window: whatever the first burst settles, the second must not add to it.

    Each burst ends with one oversized POST, and that is not decoration.  The
    router creates a segment only when the ones it has for this application
    cannot serve a request, and 2000-byte requests release their chunks as
    they complete, so a burst of them reuses the segment the warm-up caused
    and never makes the router announce another.  A descriptor leaked per
    incoming segment would then already be inside BOTH counts and cancel out
    -- the one leak this test names in its own docstring, invisible to it.
    The oversized request forces an announcement; the burst asserts that it
    happened, so the probe cannot quietly stop working.

    The POST grows by one segment per burst because the pool grows with it:
    the segment the first burst forced is free again by the time the second
    runs, and would swallow a repeat of the same body.  What stays equal
    between the two windows is the thing being counted -- one segment
    created, one descriptor received -- which is what makes the difference
    between them a leak rather than a workload difference.

    Runtimes whose application cannot answer such a request keep the bursts
    and lose only the segment part of the claim (see RUNTIMES); driving one
    into a response it cannot build would test the sample app, not libunit.
    """
    app.load(processes=1)

    # The oversized body must stay in memory to travel through shared memory
    # at all: past body_buffer_size the router spools it to a file and sends
    # a descriptor instead (nxt_h1p_request_body_read(), src/nxt_h1proto.c).
    #
    # The margin over the largest body sent below is deliberate.  Sitting on
    # the boundary would make the probe depend on which side of it a spooling
    # test is written, and the wasm component's read loop
    # (src/wasm-wasi-component/src/lib.rs) does not terminate on a short read,
    # so a body that spooled would hang that leg rather than fail it.
    assert 'success' in app.client.conf(
        {
            "http": {
                "body_buffer_size": 4 * SEGMENT,
                "max_body_size": 4 * SEGMENT,
            }
        },
        'settings',
    ), f'{app.runtime}: buffer oversized bodies in memory'

    pids = app.wait_for_pids(1)
    assert len(pids) == 1, f'{app.runtime}: single worker'

    pid = pids.pop()

    # warm-up: the first requests create the outgoing mmap segments and, in
    # threaded runtimes, the per-thread contexts
    for _ in range(10):
        app.request()

    def burst(pool):
        for _ in range(40):
            app.request()

        if app.oversized:
            app.oversized_request(pool)

        # let anything the burst opened and is about to close settle, so a
        # descriptor in flight is not counted as retained
        settled = count_fds(pid)

        for _ in range(50):
            n = count_fds(pid)

            if n is None or n == settled:
                break

            settled = n
            time.sleep(0.1)

        return settled

    if count_fds(pid) is None:
        pytest.skip('no /proc/<pid>/fd on this platform')

    warm = shm_segments(pid)

    first = burst(1)
    after_first = shm_segments(pid)

    second = burst(2)
    after_second = shm_segments(pid)

    assert app.pids() == {pid}, f'{app.runtime}: same worker throughout'

    if app.oversized:
        # Both windows, not just the second: a first burst that forced
        # nothing would leave its own leak out of `first` and make the
        # comparison below read a per-segment leak as one-time growth.
        assert len(after_first) > len(warm), (
            f'{app.runtime}: no incoming segment in the first burst, so a '
            f'leak of one cannot be seen: {sorted(warm)} -> '
            f'{sorted(after_first)}'
        )

        assert len(after_second) > len(after_first), (
            f'{app.runtime}: no incoming segment in the second burst, so a '
            f'leak of one cannot be seen: {sorted(after_first)} -> '
            f'{sorted(after_second)}'
        )

    assert first is not None and second is not None, (
        f'{app.runtime}: worker vanished mid-measurement'
    )

    assert second - first <= option.fds_threshold, (
        f'{app.runtime}: descriptor leak in application process: '
        f'{first} -> {second} across a repeated identical burst; '
        f'open now: {sorted(fd_targets(pid))}'
    )
