"""A unitd of a test's own, for options the shared one does not run with."""

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
from unit.utils import public_dir

client = HTTP1()


class Unitd:
    """Starts unitd in its own directory with the options a test gives."""

    def __init__(self):
        self.unitd = f'{option.current_dir}/build/sbin/unitd'

        if not Path(self.unitd).is_file():
            pytest.skip('could not find unitd')

        self.dir = tempfile.mkdtemp(prefix='unit-test-unitd-')
        public_dir(self.dir)

        self.log = f'{self.dir}/unit.log'
        self.listener = f'{self.dir}/app.sock'
        self.control = f'{self.dir}/control.unit.sock'
        self.pid = f'{self.dir}/unit.pid'
        self.state = f'{self.dir}/state'
        self.stderr = f'{self.dir}/stderr.log'
        self.process = None

    def returning(self, status):
        """A configuration that answers every request with status."""
        return {
            "listeners": {f"unix:{self.listener}": {"pass": "routes"}},
            "routes": [{"action": {"return": status}}],
        }

    def start(self, *args, pid=None, control=None):
        """Starts unitd; pid and control default to files in self.dir."""
        env = os.environ.copy()

        # As in conftest.unit_run(): PYTHONHOME must not be set for pytest.
        pythonhome = env.pop('UNIT_PYTHONHOME', None)
        if pythonhome:
            env['PYTHONHOME'] = pythonhome

        builddir = f'{option.current_dir}/build'

        # What unitd reports before it opens its log goes to stderr.
        with open(self.stderr, 'w', encoding='utf-8') as stderr:
            self.process = subprocess.Popen(
                [
                    self.unitd,
                    '--no-daemon',
                    '--modulesdir',
                    f'{builddir}/lib/unit/modules',
                    '--statedir',
                    self.state,
                    '--pid',
                    pid or self.pid,
                    '--log',
                    self.log,
                    '--control',
                    control or f'unix:{self.control}',
                    '--tmpdir',
                    self.dir,
                    *args,
                ],
                stdout=subprocess.DEVNULL,
                stderr=stderr,
                start_new_session=True,
                env=env,
            )

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGQUIT)

            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=30)

        shutil.rmtree(self.dir, ignore_errors=True)

    def read_log(self):
        """unit.log and the stderr written before it existed."""
        text = ''

        for path in (self.stderr, self.log):
            if Path(path).exists():
                text += Path(path).read_text(encoding='utf-8', errors='replace')

        return text

    def get(self, url='/', addr=None):
        return client.get(
            url=url, sock_type='unix', addr=addr or self.listener
        )

    def default_paths(self):
        """The built-in control socket and pid file paths, from "--help"."""
        text = subprocess.run(
            [self.unitd, '--help'], capture_output=True, text=True, check=True
        ).stdout

        paths = {}

        for name in ('--control', '--pid'):
            found = re.search(
                rf'^  {name} .*?default: "([^"]+)"', text, re.M | re.S
            )
            assert found, f'{name} default in --help'
            paths[name] = Path(found.group(1).removeprefix('unix:'))

        return paths

    def wait_for(self, predicate, what):
        """Polls predicate for up to 10 s and returns its first true value."""
        for _ in range(100):
            value = predicate()
            if value:
                return value
            time.sleep(0.1)

        pytest.fail(f'{what}\n{self.read_log()}')
