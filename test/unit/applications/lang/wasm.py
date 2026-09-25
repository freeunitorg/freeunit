from pathlib import Path
import shutil
import subprocess
from urllib.parse import quote

from unit.applications.proto import ApplicationProto
from unit.option import option


class ApplicationWasm(ApplicationProto):
    @staticmethod
    def prepare_env(script):
        clang = shutil.which('clang')
        root = Path(option.current_dir)
        sysroot = root / 'pkg/contrib/wasi-sysroot'
        include = root / 'pkg/contrib/libunit-wasm/src/c/include'
        library = root / 'pkg/contrib/libunit-wasm/src/c/libunit-wasm.c'
        source = Path(option.test_dir) / 'wasm' / script / f'{script}.c'

        if (
            clang is None
            or not sysroot.is_dir()
            or not include.is_dir()
            or not library.is_file()
            or not source.is_file()
        ):
            return None

        temp_dir = Path(option.temp_dir) / 'wasm'
        temp_dir.mkdir(parents=True, exist_ok=True)

        output = temp_dir / f'{script}.wasm'

        # One -Wl, argument: clang splits it on commas and hands each piece to
        # the linker, so "-z stack-size=..." stays a linker option.
        link_flags = '-Wl,' + ','.join(
            [
                '--no-entry',
                '--export=__heap_base',
                '--export=__data_end',
                '--export=malloc',
                '--export=free',
                '--stack-first',
                '-z',
                'stack-size=8388608',
            ]
        )

        command = [
            clang,
            f'-I{include}',
            f'--sysroot={sysroot}',
            '--target=wasm32-wasi',
            '-O2',
            '-g',
            '-std=gnu11',
            '-fno-common',
            link_flags,
            '-mexec-model=reactor',
            '--rtlib=compiler-rt',
            str(source),
            str(library),
            '-o',
            str(output),
        ]

        try:
            subprocess.check_output(command, stderr=subprocess.STDOUT)
        except (subprocess.CalledProcessError, FileNotFoundError):
            return None

        return output

    def load(self, script, access_filesystem=None, **kwargs):
        module = self.prepare_env(script)

        app = {
            'type': 'wasm',
            'processes': {'spare': 0},
            'module': str(module),
            'request_handler': 'luw_request_handler',
            'malloc_handler': 'luw_malloc_handler',
            'free_handler': 'luw_free_handler',
            # Without these two the guest's init hook never runs, so
            # request_buf stays NULL and luw_set_req_buf() memcpy()s to
            # address 0 of the linear memory instead of the allocation.
            'module_init_handler': 'luw_module_init_handler',
            'module_end_handler': 'luw_module_end_handler',
        }

        if access_filesystem is not None:
            app['access'] = {'filesystem': access_filesystem}

        self._load_conf(
            {
                'listeners': {
                    '*:8080': {'pass': f'applications/{quote(script, "")}'}
                },
                'applications': {script: app},
            },
            **kwargs,
        )
