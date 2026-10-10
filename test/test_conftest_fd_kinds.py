import os

import pytest

from conftest import _fd_kind, _parse_socket_table, _socket_names

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
