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

Main runs each store in a short-lived child process, one at a time, and waits
for it before it exits (https://github.com/freeunitorg/freeunit/issues/516).
The other tests here check that a configuration answered with 200 is on disk
after an immediate SIGTERM, and that stores in quick succession leave the
last configuration.  They need --restart only.  They catch a regression only
when a store takes longer than the few milliseconds main needs to exit, so on
a tmpfs they pass either way.

The version tests seed a state directory by hand.  unitd writes the version
file with no line end, but a file written by hand often has one.
"""

import json
import os
import re
import shutil
import signal
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


def test_state_store_full_filesystem(requires_restart, skip_alert):
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

    # The two alerts of the failed store, and the alert of main about it.
    skip_alert(
        r'failed to store current configuration',
        r'write\(.*conf\.json\.tmp',
        r'state store child \d+ failed',
    )

    try:
        # main creates and writes everything here as root, so this only has
        # to be traversable.  0777 would be the very condition the store is
        # hardened against -- a state directory another user can plant a
        # symbolic link in.
        os.chmod(statedir, 0o755)

        unit_run(state_dir=str(statedir))

        # The configuration that must survive.
        assert 'success' in client.conf(SMALL_CONF), 'the small store'
        assert wait_for_stored(statedir, SMALL_CONF) is not None, 'stored'

        stored = (statedir / 'conf.json').read_bytes()

        assert 'success' in client.conf(big_conf(400)), 'the large PUT'

        # The store is attempted asynchronously; wait for it to give up.
        # The store child logs the first alert, and main logs the second
        # when it reaps the child.
        assert Log.wait_for_record(
            r'failed to store current configuration'
        ), 'the store child failed'
        assert Log.wait_for_record(
            r'state store child \d+ failed'
        ), 'main logged the failed store'

        # The store failed; the previously stored configuration is intact,
        # byte for byte, and still parses.
        after = (statedir / 'conf.json').read_bytes()

        assert after == stored, (
            'conf.json was damaged by a store that could not complete'
        )
        assert json.loads(after)['listeners'] == SMALL_CONF['listeners']

        # And nothing was left half-written next to it.
        assert [p.name for p in statedir.iterdir() if '.tmp' in p.name] == []

        # "version" goes through the same function and is stored first, so a
        # store that damaged it would leave the state directory unloadable
        # even with conf.json intact.
        version = statedir / 'version'

        assert version.is_file(), 'version survived the failed store'
        assert version.read_bytes().strip() != b'', 'version is not empty'

    finally:
        unit_stop()
        subprocess.run(
            ['umount', str(statedir)], check=False, capture_output=True
        )
        # A failed umount would make rmdir() raise out of the finally and
        # mask whichever assertion above actually failed.
        shutil.rmtree(statedir, ignore_errors=True)


def numbered_conf(i):
    """SMALL_CONF, made different per store by a setting."""

    return {
        "settings": {"http": {"header_read_timeout": 30 + i}},
        **SMALL_CONF,
    }


def stored_timeout(statedir):
    """The setting in conf.json, or None when there is no conf.json."""

    try:
        text = (statedir / 'conf.json').read_text(encoding='utf-8')
    except FileNotFoundError:
        return None

    return json.loads(text)['settings']['http']['header_read_timeout']


def group_alive(pgid):
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False

    return True


def put_and_terminate(statedir, count):
    """Start unitd on "statedir", PUT "count" configurations, and send
    SIGTERM to main as soon as the last one is answered.  Return the
    temporary directory of that unitd once main has exited."""

    unit = unit_run(state_dir=str(statedir))
    temp_dir = unit['temp_dir']

    for i in range(count):
        assert 'success' in client.conf(numbered_conf(i)), f'PUT {i}'

    os.kill(int(unit['pid']), signal.SIGTERM)

    assert unit['process'].wait(15) == 0, 'main exited with 0'

    # Main exits only after its store child: no process of its group is
    # left to finish the store.  In a container main is PID 1, and anything
    # left would be killed with it.
    assert not group_alive(unit['pgid']), 'no store child outlives main'

    log = Path(temp_dir, 'unit.log').read_text(encoding='utf-8')

    unit_stop()

    # The teardown checks only the log of the last unitd; check this one.
    Log.check_alerts(log=log)

    return temp_dir


def restart_and_get_timeout(statedir):
    unit_run(state_dir=str(statedir))

    return client.conf_get('settings/http/header_read_timeout')


def test_state_store_survives_exit(requires_restart):
    """A configuration answered with 200 is stored even when SIGTERM
    reaches main while the store runs."""

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))

    try:
        temp_dir = put_and_terminate(statedir, 1)

        stored = stored_timeout(statedir)
        loaded = restart_and_get_timeout(statedir)

        shutil.rmtree(temp_dir, ignore_errors=True)

        assert stored == 30, 'stored before exit'
        assert loaded == 30, 'loaded on restart'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


def test_state_store_pending_at_exit(requires_restart):
    """Stores that wait behind a running store child are not dropped at
    exit: the last configuration is on disk."""

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))

    try:
        temp_dir = put_and_terminate(statedir, 4)

        stored = stored_timeout(statedir)
        loaded = restart_and_get_timeout(statedir)

        shutil.rmtree(temp_dir, ignore_errors=True)

        assert stored == 33, 'the last store won'
        assert loaded == 33, 'loaded on restart'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


def test_state_store_serialised(requires_restart):
    """Stores in quick succession run one after another and leave the last
    configuration, with no temporary file and no alert."""

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))

    try:
        unit_run(state_dir=str(statedir))

        for i in range(6):
            assert 'success' in client.conf(numbered_conf(i)), f'PUT {i}'

        last = numbered_conf(5)

        for _ in range(100):
            try:
                if stored_timeout(statedir) == 35:
                    break
            except (ValueError, KeyError):
                pass

            time.sleep(0.1)

        # A --debug build logs the start of each store child.  A store
        # child must not start before the previous one exits.
        running = None

        events = re.findall(
            r'#\d+ state store child (\d+)$|process (\d+) exited',
            Log.read(),
            re.M,
        )

        for start, exited in events:
            if start:
                assert running is None, f'child {start} ran with {running}'
                running = start

            elif exited == running:
                running = None

        assert stored_timeout(statedir) == 35, 'the last store won'

        # A store that ran after the last one would overwrite it; give any
        # such store the time to finish before the second look.
        time.sleep(1)

        conf = json.loads((statedir / 'conf.json').read_text(encoding='utf-8'))

        assert conf == last, 'conf.json is the last configuration'
        assert [p.name for p in statedir.iterdir() if '.tmp' in p.name] == []

        # Two stores that run at the same time race on the temporary name,
        # and one of them logs a failed rename() or a failed store.
        alerts = re.findall(r'.+\[alert\].+', Log.read())

        assert alerts == [], 'no alert'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


VERSION_CONF = {
    "listeners": {"*:8080": {"pass": "routes"}},
    "routes": [{"action": {"return": 204}}],
}


def run_with_version(version):
    """Start unitd on a state directory with this version file."""

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))

    (statedir / 'conf.json').write_text(json.dumps(VERSION_CONF))
    (statedir / 'version').write_bytes(version)

    unit_run(state_dir=str(statedir))

    return statedir


@pytest.mark.parametrize(
    'version',
    [b'13700\n', b'13700\r\n', b'13700 \t\n'],
    ids=['lf', 'crlf', 'ws'],
)
def test_state_store_version_line_end(requires_restart, version):
    """A line end after the number does not drop the stored configuration."""

    statedir = run_with_version(version)

    try:
        assert (
            client.conf_get('listeners') == VERSION_CONF['listeners']
        ), 'stored configuration loaded'
        assert client.get()['status'] == 204, 'stored configuration runs'
        assert not Log.findall(r'invalid version string'), 'no alert'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


@pytest.mark.parametrize(
    'version',
    [b'13700x\n', b'', b'\n', b' 13700', b'137\n00'],
    ids=['suffix', 'empty', 'blank', 'leading', 'inner'],
)
def test_state_store_version_invalid(requires_restart, skip_alert, version):
    """Other content is still an error, and nothing is restored."""

    skip_alert(r'failed to restore previous configuration')

    statedir = run_with_version(version)

    try:
        assert Log.wait_for_record(
            r'failed to restore previous configuration: '
            r'invalid version string'
        ), 'alert'
        assert client.conf_get('listeners') == {}, 'nothing restored'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)
