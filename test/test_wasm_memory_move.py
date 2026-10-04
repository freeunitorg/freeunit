"""The host reads the guest memory at its new base after memory.grow moves it.

On a 64-bit host Wasmtime reserves 4 GiB for a 32-bit linear memory, so the
memory never moves.  The test starts its own unitd with
test/wasmtime_reservation.c preloaded.  The shim sets the reservation to 0, so
each memory.grow moves the memory, and it logs each new base that the host
reads.  The guest, test/wasm/grow/grow.c, grows the memory before each point
where the host reads or writes it.
"""

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

from unit.check.check_prerequisites import check_prerequisites
from unit.http import HTTP1
from unit.option import option
from unit.utils import public_dir, waitforfiles

prerequisites = {'modules': {'wasm': 'any'}, 'features': {'clang_wasm': True}}

check_prerequisites(prerequisites)

client = HTTP1()

# 1 MiB below the request buffer, then the 32 MiB + 64 KiB that the host
# asks for, rounded up to 36 MiB.
INITIAL_MEMORY = 36 * 1024 * 1024

REQUESTS = 3

# One move in the module init hook, and three in each request.
MOVES = 1 + 3 * REQUESTS

ENGINE = r'wasmtime_reservation: engine with no reservation'
MOVED = r'wasmtime_reservation: memory base moved'


@pytest.fixture(scope='module')
def shim():
    if option.system != 'Linux':
        pytest.skip('the shim is tested on Linux only')

    # The loader drops LD_PRELOAD for a set-id binary or a binary with file
    # capabilities, as in test_capget_fallback.py.
    unitd = Path(f'{option.current_dir}/build/sbin/unitd')

    if unitd.is_file():
        if unitd.stat().st_mode & (stat.S_ISUID | stat.S_ISGID):
            pytest.skip('unitd is set-id; LD_PRELOAD would be dropped')

        try:
            os.getxattr(unitd, 'security.capability')

        except OSError:
            pass  # No file capabilities.

        else:
            pytest.skip(
                'unitd has file capabilities; LD_PRELOAD would be dropped'
            )

    compiler = shutil.which('cc') or shutil.which('gcc')

    if compiler is None:
        pytest.skip('requires a C compiler')

    outdir = tempfile.mkdtemp(prefix='unit-test-wasm-')
    library = f'{outdir}/wasmtime_reservation.so'

    build = subprocess.run(
        [
            compiler,
            '-shared',
            '-fPIC',
            '-O1',
            '-o',
            library,
            f'{option.test_dir}/wasmtime_reservation.c',
            '-ldl',
        ],
        check=False,
        capture_output=True,
    )

    assert build.returncode == 0, build.stderr.decode()

    yield library

    shutil.rmtree(outdir, ignore_errors=True)


@pytest.fixture
def unitd(shim):
    """A unitd of the test's own, because conftest's unitd takes no
    environment."""
    temp = tempfile.mkdtemp(prefix='unit-test-wasm-')
    public_dir(temp)
    Path(f'{temp}/state').mkdir()

    env = os.environ.copy()
    env['LD_PRELOAD'] = shim

    with open(f'{temp}/stderr.log', 'w', encoding='utf-8') as stderr:
        process = subprocess.Popen(
            [
                f'{option.current_dir}/build/sbin/unitd',
                '--no-daemon',
                '--modulesdir',
                f'{option.current_dir}/build/lib/unit/modules',
                '--statedir',
                f'{temp}/state',
                '--pid',
                f'{temp}/unit.pid',
                '--log',
                f'{temp}/unit.log',
                '--control',
                f'unix:{temp}/control.unit.sock',
                '--tmpdir',
                temp,
            ],
            stderr=stderr,
            start_new_session=True,
            env=env,
        )

    yield temp

    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass

    process.wait(timeout=30)
    shutil.rmtree(temp, ignore_errors=True)


def build_guest(outdir):
    output = f'{outdir}/grow.wasm'

    subprocess.check_output(
        [
            shutil.which('clang'),
            '--target=wasm32-unknown-unknown',
            '-O2',
            '-nostdlib',
            f'-Wl,--no-entry,--initial-memory={INITIAL_MEMORY}',
            f'{option.test_dir}/wasm/grow/grow.c',
            '-o',
            output,
        ],
        stderr=subprocess.STDOUT,
    )

    return output


def alerts(log):
    return [
        line
        for line in log.read_text(errors='replace').splitlines()
        if '[alert]' in line
    ]


def test_wasm_memory_move(unitd):
    control = f'{unitd}/control.unit.sock'
    listener = f'{unitd}/app.sock'
    log = Path(f'{unitd}/unit.log')

    assert waitforfiles(control), Path(f'{unitd}/stderr.log').read_text()

    conf = {
        'listeners': {f'unix:{listener}': {'pass': 'applications/grow'}},
        'applications': {
            'grow': {
                'type': 'wasm',
                # One worker: "max" is 1 by default.  The shim counts the
                # moves in each process, so MOVES is right for one worker.
                'processes': {'spare': 0},
                'module': build_guest(unitd),
                'request_handler': 'request_handler',
                'malloc_handler': 'malloc_handler',
                'free_handler': 'free_handler',
                # A request_init or request_end hook would read the base
                # again, and hide a missing read after the request handler.
                'module_init_handler': 'module_init_handler',
            }
        },
    }

    resp = client.put(
        url='/config',
        sock_type='unix',
        addr=control,
        body=json.dumps(conf),
    )

    assert 'success' in resp['body']

    for i in range(1, REQUESTS + 1):
        resp = client.get(sock_type='unix', addr=listener)

        assert resp['status'] == 200, f'request {i}: {alerts(log)}'
        assert resp['body'] == 'fresh\n', f'request {i}: {alerts(log)}'

    # The host reads the base for the last time after response_end(), so
    # that line can come after the response.
    for _ in range(50):
        moves = len(re.findall(MOVED, log.read_text(errors='replace')))

        if moves >= MOVES:
            break

        time.sleep(0.1)

    text = log.read_text(errors='replace')

    shim_lines = '\n'.join(
        line for line in text.splitlines() if 'wasmtime_reservation' in line
    )

    assert re.search(ENGINE, text), 'no engine line from the shim in unit.log'
    assert moves == MOVES, f'{moves} moves, expected {MOVES}:\n{shim_lines}'
    assert not re.search(r'exited on signal', text)
