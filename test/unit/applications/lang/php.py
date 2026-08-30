import os
from pathlib import Path
import shutil

import pytest

from unit.applications.proto import ApplicationProto
from unit.option import option


def _absolute_symlinks(path, names):
    # A shutil.copytree() filter.  Relative symlinks stay inside the copy,
    # but an absolute one -- /usr/share/zoneinfo/localtime -> /etc/localtime
    # on Debian -- points back at the host, and the conftest teardown chmods
    # every file it walks in the temp dir, following it out of the rootfs.
    return [
        name
        for name in names
        if os.path.islink(f'{path}/{name}')
        and os.path.isabs(os.readlink(f'{path}/{name}'))
    ]


class ApplicationPHP(ApplicationProto):
    def __init__(self, application_type='php'):
        self.application_type = application_type

    def load(self, script, index='index.php', **kwargs):
        script_path = f'{option.test_dir}/php/{script}'

        if kwargs.get('isolation') and kwargs['isolation'].get('rootfs'):
            rootfs = kwargs['isolation']['rootfs']

            Path(f'{rootfs}/app/php/').mkdir(parents=True, exist_ok=True)

            if not Path(f'{rootfs}/app/php/{script}').exists():
                shutil.copytree(script_path, f'{rootfs}/app/php/{script}')

            # PHP built with --with-system-tzdata -- every Debian, Ubuntu and
            # Fedora package -- opens the timezone database lazily, at request
            # time, which is after Unit has chroot()ed the app worker into
            # "rootfs".  Such builds segfault instead of raising an error when
            # the database is not there, so provision it and keep these tests
            # about Unit rather than about PHP packaging.
            tzdir = '/usr/share/zoneinfo'

            if not Path(tzdir).is_dir():
                pytest.skip(
                    f'no timezone database at {tzdir} to copy into the rootfs'
                )

            if not Path(f'{rootfs}{tzdir}').exists():
                shutil.copytree(
                    tzdir,
                    f'{rootfs}{tzdir}',
                    symlinks=True,
                    ignore=_absolute_symlinks,
                )

            script_path = f'/app/php/{script}'

        app = {
            "type": self.get_application_type(),
            "processes": kwargs.pop('processes', {"spare": 0}),
            "root": script_path,
            "working_directory": script_path,
            "index": index,
        }

        for attr in (
            'environment',
            'limits',
            'options',
            'targets',
        ):
            if attr in kwargs:
                app[attr] = kwargs.pop(attr)

        self._load_conf(
            {
                "listeners": {"*:8080": {"pass": f"applications/{script}"}},
                "applications": {script: app},
            },
            **kwargs,
        )
