from pathlib import Path
import shutil
import subprocess
from urllib.parse import quote

from unit.applications.proto import ApplicationProto
from unit.option import option


class ApplicationWasmComponent(ApplicationProto):
    @staticmethod
    def prepare_env(script):
        try:
            subprocess.check_output(
                ['cargo', 'component', '--help'],
                stderr=subprocess.STDOUT,
            )
        except (subprocess.CalledProcessError, FileNotFoundError):
            return None

        temp_dir = Path(f'{option.temp_dir}/wasm_component/')

        if not temp_dir.exists():
            temp_dir.mkdir()

        app_path = f'{temp_dir}/{script}'

        shutil.copytree(f'{option.test_dir}/wasm_component/{script}', app_path)

        try:
            output = subprocess.check_output(
                ['cargo', 'component', 'build', '--release'],
                cwd=app_path,
                stderr=subprocess.STDOUT,
            )
        except KeyboardInterrupt:
            raise

        except subprocess.CalledProcessError:
            return None

        return output

    @staticmethod
    def prepare_env_wasip2(script, build_dir):
        """Build a guest with rustc's wasm32-wasip2 target, which links a
        component by itself.  The WASI 0.3 guests are built this way, as
        "cargo component" makes WASI 0.2 components.  `build_dir` must
        outlive the tests that use the component: the per-test temporary
        directory is removed after each test.  Returns the path of the
        component, or None if it cannot be built here."""

        app_path = f'{build_dir}/{script}'

        shutil.copytree(f'{option.test_dir}/wasm_component/{script}', app_path)

        try:
            subprocess.check_output(
                [
                    'cargo',
                    'build',
                    '--release',
                    '--locked',
                    '--target',
                    'wasm32-wasip2',
                ],
                cwd=app_path,
                stderr=subprocess.STDOUT,
            )
        except KeyboardInterrupt:
            raise

        except (subprocess.CalledProcessError, FileNotFoundError):
            return None

        return f'{app_path}/target/wasm32-wasip2/release/test_wasi_component.wasm'

    def load(
        self,
        script,
        execution_timeout=None,
        limits=None,
        access=None,
        component=None,
        **kwargs,
    ):
        if component is None:
            self.prepare_env(script)

            component = f'{option.temp_dir}/wasm_component/{script}/target/wasm32-wasip1/release/test_wasi_component.wasm'

        component_path = component

        app = {
            "type": "wasm-wasi-component",
            "processes": {"spare": 0},
            "component": component_path,
        }

        if execution_timeout is not None:
            app["execution_timeout"] = execution_timeout

        if limits is not None:
            app["limits"] = limits

        if access is not None:
            app["access"] = access

        self._load_conf(
            {
                "listeners": {
                    "*:8080": {"pass": f"applications/{quote(script, '')}"}
                },
                "applications": {script: app},
            },
            **kwargs,
        )
