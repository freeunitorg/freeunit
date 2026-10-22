import os
import re
from collections import Counter

import pytest

import conftest
from conftest import (
    _fd_kind,
    _fd_kinds,
    _fd_targets,
    _parse_socket_table,
    _socket_names,
)
from unit.log import Log

HEADER = 'Num RefCount Protocol Flags Type St Inode Path\n'


def unix_line(inode, path):
    return f'0000000000000000: 00000002 00000000 00010000 0001 01 {inode} {path}'


def test_fd_kinds_unix_path_with_spaces():
    text = HEADER + unix_line(111, '/tmp/a dir/with  two spaces.sock') + '\n'

    assert _parse_socket_table('unix', text) == {
        '111': 'unix:/tmp/a dir/with  two spaces.sock'
    }


def test_fd_kinds_unix_autobind_and_unnamed():
    text = HEADER + unix_line(1, '@0a1b2') + '\n' + unix_line(2, '') + '\n'

    assert _parse_socket_table('unix', text) == {
        '1': 'unix:@autobind',
        '2': 'unix',
    }


def test_fd_kinds_unix_autobind_five_hex_only():
    # The kernel names an autobind socket "@%05x" of a 20-bit counter, so
    # the name has exactly five hex digits.  A longer abstract name is the
    # application's own and keeps its name.
    text = (
        HEADER
        + unix_line(3, '@fffff')
        + '\n'
        + unix_line(4, '@0a1b2c3d')
        + '\n'
        + unix_line(5, '@0a1b')
        + '\n'
    )

    assert _parse_socket_table('unix', text) == {
        '3': 'unix:@autobind',
        '4': 'unix:@0a1b2c3d',
        '5': 'unix:@0a1b',
    }


def test_fd_kinds_non_utf8_bytes(tmp_path):
    raw = (HEADER + unix_line(5, '/tmp/x\udcff')).encode(
        'utf-8', 'surrogateescape'
    )
    assert b'\xff' in raw

    # same decode as _socket_names() applies to the file bytes
    text = raw.decode('utf-8', 'backslashreplace')

    assert _parse_socket_table('unix', text) == {'5': 'unix:/tmp/x\\xff'}


def test_fd_kinds_socket_names_reads_non_utf8_table(monkeypatch):
    class Fake:
        def __init__(self, path):
            self.path = str(path)

        def read_bytes(self):
            if self.path.endswith('/net/unix'):
                return (HEADER + unix_line(9, '/tmp/y')).encode() + b'\xff\n'
            raise FileNotFoundError(self.path)

    monkeypatch.setattr('conftest.Path', Fake)

    assert _socket_names(os.getpid()) == {'9': 'unix:/tmp/y\\xff'}


def test_fd_kinds_tcp_time_wait():
    text = (
        'sl local_address rem_address st tx_queue rx_queue tr tm->when '
        'retrnsmt uid timeout inode\n'
        '0: 0100007F:1F90 0100007F:D2A4 06 00000000:00000000 '
        '03:000001AC 00000000 0 0 4242 1 0000000000000000\n'
    )

    assert _parse_socket_table('tcp', text) == {'4242': 'tcp time-wait'}


def test_fd_kinds_tcp_new_syn_recv():
    text = (
        'sl local_address rem_address st tx_queue rx_queue tr tm->when '
        'retrnsmt uid timeout inode\n'
        '0: 0100007F:1F90 0100007F:D2A4 0C 00000000:00000000 '
        '03:000001AC 00000000 0 0 4343 1 0000000000000000\n'
    )

    assert _parse_socket_table('tcp', text) == {'4343': 'tcp new-syn-recv'}


@pytest.mark.skipif(
    not os.path.isdir('/proc/self/fd'), reason='needs /proc/self/fd'
)
def test_fd_kinds_count_and_kinds_from_one_listing():
    count, targets = _fd_targets(os.getpid())
    r, w = os.pipe()

    try:
        count2, targets2 = _fd_targets(os.getpid())
    finally:
        os.close(r)
        os.close(w)

    # A listing of /proc/self/fd counts its own directory descriptor, which
    # is closed before readlink().  So compare the changes, not the totals.
    before = _fd_kinds(os.getpid(), None, targets)
    after = _fd_kinds(os.getpid(), None, targets2)

    assert count2 - count == 2
    assert len(targets2) - len(targets) == 2
    assert after['pipe'] - before['pipe'] == 2


@pytest.mark.skipif(
    not os.path.isdir('/proc/self/fd'), reason='needs /proc/self/fd'
)
def test_fd_kinds_leak_message_count_from_the_listing(monkeypatch):
    # _check_fds() must take the "+N fds" of a leak message from the same
    # listing as the kinds.  _count_fds() here disagrees with every
    # listing, so a count from it shows in the message.
    samples = []

    def spy(pid):
        sample = _fd_targets(pid)
        samples.append(sample)
        return sample

    pid = os.getpid()
    base = _fd_targets(pid)[0] - 5

    monkeypatch.setattr(conftest, '_fd_targets', spy)
    monkeypatch.setattr(conftest, '_count_fds', lambda pid: 1000000)
    monkeypatch.setattr(conftest, 'pid_by_name', lambda name: pid)
    monkeypatch.setattr(conftest.time, 'sleep', lambda seconds: None)
    monkeypatch.setattr(conftest.option, 'fds_threshold', 0, raising=False)
    monkeypatch.setattr(Log, 'print_log', staticmethod(lambda log=None: None))
    monkeypatch.setitem(conftest.unit_instance, 'pid', pid)
    monkeypatch.setitem(
        conftest._fds_info,
        'main',
        {'fds': base, 'kinds': Counter(), 'skip': False},
    )

    for name in ['controller', 'router']:
        monkeypatch.setitem(
            conftest._fds_info,
            name,
            {'name': name, 'pid': pid, 'fds': 0, 'kinds': {}, 'skip': True},
        )

    with pytest.raises(AssertionError) as e:
        conftest._check_fds(log='')

    m = re.search(r'descriptors leak main process: \+(\d+) fds', str(e.value))

    assert m, str(e.value)
    assert int(m.group(1)) == samples[-1][0] - base


@pytest.mark.skipif(
    not os.path.isdir('/proc/self/fd'), reason='needs /proc/self/fd'
)
def test_fd_kinds_non_utf8_file_name(tmp_path):
    name = os.fsdecode(b'leak-\xff.bin')
    with open(tmp_path / name, 'w') as f:
        target = os.readlink(f'/proc/self/fd/{f.fileno()}')

    assert '\udcff' in target

    kind = _fd_kind(target, {})

    assert kind.endswith('leak-\\xff.bin')
    kind.encode('utf-8')
