"""Persistence of the state directory (issue #215).

The controller hands each accepted configuration to the main process, which
writes it to <statedir>/conf.json.  That write used to go straight into
conf.json with O_TRUNC, so a store that could not finish left the file
truncated -- and the short-write path unlinked it outright, which is what
this test observes on the unfixed code: a configuration too large for the
filesystem leaves no conf.json at all.

The failure is arranged by putting the state directory on a 64 KiB tmpfs,
which needs root and CAP_SYS_ADMIN (pytest as root in a --privileged
container) as well as --restart; the test skips otherwise.
"""

import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

from conftest import unit_run, unit_stop
from unit.applications.proto import ApplicationProto
from unit.log import Log

client = ApplicationProto()


SMALL_CONF = {
    "listeners": {"*:8080": {"pass": "routes"}},
    "routes": [{"action": {"return": 200}}],
}


def big_conf(routes):
    """Far more than the 64 KiB the filesystem below has."""

    return {
        "listeners": {"*:8080": {"pass": "routes"}},
        "routes": [
            {"match": {"uri": f"/{'p' * 200}{i}"}, "action": {"return": 200}}
            for i in range(routes)
        ],
    }


def wait_for_stored(statedir, expected, wait=100):
    """The store is asynchronous: the controller answers the PUT and only
    then asks the main process to persist it."""

    conf_json = statedir / 'conf.json'

    for _ in range(wait):
        try:
            conf = json.loads(conf_json.read_text(encoding='utf-8'))
            if conf.get('listeners') == expected['listeners']:
                return conf
        except (OSError, ValueError):
            pass

        time.sleep(0.1)

    return None


def test_state_store_full_filesystem(skip_alert):
    """A store that runs out of space must not damage the stored config."""

    if os.geteuid() != 0:
        pytest.skip('requires root to mount a tmpfs')

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))

    mounted = subprocess.run(
        ['mount', '-t', 'tmpfs', '-o', 'size=64k', 'none', str(statedir)],
        check=False,
        capture_output=True,
    )

    if mounted.returncode != 0:
        pytest.skip(f'could not mount a tmpfs: {mounted.stderr.decode()}')

    # The two alerts the failed store is expected to log.
    skip_alert(
        r'failed to store current configuration',
        r'write\(.*conf\.json\.tmp',
    )

    try:
        os.chmod(statedir, 0o777)

        unit_run(state_dir=str(statedir))

        # The configuration that must survive.
        assert 'success' in client.conf(SMALL_CONF), 'the small store'
        assert wait_for_stored(statedir, SMALL_CONF) is not None, 'stored'

        stored = (statedir / 'conf.json').read_bytes()

        assert 'success' in client.conf(big_conf(400)), 'the large PUT'

        # The store is attempted asynchronously; wait for it to give up.
        Log.wait_for_record(r'failed to store current configuration')

        # The store failed; the previously stored configuration is intact,
        # byte for byte, and still parses.
        after = (statedir / 'conf.json').read_bytes()

        assert after == stored, (
            'conf.json was damaged by a store that could not complete'
        )
        assert json.loads(after)['listeners'] == SMALL_CONF['listeners']

        # And nothing was left half-written next to it.
        assert [p.name for p in statedir.iterdir() if '.tmp' in p.name] == []

    finally:
        unit_stop()
        subprocess.run(
            ['umount', str(statedir)], check=False, capture_output=True
        )
        statedir.rmdir()
