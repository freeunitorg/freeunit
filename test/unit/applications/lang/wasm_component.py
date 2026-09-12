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

    def load(self, script, execution_timeout=None, limits=None, **kwargs):
        self.prepare_env(script)

        component_path = f'{option.temp_dir}/wasm_component/{script}/target/wasm32-wasip1/release/test_wasi_component.wasm'

        app = {
            "type": "wasm-wasi-component",
            "processes": {"spare": 0},
            "component": component_path,
        }

        if execution_timeout is not None:
            app["execution_timeout"] = execution_timeout

        if limits is not None:
            app["limits"] = limits

        self._load_conf(
            {
                "listeners": {
                    "*:8080": {"pass": f"applications/{quote(script, '')}"}
                },
                "applications": {script: app},
            },
            **kwargs,
        )
