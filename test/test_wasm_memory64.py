from pathlib import Path
import shutil
import subprocess

from unit.applications.lang.wasm import ApplicationWasm
from unit.check.check_prerequisites import check_prerequisites
from unit.option import option

prerequisites = {'modules': {'wasm': 'any'}, 'features': {'clang_wasm': True}}

check_prerequisites(prerequisites)

client = ApplicationWasm()

# 1 MiB below the request buffer, then the 32 MiB + 64 KiB that the host
# asks for, rounded up to 36 MiB.
INITIAL_MEMORY = 36 * 1024 * 1024


def build_memory64_guest():
    source = Path(option.test_dir) / 'wasm' / 'memory64' / 'memory64.c'
    output = Path(option.temp_dir) / 'memory64.wasm'

    subprocess.check_output(
        [
            shutil.which('clang'),
            '--target=wasm64-unknown-unknown',
            '-O2',
            '-nostdlib',
            f'-Wl,--no-entry,--initial-memory={INITIAL_MEMORY}',
            str(source),
            '-o',
            str(output),
        ],
        stderr=subprocess.STDOUT,
    )

    return output


def test_wasm_memory64_refused(wait_for_record):
    assert 'success' in client.conf(
        {
            'listeners': {'*:8080': {'pass': 'applications/memory64'}},
            'applications': {
                'memory64': {
                    'type': 'wasm',
                    'processes': {'spare': 0},
                    'module': str(build_memory64_guest()),
                    'request_handler': 'request_handler',
                    'malloc_handler': 'malloc_handler',
                    'free_handler': 'free_handler',
                }
            },
        }
    )

    assert client.get()['status'] == 503

    assert wait_for_record(r'module memory is 64-bit') is not None
