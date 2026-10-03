import ctypes
import os
import shutil
import signal
import tempfile
import threading
import time
from pathlib import Path

import pytest

from conftest import pid_by_name, unit_run, unit_stop
from unit.applications.proto import ApplicationProto
from unit.option import option

prerequisites = {'modules': {'njs': 'any'}}

client = ApplicationProto()


def njs_script_load(module, name=None, expect='success'):
    if name is None:
        name = module

    with open(f'{option.test_dir}/njs/{module}/script.js', 'rb') as script:
        assert expect in client.conf(script.read(), f'/js_modules/{name}')


def test_njs_modules():
    njs_script_load('next')

    assert 'export' in client.conf_get('/js_modules/next')
    assert 'error' in client.conf_post('"blah"', '/js_modules/next')

    assert 'success' in client.conf(
        {
            "settings": {"js_module": "next"},
            "listeners": {"*:8080": {"pass": "routes/first"}},
            "routes": {
                "first": [{"action": {"pass": "`routes/${next.route()}`"}}],
                "next": [{"action": {"return": 200}}],
            },
        }
    )
    assert client.get()['status'] == 200, 'string'

    assert 'success' in client.conf({"js_module": ["next"]}, 'settings')
    assert client.get()['status'] == 200, 'array'

    # add one more value to array

    assert len(client.conf_get('/js_modules').keys()) == 1

    njs_script_load('next', 'next_2')

    assert len(client.conf_get('/js_modules').keys()) == 2

    assert 'success' in client.conf_post('"next_2"', 'settings/js_module')
    assert client.get()['status'] == 200, 'array len 2'

    assert 'success' in client.conf(
        '"`routes/${next_2.route()}`"', 'routes/first/0/action/pass'
    )
    assert client.get()['status'] == 200, 'array new'

    # can't update exsisting script

    njs_script_load('global_this', 'next', expect='error')

    # delete modules

    assert 'error' in client.conf_delete('/js_modules/next_2')
    assert 'success' in client.conf_delete('settings/js_module')
    assert 'success' in client.conf_delete('/js_modules/next_2')


def test_njs_modules_import():
    njs_script_load('import_from')

    assert 'success' in client.conf(
        {
            "settings": {"js_module": "import_from"},
            "listeners": {"*:8080": {"pass": "routes/first"}},
            "routes": {
                "first": [
                    {"action": {"pass": "`routes/${import_from.num()}`"}}
                ],
                "number": [{"action": {"return": 200}}],
            },
        }
    )
    assert client.get()['status'] == 200


def test_njs_modules_this():
    njs_script_load('global_this')

    assert 'success' in client.conf(
        {
            "settings": {"js_module": "global_this"},
            "listeners": {"*:8080": {"pass": "routes/first"}},
            "routes": {
                "first": [
                    {"action": {"pass": "`routes/${global_this.str()}`"}}
                ],
                "string": [{"action": {"return": 200}}],
            },
        }
    )
    assert client.get()['status'] == 200


def test_njs_modules_invalid(skip_alert):
    skip_alert(r'.*JS compile module.*failed.*')

    njs_script_load('invalid', expect='error')


def test_njs_settings_js_module_nul():
    # A "settings.js_module" reference is used as a NUL-terminated C-string
    # store name; an embedded NUL (which survives JSON parsing in a
    # length-tracked nxt_str_t) or an empty value must be rejected by the
    # c-string validator, before the module-store lookup.
    #
    # The module-store lookup is length-aware and would also reject these
    # values (as "not found"), so assert the validator's own diagnostic to
    # prove the c-string guard ran rather than the lookup merely failing.
    njs_script_load('next')

    assert 'success' in client.conf({"js_module": "next"}, 'settings'), 'valid'

    resp = client.conf({"js_module": "next\0x"}, 'settings')
    assert 'null character' in resp.get('detail', ''), 'nul'

    resp = client.conf({"js_module": ""}, 'settings')
    assert 'must not be empty' in resp.get('detail', ''), 'empty'

    resp = client.conf({"js_module": ["next\0x"]}, 'settings')
    assert 'null character' in resp.get('detail', ''), 'array nul'


SHORT_MODULE = b"""export default {
    "route": function() {return 'next'}
}
"""

LONG_MODULE = b"""export default {
    "route": function() {return 'next'},
    "other": function() {return '%s'}
}
""" % (b'x' * 4096)


def route_conf(name):
    return {
        "settings": {"js_module": name},
        "listeners": {"*:8080": {"pass": "routes/first"}},
        "routes": {
            "first": [{"action": {"pass": f"`routes/${{{name}.route()}}`"}}],
            "next": [{"action": {"return": 200}}],
        },
    }


def start_on(statedir):
    # main writes here as root; the directory only has to be traversable.
    os.chmod(statedir, 0o755)
    unit_run(state_dir=str(statedir))


def test_njs_modules_replace_restart(requires_restart):
    """A module deleted and uploaded again with a shorter text is stored
    and loaded as the short text.  Main deletes and stores in its store
    child, in the order the requests came."""

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    stored = statedir / 'scripts' / 'm'

    try:
        start_on(statedir)

        assert 'success' in client.conf(LONG_MODULE, '/js_modules/m')
        assert stored.read_bytes() == LONG_MODULE, 'long stored'

        assert 'success' in client.conf_delete('/js_modules/m')
        assert 'success' in client.conf(SHORT_MODULE, '/js_modules/m')
        assert stored.read_bytes() == SHORT_MODULE, 'short stored'

        unit_stop()
        start_on(statedir)

        assert 'm' in client.conf_get('/js_modules'), 'loaded on restart'
        assert 'success' in client.conf(route_conf('m'))
        assert client.get()['status'] == 200

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


def test_njs_modules_replace_torn_file(requires_restart, skip_alert):
    """A module file left torn on disk, as a write cut short leaves it, is
    not loaded at startup.  Uploading the module again must replace the
    whole file, not overwrite its head and keep the old tail."""

    skip_alert(r'.*JS compile module.*failed.*')

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    scripts = statedir / 'scripts'
    stored = scripts / 'm'

    try:
        # The first half of LONG_MODULE: a write cut short.
        scripts.mkdir()
        stored.write_bytes(LONG_MODULE[: len(LONG_MODULE) // 2])

        start_on(statedir)

        assert 'm' not in client.conf_get('/js_modules'), 'torn not loaded'

        assert 'success' in client.conf(SHORT_MODULE, '/js_modules/m')

        on_disk = stored.read_bytes()
        assert on_disk == SHORT_MODULE, f'stored {len(on_disk)} bytes'

        # The router reads the module from the file, not from the upload.
        assert 'success' in client.conf(route_conf('m')), 'module compiles'
        assert client.get()['status'] == 200

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


def test_njs_modules_load_after_broken(requires_restart, skip_alert):
    """One stored module that does not compile must not hide the modules
    read after it at startup."""

    skip_alert(r'.*JS compile module.*failed.*')

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    scripts = statedir / 'scripts'

    try:
        scripts.mkdir()

        good = [f'good{i}' for i in range(8)]

        for name in good:
            (scripts / name).write_bytes(SHORT_MODULE)

        # Pick a name that readdir() returns before at least one good module;
        # startup reads the directory in that order.
        for i in range(32):
            broken = f'broken{i}'
            (scripts / broken).write_bytes(b'export default {')

            if os.listdir(scripts)[-1] != broken:
                break

            (scripts / broken).unlink()
        else:
            assert False, 'no name is read before a good module'

        start_on(statedir)

        loaded = client.conf_get('/js_modules')

        assert sorted(n for n in loaded if n in good) == good, 'all loaded'
        assert broken not in loaded

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


def test_njs_modules_name_dot():
    """Main keeps its temporary file in the scripts directory under a name
    that starts with ".", so no module name may start with "."."""

    for name in ['.x', '.store.tmp']:
        resp = client.conf(SHORT_MODULE, f'/js_modules/{name}')
        assert resp.get('error') == 'Invalid JS module name.', name

    assert client.conf_get('/js_modules') == {}


def test_njs_modules_skip_store_tmp(requires_restart):
    """A temporary file that a store left behind is not loaded as a
    module."""

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    scripts = statedir / 'scripts'

    try:
        scripts.mkdir()
        (scripts / '.store.tmp').write_bytes(SHORT_MODULE)

        start_on(statedir)

        assert client.conf_get('/js_modules') == {}

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


PTRACE_CONT = 7
PTRACE_DETACH = 17
PTRACE_GETEVENTMSG = 0x4201
PTRACE_SEIZE = 0x4206
PTRACE_INTERRUPT = 0x4207
PTRACE_O_TRACEFORK = 0x2
PTRACE_O_TRACEVFORK = 0x4
PTRACE_O_TRACECLONE = 0x8
PTRACE_EVENT_STOP = 128
WALL = 0x40000000


class ForkFreezer(threading.Thread):
    """Trace the main process with ptrace(2) and keep the first process it
    forks stopped until release().  Every later child runs untouched.
    unitd has no test hooks, so this is how a store child is held."""

    def __init__(self, pid):
        super().__init__(daemon=True)

        self.pid = pid
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.libc.ptrace.restype = ctypes.c_long
        self.libc.ptrace.argtypes = [
            ctypes.c_long,
            ctypes.c_long,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]

        self.error = None
        self.frozen = None
        self.attached = threading.Event()
        self.holding = threading.Event()
        self.release_req = threading.Event()
        self.stop_req = threading.Event()
        self.done = threading.Event()

    def ptrace(self, req, pid, addr=0, data=0):
        return self.libc.ptrace(req, pid, addr, data)

    def run(self):
        try:
            self.trace()
        finally:
            self.done.set()

    def trace(self):
        opts = PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE

        if self.ptrace(PTRACE_SEIZE, self.pid, 0, opts) != 0:
            self.error = os.strerror(ctypes.get_errno())
            self.attached.set()
            return

        self.attached.set()

        released = False
        interrupted = False

        while True:
            if self.release_req.is_set() and self.holding.is_set():
                if not released:
                    self.ptrace(PTRACE_DETACH, self.frozen)
                    released = True

            if self.stop_req.is_set() and not interrupted:
                self.ptrace(PTRACE_INTERRUPT, self.pid)
                interrupted = True

            pid, status = os.waitpid(self.pid, WALL | os.WNOHANG)

            if pid == 0:
                time.sleep(0.002)
                continue

            if not os.WIFSTOPPED(status):
                return

            sig = os.WSTOPSIG(status)
            event = status >> 16

            if event == 0:
                # A signal for main: deliver it.
                if interrupted:
                    self.ptrace(PTRACE_DETACH, self.pid, 0, sig)
                    return

                self.ptrace(PTRACE_CONT, self.pid, 0, sig)
                continue

            if event == PTRACE_EVENT_STOP:
                if interrupted:
                    self.ptrace(PTRACE_DETACH, self.pid)
                    return

                self.ptrace(PTRACE_CONT, self.pid)
                continue

            # A fork, vfork or clone event: a new traced child.
            msg = ctypes.c_ulong()
            self.ptrace(
                PTRACE_GETEVENTMSG, self.pid, 0, ctypes.addressof(msg)
            )
            child = msg.value

            # The child reports its first stop; take it before main runs on.
            os.waitpid(child, WALL)

            if self.frozen is None:
                self.frozen = child
                self.holding.set()

            else:
                self.ptrace(PTRACE_DETACH, child)

            self.ptrace(PTRACE_CONT, self.pid)

    def release(self):
        self.release_req.set()

    def stop(self):
        self.release_req.set()
        self.stop_req.set()
        self.done.wait(10)


def test_njs_modules_store_router_restart(
    requires_restart, unit_pid, skip_alert, skip_fds_check
):
    """The router restarts while main stores a module.  The controller must
    answer the upload once, when the module is on disk, and must run a
    configuration sent meanwhile only after the store."""

    skip_alert(r'process \d+ exited on signal 9')
    skip_fds_check(router=True)

    # The store of the configuration that the start of the test sent must
    # not be the child that is held.
    time.sleep(0.5)

    controller = pid_by_name('unit: controller')
    router = pid_by_name('unit: router')

    freezer = ForkFreezer(unit_pid)
    freezer.start()
    freezer.attached.wait(5)

    if freezer.error is not None:
        freezer.stop()
        pytest.skip(f'ptrace: {freezer.error}')

    answers = {}

    def put(key, body, path='/config'):
        answers[key] = client.conf(body, path)
        answers[f'{key}_time'] = time.monotonic()

    upload = threading.Thread(
        target=put, args=('module', SHORT_MODULE, '/js_modules/m')
    )
    reconf = threading.Thread(
        target=put,
        args=(
            'conf',
            {
                "listeners": {"*:8080": {"pass": "routes"}},
                "routes": [{"action": {"return": 204}}],
            },
        ),
    )

    try:
        upload.start()

        assert freezer.holding.wait(10), 'the store child is held'

        os.kill(int(router), signal.SIGKILL)

        for _ in range(100):
            new_router = pid_by_name('unit: router')
            if new_router not in (None, router):
                break

            time.sleep(0.05)

        else:
            assert False, 'the router restarted'

        # Let the controller see the new router and send it the config.
        time.sleep(1)

        reconf.start()
        time.sleep(0.5)

        assert 'module' not in answers, 'no answer before the store'
        assert 'conf' not in answers, 'no reconfiguration during the store'

        released = time.monotonic()
        freezer.release()

        upload.join(10)
        reconf.join(10)

    finally:
        freezer.stop()

    assert answers['module'].get('success') == 'JS module uploaded.'
    assert answers['module_time'] >= released
    assert 'success' in answers['conf'], 'reconfiguration'
    assert answers['conf_time'] >= released, 'after the store'

    assert pid_by_name('unit: controller') == controller, 'controller alive'
    assert 'm' in client.conf_get('/js_modules')
    assert client.get()['status'] == 204
