from pathlib import Path
import shutil
import subprocess

import pytest

from unit.applications.lang.wasm import ApplicationWasm
from unit.check.check_prerequisites import check_prerequisites
from unit.option import option

prerequisites = {'modules': {'wasm': 'any'}, 'features': {'clang_wasm': True}}

check_prerequisites(prerequisites)

client = ApplicationWasm()

# The request buffer starts at 1 MiB.  The host asks for 32 MiB + 64 KiB
# from there, so the memory needs at least 33 MiB + 64 KiB.
INITIAL_MEMORY = 36 * 1024 * 1024


def build_noreq_guest(call, initialize=False):
    source = Path(option.test_dir) / 'wasm' / 'noreq' / 'noreq.c'
    suffix = '-initialize' if initialize else ''
    output = Path(option.temp_dir) / f'noreq-{call}{suffix}.wasm'

    defines = [f'-DCALL_{call.upper()}']

    if initialize:
        defines.append('-DCALL_IN_INITIALIZE')

    subprocess.check_output(
        [
            shutil.which('clang'),
            '--target=wasm32-unknown-unknown',
            '-O2',
            '-nostdlib',
            *defines,
            '-Wl,--no-entry,--stack-first,-z,stack-size=65536,'
            f'--initial-memory={INITIAL_MEMORY}',
            str(source),
            '-o',
            str(output),
        ],
        stderr=subprocess.STDOUT,
    )

    return output


def load_noreq(module, **handlers):
    app = {
        'type': 'wasm',
        'processes': {'spare': 0},
        'module': str(module),
        'request_handler': 'request_handler',
        'malloc_handler': 'malloc_handler',
        'free_handler': 'free_handler',
    }
    app.update(handlers)

    assert 'success' in client.conf(
        {
            'listeners': {'*:8080': {'pass': 'applications/noreq'}},
            'applications': {'noreq': app},
        }
    )


@pytest.mark.parametrize(
    'call', ['send_response', 'send_headers', 'response_end']
)
def test_wasm_import_in_initialize(call, wait_for_record):
    load_noreq(build_noreq_guest(call, initialize=True))

    assert client.get()['status'] == 503

    record = rf'nxt_wasm_{call}\(\) called outside a request'
    assert wait_for_record(record) is not None


def test_wasm_import_in_malloc_handler(wait_for_record):
    load_noreq(
        build_noreq_guest('send_headers'), malloc_handler='malloc_import'
    )

    assert client.get()['status'] == 503

    record = r'nxt_wasm_send_headers\(\) called outside a request'
    assert wait_for_record(record) is not None


def test_wasm_import_in_module_init(wait_for_record):
    load_noreq(
        build_noreq_guest('response_end'), module_init_handler='init_import'
    )

    resp = client.get()

    assert resp['status'] == 200
    assert resp['body'] == 'ok\n'

    record = r'failed to call hook function \[init_import\]'
    assert wait_for_record(record) is not None

    record = r'nxt_wasm_response_end\(\) called outside a request'
    assert wait_for_record(record) is not None
