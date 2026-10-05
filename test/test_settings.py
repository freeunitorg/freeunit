import json
import re
import shutil
import socket
import ssl
import subprocess
import tempfile
import time
from pathlib import Path

import pytest
from conftest import unit_run, unit_stop
from unit.applications.lang.python import ApplicationPython
from unit.applications.tls import ApplicationTLS
from unit.option import option
from unit import port as port_map
from unit.status import Status

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()


def build_define(header, name):
    """A number that ./configure wrote for the unitd under test.  The
    interpreter that runs the tests can have another width."""

    path = Path(option.current_dir) / 'build/include' / header
    found = re.search(fr'^#define {name}\s+(\d+)$', path.read_text(), re.M)
    assert found is not None, f'{name} in {header}'

    return int(found.group(1))


def sysctl():
    try:
        out = subprocess.check_output(
            ['sysctl', '-a'], stderr=subprocess.STDOUT
        ).decode()
    except FileNotFoundError:
        pytest.skip('requires sysctl')

    return out


def test_settings_large_header_buffer_size():
    client.load('empty')

    def set_buffer_size(size):
        assert 'success' in client.conf(
            {'http': {'large_header_buffer_size': size}},
            'settings',
        )

    def header_value(size, expect=200):
        headers = {'Host': 'a' * (size - 1), 'Connection': 'close'}
        assert client.get(headers=headers)['status'] == expect

    set_buffer_size(4096)
    header_value(4096)
    header_value(4097, 431)

    set_buffer_size(16384)
    header_value(16384)
    header_value(16385, 431)


def test_settings_large_header_buffers():
    client.load('empty')

    def set_buffers(buffers):
        assert 'success' in client.conf(
            {'http': {'large_header_buffers': buffers}},
            'settings',
        )

    def big_headers(headers_num, expect=200):
        headers = {'Host': 'localhost', 'Connection': 'close'}

        for i in range(headers_num):
            headers[f'Custom-header-{i}'] = 'a' * 8000

        assert client.get(headers=headers)['status'] == expect

    set_buffers(1)
    big_headers(1)
    big_headers(2, 431)

    set_buffers(2)
    big_headers(2)
    big_headers(3, 431)

    set_buffers(8)
    big_headers(8)
    big_headers(9, 431)


def test_settings_size_validation():
    client.load('empty')

    # The router maps each size to ssize_t (NXT_CONF_SIZE_BOUND).
    size_t_size = build_define('nxt_auto_config.h', 'NXT_SIZE_T_SIZE')
    bound = 2 ** (size_t_size * 8 - 1)

    if bound == 2**31:
        largest = bound - 1
    else:
        # The JSON parser refuses 2^63 itself: it is too long.
        largest = 2**40

    def http(name, value):
        return {'http': {name: value}}

    def compression(_, value):
        compressor = {'encoding': 'identity', 'min_length': value}

        return {
            'http': {
                'compression': {
                    'types': ['text/plain'],
                    'compressors': compressor,
                }
            }
        }

    members = [
        ('large_header_buffer_size', 'settings', http),
        ('large_header_buffers', 'settings', http),
        ('body_buffer_size', 'settings', http),
        ('max_body_size', 'settings', http),
        (
            'max_frame_size',
            'settings',
            lambda n, v: {'http': {'websocket': {n: v}}},
        ),
        ('min_length', 'settings', compression),
        ('shm', 'applications/empty/limits', lambda n, v: {n: v}),
    ]

    try:
        for name, url, conf in members:
            resp = client.conf(conf(name, -1), url)
            assert 'error' in resp, f'{name} -1'
            assert 'must not be negative' in resp['detail'], name

            resp = client.conf(conf(name, bound), url)
            assert 'error' in resp, f'{name} {bound}'

            if bound == 2**31:
                assert f'must be less than {bound}.' in resp['detail'], name

            assert 'error' in client.conf(conf(name, 1.5), url), name

            # The application restarts with each "shm" limit: keep it usable.
            if name == 'shm':
                accepted = (100 * 1024 * 1024,)
            else:
                accepted = (0, largest)

            for value in accepted:
                assert 'success' in client.conf(
                    conf(name, value), url
                ), f'{name} {value}'

    finally:
        # The default no-restart suite preserves /settings between tests.
        client.conf_delete('settings/http')


STORED_CONF = {
    'listeners': {'*:8080': {'pass': 'routes'}},
    'routes': [{'action': {'return': 204}}],
    'settings': {'http': {'max_body_size': -1}},
}


def run_with_stored_conf(version):
    """Start unitd on a state directory that an earlier version wrote."""

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))

    (statedir / 'conf.json').write_text(json.dumps(STORED_CONF))

    if version is not None:
        (statedir / 'version').write_text(str(version))

    unit_run(state_dir=str(statedir))

    return statedir


@pytest.mark.parametrize('version', [13700, None])
def test_settings_size_negative_stored(
    requires_restart, version, wait_for_record
):
    """Earlier versions accepted a negative size.  unitd still loads a
    stored configuration with one, whatever its version file says, and the
    control API refuses a new one."""

    statedir = run_with_stored_conf(version)

    try:
        assert (
            client.conf_get('settings/http/max_body_size') == -1
        ), 'stored configuration loaded'
        assert client.get()['status'] == 204, 'stored configuration runs'

        assert wait_for_record(
            r'\[warn\].+the restored configuration has a negative '
            r'"max_body_size" at "/settings/http/max_body_size"'
        ), 'warning'

        # The control API validates the whole configuration.
        resp = client.conf([{'action': {'return': 200}}], 'routes')
        assert 'error' in resp, 'update refused'
        assert 'must not be negative' in resp['detail']
        assert resp['location']['path'] == '/settings/http/max_body_size'
        assert client.get()['status'] == 204, 'configuration kept'

        assert 'success' in client.conf(
            '1048576', 'settings/http/max_body_size'
        ), 'corrected'
        assert 'success' in client.conf(
            [{'action': {'return': 200}}], 'routes'
        ), 'update after the correction'
        assert client.get()['status'] == 200

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


def test_settings_server_version():
    client.load('empty')

    assert client.get()['headers']['Server'].startswith('Unit/')

    assert 'success' in client.conf(
        {"http": {"server_version": False}}, 'settings'
    ), 'remove version'
    assert client.get()['headers']['Server'] == 'Unit'

    assert 'success' in client.conf(
        {"http": {"server_version": True}}, 'settings'
    ), 'add version'
    assert client.get()['headers']['Server'].startswith('Unit/')


def test_settings_header_read_timeout():
    client.load('empty')

    def req():
        (_, sock) = client.http(
            b"""GET / HTTP/1.1
""",
            start=True,
            read_timeout=1,
            raw=True,
        )

        time.sleep(3)

        return client.http(
            b"""Host: localhost
Connection: close

""",
            sock=sock,
            raw=True,
        )

    assert 'success' in client.conf(
        {'http': {'header_read_timeout': 2}}, 'settings'
    )
    assert req()['status'] == 408, 'status header read timeout'

    assert 'success' in client.conf(
        {'http': {'header_read_timeout': 7}}, 'settings'
    )
    assert req()['status'] == 200, 'status header read timeout 2'


def test_settings_header_read_timeout_update():
    client.load('empty')

    assert 'success' in client.conf(
        {'http': {'header_read_timeout': 4}}, 'settings'
    )

    sock = client.http(
        b"""GET / HTTP/1.1
""",
        raw=True,
        no_recv=True,
    )

    time.sleep(2)

    sock = client.http(
        b"""Host: localhost
""",
        sock=sock,
        raw=True,
        no_recv=True,
    )

    time.sleep(2)

    (resp, sock) = client.http(
        b"""X-Blah: blah
""",
        start=True,
        sock=sock,
        read_timeout=1,
        raw=True,
    )

    if len(resp) != 0:
        sock.close()

    else:
        time.sleep(2)

        resp = client.http(
            b"""Connection: close

""",
            sock=sock,
            raw=True,
        )

    assert resp['status'] == 408, 'status header read timeout update'


def test_settings_body_read_timeout():
    client.load('empty')

    def req():
        (_, sock) = client.http(
            b"""POST / HTTP/1.1
Host: localhost
Content-Length: 10
Connection: close

""",
            start=True,
            raw_resp=True,
            read_timeout=1,
            raw=True,
        )

        time.sleep(3)

        return client.http(b"""0123456789""", sock=sock, raw=True)

    assert 'success' in client.conf(
        {'http': {'body_read_timeout': 2}}, 'settings'
    )
    assert req()['status'] == 408, 'status body read timeout'

    assert 'success' in client.conf(
        {'http': {'body_read_timeout': 7}}, 'settings'
    )
    assert req()['status'] == 200, 'status body read timeout 2'


def test_settings_body_read_timeout_update():
    client.load('empty')

    assert 'success' in client.conf(
        {'http': {'body_read_timeout': 4}}, 'settings'
    )

    (resp, sock) = client.http(
        b"""POST / HTTP/1.1
Host: localhost
Content-Length: 10
Connection: close

""",
        start=True,
        read_timeout=1,
        raw=True,
    )

    time.sleep(2)

    (resp, sock) = client.http(
        b"""012""", start=True, sock=sock, read_timeout=1, raw=True
    )

    time.sleep(2)

    (resp, sock) = client.http(
        b"""345""", start=True, sock=sock, read_timeout=1, raw=True
    )

    time.sleep(2)

    resp = client.http(b"""6789""", sock=sock, raw=True)

    assert resp['status'] == 200, 'status body read timeout update'


def test_settings_send_timeout(temp_dir):
    client.load('body_generate')

    def req(addr, data_len):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(addr)

        req = f"""GET / HTTP/1.1
Host: localhost
X-Length: {data_len}
Connection: close

"""

        sock.sendall(req.encode())

        data = sock.recv(16).decode()

        time.sleep(3)

        data += client.recvall(sock).decode()

        sock.close()

        return data

    sysctl_out = sysctl()
    values = re.findall(r'net.core.[rw]mem_(?:max|default).*?(\d+)', sysctl_out)
    values = [int(v) for v in values]

    data_len = 1048576 if len(values) == 0 else 10 * max(values)

    addr = f'{temp_dir}/sock'

    assert 'success' in client.conf(
        {f'unix:{addr}': {'application': 'body_generate'}}, 'listeners'
    )

    assert 'success' in client.conf({'http': {'send_timeout': 1}}, 'settings')

    data = req(addr, data_len)
    assert re.search(r'200 OK', data), 'send timeout status'
    assert len(data) < data_len, 'send timeout data '

    client.conf({'http': {'send_timeout': 7}}, 'settings')

    data = req(addr, data_len)
    assert re.search(r'200 OK', data), 'send timeout status  2'
    assert len(data) > data_len, 'send timeout data 2'


def test_settings_send_timeout_recovery(temp_dir):
    # test_settings_send_timeout checks the framing of a single aborted
    # response.  Nothing asserted that the router is still usable afterwards,
    # so a request that fails to complete on the abort path -- e.g. its last
    # buffer completed twice, or never -- would surface only indirectly.  Abort
    # several streamed responses in a row over a unix listener, then serve a
    # normal request on the same listener.
    client.load('body_generate')

    def req(addr, data_len):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)

        # A router that stops serving after an abort is exactly what this test
        # looks for; without a timeout that failure would hang the run instead
        # of failing it.
        sock.settimeout(30)
        sock.connect(addr)

        req = f"""GET / HTTP/1.1
Host: localhost
X-Length: {data_len}
Connection: close

"""

        sock.sendall(req.encode())

        data = sock.recv(16).decode()

        time.sleep(3)

        data += client.recvall(sock).decode()

        sock.close()

        return data

    sysctl_out = sysctl()
    values = re.findall(r'net.core.[rw]mem_(?:max|default).*?(\d+)', sysctl_out)
    values = [int(v) for v in values]

    data_len = 1048576 if len(values) == 0 else 10 * max(values)

    addr = f'{temp_dir}/sock'

    assert 'success' in client.conf(
        {f'unix:{addr}': {'application': 'body_generate'}}, 'listeners'
    )

    assert 'success' in client.conf({'http': {'send_timeout': 1}}, 'settings')

    try:
        for i in range(3):
            data = req(addr, data_len)
            assert re.search(r'200 OK', data), f'aborted status {i}'
            assert len(data) < data_len, f'aborted data {i}'

        data = req(addr, 10)
        assert re.search(r'200 OK', data), 'recovery status'
        assert re.search(r'XXXXXXXXXX$', data), 'recovery data'

    finally:
        # The default no-restart suite preserves /settings between tests.
        client.conf_delete('settings/http/send_timeout')


def min_rate_conf(conf):
    assert 'success' in client.conf({'http': conf}, 'settings')


def min_rate_reset():
    # The default no-restart suite preserves /settings between tests.
    for name in (
        'body_read_timeout',
        'body_min_rate',
        'send_timeout',
        'send_min_rate',
        'chunked_transform',
    ):
        client.conf_delete(f'settings/http/{name}')


BODY_RATE_RECORD = r'client body rate is less than body_min_rate'


def min_rate_slow_body(sock, part, limit):
    # Sends "part" about once each second for at most "limit" seconds, and
    # reads what the router sends back.  Returns (closed, data, elapsed).
    sock.settimeout(1)

    start = time.monotonic()
    data = b''
    closed = False

    while time.monotonic() - start < limit:
        try:
            sock.sendall(part)
        except OSError:
            closed = True
            break

        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            closed = True
            break

        if not chunk:
            closed = True
            break

        data += chunk

    return closed, data, time.monotonic() - start


@pytest.mark.parametrize('timeout', [2, 0])
def test_settings_body_min_rate_slow_body(timeout, wait_for_record):
    # A client sends the body 1 byte each second.  Each byte starts the
    # body_read_timeout gap timer again, so only the rate floor can stop
    # this client.  The floor check starts after the grace time (equal to
    # body_read_timeout, 2 s).  The first body read after the grace time
    # must give 408.  With body_read_timeout 0 there is no gap timer and
    # no grace time, and each read is checked against the time since the
    # previous read.  The first slow read must give 408.
    client.load('empty')

    min_rate_conf({'body_read_timeout': timeout, 'body_min_rate': 256})

    try:
        sock = socket.create_connection(('127.0.0.1', port_map.port(8080)))
        sock.settimeout(1)
        sock.sendall(
            b'POST / HTTP/1.1\r\n'
            b'Host: localhost\r\n'
            b'Content-Length: 10000\r\n'
            b'Connection: close\r\n\r\n'
        )

        closed, data, elapsed = min_rate_slow_body(sock, b'x', 12)
        sock.close()

        # The router closes the connection after the 408 response.  The
        # client can get a reset before it reads the response, because
        # the router does not read the rest of the body.
        assert closed or data, 'slow body stopped'
        assert data == b'' or data.startswith(b'HTTP/1.1 408'), 'slow body 408'
        assert elapsed < 8, 'slow body stopped in time'
        assert wait_for_record(BODY_RATE_RECORD) is not None, 'slow body log'

    finally:
        min_rate_reset()


def test_settings_body_min_rate_chunked(wait_for_record):
    # The same slow client as in the slow body test, with a chunked body.
    # The client sends a chunk of 1 byte each second.  The chunked body is
    # read in the same function as a body with Content-Length, but the
    # chunk parser runs there first.  The floor counts the chunk framing
    # too: 6 bytes each second, which is much less than 256.
    client.load('empty')

    min_rate_conf(
        {
            'body_read_timeout': 2,
            'body_min_rate': 256,
            'chunked_transform': True,
        }
    )

    try:
        sock = socket.create_connection(('127.0.0.1', port_map.port(8080)))
        sock.sendall(
            b'POST / HTTP/1.1\r\n'
            b'Host: localhost\r\n'
            b'Transfer-Encoding: chunked\r\n'
            b'Connection: close\r\n\r\n'
        )

        closed, data, elapsed = min_rate_slow_body(sock, b'1\r\nx\r\n', 12)
        sock.close()

        assert closed or data, 'chunked body stopped'
        assert data == b'' or data.startswith(
            b'HTTP/1.1 408'
        ), 'chunked body 408'
        assert elapsed < 8, 'chunked body stopped in time'
        assert wait_for_record(BODY_RATE_RECORD) is not None, 'chunked log'

    finally:
        min_rate_reset()


def test_settings_body_min_rate_tls(wait_for_record):
    # The same slow client as in the slow body test, on a TLS listener.
    # The floor counts the bytes that the TLS layer gives to the router,
    # so 1 byte each second is still below 256.
    if not option.available['modules'].get('openssl'):
        pytest.skip('requires openssl')

    client.load('empty')

    ApplicationTLS().certificate()

    assert 'success' in client.conf(
        {
            "pass": "applications/empty",
            "tls": {"certificate": "default"},
        },
        'listeners/*:8080',
    )

    min_rate_conf({'body_read_timeout': 2, 'body_min_rate': 256})

    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE

    try:
        sock = context.wrap_socket(
            socket.create_connection(('127.0.0.1', port_map.port(8080)))
        )
        sock.sendall(
            b'POST / HTTP/1.1\r\n'
            b'Host: localhost\r\n'
            b'Content-Length: 10000\r\n'
            b'Connection: close\r\n\r\n'
        )

        closed, data, elapsed = min_rate_slow_body(sock, b'x', 12)
        sock.close()

        assert closed or data, 'TLS slow body stopped'
        assert data == b'' or data.startswith(
            b'HTTP/1.1 408'
        ), 'TLS slow body 408'
        assert elapsed < 8, 'TLS slow body stopped in time'
        assert wait_for_record(BODY_RATE_RECORD) is not None, 'TLS log'

    finally:
        min_rate_reset()


def test_settings_body_min_rate_burst_then_slow(wait_for_record):
    # A client sends half of the body at once, then 1 byte each second.
    # With one average over the whole body, the burst would give credit
    # for about 195 s at 256 bytes per second.  The rate is checked for
    # each window of at least body_read_timeout, so the first slow window
    # must give 408.
    client.load('empty')

    min_rate_conf({'body_read_timeout': 2, 'body_min_rate': 256})

    try:
        sock = socket.create_connection(('127.0.0.1', port_map.port(8080)))
        sock.settimeout(1)
        sock.sendall(
            b'POST / HTTP/1.1\r\n'
            b'Host: localhost\r\n'
            b'Content-Length: 100000\r\n'
            b'Connection: close\r\n\r\n'
        )
        sock.sendall(b'x' * 50000)

        closed, data, elapsed = min_rate_slow_body(sock, b'x', 20)
        sock.close()

        assert closed or data, 'burst then slow body stopped'
        assert data == b'' or data.startswith(
            b'HTTP/1.1 408'
        ), 'burst then slow body 408'
        assert elapsed < 12, 'burst then slow body stopped in time'
        assert (
            wait_for_record(BODY_RATE_RECORD) is not None
        ), 'burst then slow body log'

    finally:
        min_rate_reset()


@pytest.mark.parametrize('timeout', [2, 0])
def test_settings_body_min_rate_normal_body(timeout):
    # A body that is sent faster than the floor must not be stopped, also
    # with body_read_timeout 0, when each read is checked.  The second
    # request on the same keep-alive connection comes after an idle time
    # that is longer than the grace time.  It must start with new rate
    # state.
    client.load('empty')

    min_rate_conf({'body_read_timeout': timeout, 'body_min_rate': 256})

    def req(sock, close):
        sock.sendall(
            b'POST / HTTP/1.1\r\n'
            b'Host: localhost\r\n'
            b'Content-Length: 100000\r\n'
            + (b'Connection: close\r\n' if close else b'')
            + b'\r\n'
        )

        for _ in range(10):
            time.sleep(0.2)
            sock.sendall(b'x' * 10000)

        return sock.recv(4096)

    try:
        sock = socket.create_connection(('127.0.0.1', port_map.port(8080)))
        sock.settimeout(10)

        assert req(sock, False).startswith(b'HTTP/1.1 200'), 'first'

        time.sleep(3)

        assert req(sock, True).startswith(b'HTTP/1.1 200'), 'second'

        sock.close()

    finally:
        min_rate_reset()


CONTINUE = b'HTTP/1.1 100 Continue\r\n\r\n'


def min_rate_expect(length):
    # Sends a header with "Expect: 100-continue" and reads the 100.  The
    # client sends no body byte before it gets the 100.
    sock = socket.create_connection(('127.0.0.1', port_map.port(8080)))
    sock.sendall(
        b'POST / HTTP/1.1\r\n'
        b'Host: localhost\r\n'
        b'Expect: 100-continue\r\n'
        + f'Content-Length: {length}\r\n'.encode()
        + b'Connection: close\r\n\r\n'
    )

    data = b''
    sock.settimeout(5)

    while len(data) < len(CONTINUE):
        try:
            part = sock.recv(len(CONTINUE) - len(data))
        except socket.timeout:
            break

        if not part:
            break

        data += part

    assert data == CONTINUE, '100 before the body'

    return sock


def test_settings_body_min_rate_expect_slow_body(wait_for_record):
    # After a 100 (Continue), the body read state starts in another
    # function.  The floor must be active there too.  Otherwise a client
    # can avoid the floor with "Expect: 100-continue".  The client sends
    # 1 byte each second after the 100 and must get 408.
    client.load('empty')

    min_rate_conf({'body_read_timeout': 2, 'body_min_rate': 256})

    try:
        sock = min_rate_expect(10000)
        sock.settimeout(1)

        closed, data, elapsed = min_rate_slow_body(sock, b'x', 12)
        sock.close()

        assert closed or data, 'slow body after 100 stopped'
        assert data == b'' or data.startswith(
            b'HTTP/1.1 408'
        ), 'slow body after 100 408'
        assert elapsed < 8, 'slow body after 100 stopped in time'
        assert (
            wait_for_record(BODY_RATE_RECORD) is not None
        ), 'slow body after 100 log'

    finally:
        min_rate_reset()


def test_settings_body_min_rate_expect_wait():
    # The client reads the 100, waits 1 s, and then sends the body much
    # faster than the floor.  The wait is shorter than body_read_timeout.
    # Like the gap timer, the floor counts the wait in the first window.
    # The client sends enough bytes before the first check, at 2 s, so
    # it must get 200.
    client.load('empty')

    min_rate_conf({'body_read_timeout': 2, 'body_min_rate': 256})

    try:
        sock = min_rate_expect(20000)

        time.sleep(1)

        try:
            for _ in range(20):
                sock.sendall(b'x' * 1000)
                time.sleep(0.1)

        except OSError:
            # The router answered early.  The status shows why.
            pass

        sock.settimeout(10)

        try:
            data = sock.recv(4096)
        except OSError:
            data = b''

        sock.close()

        assert data.startswith(b'HTTP/1.1 200'), 'wait after 100'

    finally:
        min_rate_reset()


@pytest.mark.parametrize('timeout', [3, 0])
def test_settings_send_min_rate_slow_read(timeout, system, wait_for_record):
    # A client reads a large response slower than send_min_rate, but fast
    # enough that the router can write some data before each send_timeout.
    # Each write starts the send_timeout gap timer again, so only the rate
    # floor can stop this client.  With send_timeout 0 there is no gap
    # timer and no grace time, and each write is checked against the time
    # since the previous write.
    #
    # The router counts the bytes that the kernel accepts.  On loopback the
    # kernel send buffer grows to some MiB, and the kernel signals a write
    # event only when about one third of the buffer is free.  A client that
    # reads only some bytes each second thus gets no write events, and the
    # gap timer stops it.  To test the floor, this test uses larger numbers:
    # the floor is 8 MiB/s and the client reads about 1 MiB/s.  The logic
    # is the same as for a floor of 256 B/s on a real network.
    #
    # After the router closes the connection, the kernel still gives the
    # client the data in the socket buffers.  Thus the test finds the close
    # in the router log.  The time limits are generous because the kernel
    # buffer sizes and the time of the write events are not exact.
    #
    # The numbers above are for the Linux loopback buffers.  Other systems
    # size and signal the send buffer in a different way.
    if system != 'Linux':
        pytest.skip('Linux loopback buffer sizes only')

    client.load('body_generate')

    min_rate_conf({'send_timeout': timeout, 'send_min_rate': 8388608})

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
        sock.settimeout(1)
        sock.connect(('127.0.0.1', port_map.port(8080)))
        sock.sendall(
            b'GET / HTTP/1.1\r\n'
            b'Host: localhost\r\n'
            b'X-Length: 67108864\r\n'
            b'Connection: close\r\n\r\n'
        )

        start = time.monotonic()
        total = 0
        closed = False

        while time.monotonic() - start < 12:
            time.sleep(0.05)

            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                closed = True
                break

            if not chunk:
                closed = True
                break

            total += len(chunk)

        assert (
            wait_for_record(
                r'client send rate is less than send_min_rate', wait=50
            )
            is not None
        ), 'slow read stopped by the floor'

        # Read the data that the kernel keeps.  Then the close must come.
        sock.settimeout(5)

        try:
            while not closed:
                chunk = sock.recv(65536)
                if not chunk:
                    closed = True
                total += len(chunk)
        except OSError:
            closed = True

        sock.close()

        assert closed, 'slow read connection closed'
        assert total < 67108864, 'slow read response cut'

    finally:
        min_rate_reset()


@pytest.mark.parametrize('timeout', [2, 0])
def test_settings_send_min_rate_fast_read(timeout):
    # A client that reads at full speed must get all the data, also with
    # send_timeout 0, when each write is checked.  Two responses on one
    # keep-alive connection check that the second response starts with
    # new rate state.
    client.load('body_generate')

    min_rate_conf({'send_timeout': timeout, 'send_min_rate': 256})

    try:
        for _ in range(2):
            resp = client.get(
                headers={
                    'Host': 'localhost',
                    'X-Length': '4194304',
                    'Connection': 'close',
                },
            )
            assert resp['status'] == 200
            assert len(resp['body']) == 4194304

        sock = socket.create_connection(('127.0.0.1', port_map.port(8080)))
        sock.settimeout(10)

        for close in (False, True):
            time.sleep(3)
            sock.sendall(
                b'GET / HTTP/1.1\r\n'
                b'Host: localhost\r\n'
                b'X-Length: 1048576\r\n'
                + (b'Connection: close\r\n' if close else b'')
                + b'\r\n'
            )

            data = b''
            while b'\r\n\r\n' not in data:
                data += sock.recv(65536)

            head, body = data.split(b'\r\n\r\n', 1)
            assert head.startswith(b'HTTP/1.1 200'), 'keep-alive status'

            while len(body) < 1048576:
                chunk = sock.recv(65536)
                assert chunk, 'keep-alive body'
                body += chunk

            assert len(body) == 1048576, 'keep-alive length'

        sock.close()

    finally:
        min_rate_reset()


def test_settings_send_min_rate_slow_application():
    # The send floor counts only the time when response data waits for the
    # client.  The time when the router waits for the application is not
    # counted.  Thus a slow application (for example a stream of events)
    # is not stopped, also with a high floor.  The application sends 4
    # parts with 1 s between them, which is longer than the grace time
    # (send_timeout 2).  The client reads at full speed.
    client.load('delayed')

    min_rate_conf({'send_timeout': 2, 'send_min_rate': 8388608})

    try:
        body = '0123456789' * 400

        resp = client.post(
            headers={
                'Host': 'localhost',
                'X-Parts': '4',
                'X-Delay': '1',
                'Connection': 'close',
            },
            body=body,
            read_timeout=30,
        )

        assert resp['status'] == 200, 'slow application status'
        assert resp['body'] == body, 'slow application body'

    finally:
        min_rate_reset()


def test_settings_send_min_rate_gap_longer_than_grace(search_in_file):
    # The time between send periods is not counted, also when it is longer
    # than the grace time.  The application sends 1000 bytes, waits 2.5 s,
    # and sends 1000 bytes more.  The grace time is 2 s.  The client reads
    # at full speed, so the first period stops after about 1 ms.  The second
    # period starts again from its own write, and its first check sees about
    # 2 ms, not 2.5 s.  If the gap was counted, the check after the second
    # write would see about 2000 bytes in 2.5 s, below the floor.  That
    # write sends the last data, so the client still gets the full body,
    # but the router closes the connection.  Thus the test sends a second
    # request on the same connection.  On a router that counts the gap,
    # that request finds the connection closed ('gap header'), and this
    # assertion fails before the log search.
    client.load('send_bursts')

    min_rate_conf({'send_timeout': 2, 'send_min_rate': 1048576})

    sock = socket.create_connection(('127.0.0.1', port_map.port(8080)))
    sock.settimeout(10)

    def response(length):
        data = b''
        while b'\r\n\r\n' not in data:
            chunk = sock.recv(65536)
            assert chunk, 'gap header'
            data += chunk

        head, body = data.split(b'\r\n\r\n', 1)
        assert head.startswith(b'HTTP/1.1 200'), 'gap status'

        while len(body) < length:
            chunk = sock.recv(65536)
            assert chunk, 'gap body'
            body += chunk

        return body

    try:
        start = time.monotonic()

        sock.sendall(
            b'GET / HTTP/1.1\r\n'
            b'Host: localhost\r\n'
            b'X-Part-Size: 1000\r\n'
            b'X-Bursts: 2\r\n'
            b'X-Interval: 2.5\r\n\r\n'
        )

        assert response(2000) == b'x' * 2000, 'gap body'
        assert time.monotonic() - start >= 2.5, 'gap time'

        sock.sendall(
            b'GET / HTTP/1.1\r\n'
            b'Host: localhost\r\n'
            b'X-Part-Size: 10\r\n'
            b'Connection: close\r\n\r\n'
        )

        assert response(10) == b'x' * 10, 'gap keep-alive body'

        assert (
            search_in_file(r'client send rate is less than send_min_rate')
            is None
        ), 'gap not stopped by the floor'

    finally:
        sock.close()
        min_rate_reset()


def test_settings_send_min_rate_stream_slow_read(
    system, temp_dir, wait_for_record
):
    # An application sends a stream in bursts, 1.5 s apart.  The client
    # keeps the socket send buffer full.  Each 1.5 s it reads only one burst.
    # Then the parts of the burst that wait can go out before send_timeout.
    # Thus each write event empties the router write queue, and each send
    # period stops there.  The router must check the rate at that point
    # too.  Otherwise it never checks this client, and the client reads
    # below the floor for as long as the stream lasts.
    #
    # The test uses a unix socket.  On Linux, the kernel takes a unix socket
    # write of 32 KiB whole or not at all, and the send buffer has the fixed
    # size net.core.wmem_default.  A write event comes when 3/4 of the
    # buffer is free.  Thus a part that does not fit waits whole, and the
    # next write event sends all the parts that wait.  No write sends only
    # some of the queued data.
    if system != 'Linux':
        pytest.skip('Linux unix socket buffers only')

    # A new unix socket gets this size, the router socket too.  A container
    # can have no /proc/sys/net/core/wmem_default, so ask a socket.
    pair = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    wmem = pair[0].getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
    pair[0].close()
    pair[1].close()

    # The first burst fills the send buffer, and 2 or 3 parts wait.  Each
    # next burst is a little more than the space that a write event frees.
    # The client reads one burst each time, so 2 or 3 parts wait in each
    # cycle, for about 1 s.  With wmem_default 212992: 9, then 6 parts.
    part = 32768
    first = wmem // part + 3
    parts = wmem * 3 // 4 // part + 2
    bursts = 5
    length = part * (first + parts * (bursts - 1))

    client.load('send_bursts')

    addr = f'{temp_dir}/sock'

    assert 'success' in client.conf(
        {f'unix:{addr}': {'application': 'send_bursts'}}, 'listeners'
    )

    # The router measures about one burst for each 1 s of waiting.  The
    # floor is four bursts each second.
    min_rate_conf({'send_timeout': 2, 'send_min_rate': 4 * parts * part})

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    start = time.monotonic()

    def read(size):
        # Returns "size" bytes, or less if the connection closes.
        data = b''

        while len(data) < size:
            try:
                chunk = sock.recv(min(65536, size - len(data)))
            except OSError:
                break

            if not chunk:
                break

            data += chunk

        return data

    try:
        sock.settimeout(5)
        sock.connect(addr)
        sock.sendall(
            b'GET / HTTP/1.1\r\n'
            b'Host: localhost\r\n'
            + f'X-First: {first}\r\n'.encode()
            + f'X-Parts: {parts}\r\n'.encode()
            + f'X-Part-Size: {part}\r\n'.encode()
            + f'X-Bursts: {bursts}\r\n'.encode()
            + b'X-Interval: 1.5\r\n'
            b'Connection: close\r\n\r\n'
        )
        start = time.monotonic()

        head = b''
        while not head.endswith(b'\r\n\r\n'):
            byte = read(1)
            if not byte:
                break

            head += byte

        assert head.startswith(b'HTTP/1.1 200'), 'stream status'

        total = 0

        for burst in range(bursts):
            time.sleep(max(0, start + 1.5 * burst + 1 - time.monotonic()))

            size = parts * part if burst < bursts - 1 else length - total
            got = len(read(size))
            total += got

            if got < size:
                break

        assert total < length, 'slow stream reader stopped'

        assert (
            wait_for_record(
                r'client send rate is less than send_min_rate', wait=50
            )
            is not None
        ), 'slow stream reader stopped by the floor'

    finally:
        sock.close()
        min_rate_reset()

        # The application still runs its schedule.  Let it end before the
        # next test changes the configuration.
        time.sleep(max(0, start + 7 - time.monotonic()))


def test_settings_min_rate_validation():
    client.load('empty')

    try:
        for name in ('body_min_rate', 'send_min_rate'):
            assert 'error' in client.conf({'http': {name: -1}}, 'settings')
            assert 'error' in client.conf({'http': {name: 1.5}}, 'settings')
            assert 'error' in client.conf({'http': {name: '256'}}, 'settings')
            assert 'error' in client.conf(
                {'http': {name: 2147483648}}, 'settings'
            )
            assert 'success' in client.conf({'http': {name: 0}}, 'settings')
            assert 'success' in client.conf(
                {'http': {name: 2147483647}}, 'settings'
            )
            assert 'success' in client.conf({'http': {name: 256}}, 'settings')

    finally:
        min_rate_reset()


TIMEOUTS = (
    'http/header_read_timeout',
    'http/body_read_timeout',
    'http/send_timeout',
    'http/idle_timeout',
    'http/websocket/read_timeout',
    'http/websocket/keepalive_interval',
)


def test_settings_timeout_validation():
    client.load('empty')

    def put(path, value):
        conf = value
        for seg in reversed(path.split('/')):
            conf = {seg: conf}

        return client.conf(conf, 'settings')

    try:
        for path in TIMEOUTS:
            for value in (-1, 2147484, 4294968):
                assert 'error' in put(path, value), f'{path} {value}'

            assert 'success' in put(path, 0), path
            assert 'success' in put(path, 2147483), path

        resp = put('http/idle_timeout', -1)
        assert (
            resp['detail'] == 'The "idle_timeout" number must not be negative.'
        ), 'message'
        assert resp['location']['path'] == '/settings/http/idle_timeout'

        resp = put('http/idle_timeout', 2147484)
        assert (
            resp['detail']
            == 'The "idle_timeout" number must not exceed 2147483.'
        ), 'message 2'

    finally:
        # The default no-restart suite preserves /settings between tests.
        client.conf_delete('settings/http')


def test_settings_timeout_stored(
    requires_restart, wait_for_record, monkeypatch
):
    """Earlier versions accepted any timeout.  unitd still loads a stored
    configuration with one out of the range, and it serves requests."""

    client.load('empty')

    conf = client.conf_get()
    conf['settings'] = {'http': {'idle_timeout': -1}}

    unit_stop()

    statedir = Path(tempfile.mkdtemp(prefix='unit-state-'))
    (statedir / 'conf.json').write_text(json.dumps(conf))

    # unit_run() checks that /status lists no application.  The stored
    # configuration has one.
    monkeypatch.setattr(Status, '_check_zeros', lambda: None)

    try:
        unit_run(state_dir=str(statedir))

        assert (
            client.conf_get('settings/http/idle_timeout') == -1
        ), 'stored configuration loaded'

        assert wait_for_record(
            r'\[warn\].+the restored configuration has a '
            r'"idle_timeout" number out of the range 0 to 2147483 at '
            r'"/settings/http/idle_timeout"'
        ), 'warning'

        # The router applied the configuration, so the listener answers.
        # The idle timer fires at once, as before, so the answer can be
        # 408.
        assert client.get()['status'] in (200, 408), 'served'

        resp = client.conf(
            {'*:8080': {'pass': 'applications/empty'}}, 'listeners'
        )
        assert 'error' in resp, 'update refused'
        assert (
            resp['location']['path'] == '/settings/http/idle_timeout'
        ), 'pointer'

    finally:
        unit_stop()
        shutil.rmtree(statedir, ignore_errors=True)


def test_settings_idle_timeout():
    client.load('empty')

    def req():
        (_, sock) = client.get(
            headers={'Host': 'localhost', 'Connection': 'keep-alive'},
            start=True,
            read_timeout=1,
        )

        time.sleep(3)

        return client.get(sock=sock)

    assert client.get()['status'] == 200, 'init'

    assert 'success' in client.conf({'http': {'idle_timeout': 2}}, 'settings')
    assert req()['status'] == 408, 'status idle timeout'

    assert 'success' in client.conf({'http': {'idle_timeout': 7}}, 'settings')
    assert req()['status'] == 200, 'status idle timeout 2'


def test_settings_idle_timeout_2():
    client.load('empty')

    def req():
        sock = client.http(b'', raw=True, no_recv=True)

        time.sleep(3)

        return client.get(sock=sock)

    assert client.get()['status'] == 200, 'init'

    assert 'success' in client.conf({'http': {'idle_timeout': 1}}, 'settings')
    assert req()['status'] == 408, 'status idle timeout'

    assert 'success' in client.conf({'http': {'idle_timeout': 7}}, 'settings')
    assert req()['status'] == 200, 'status idle timeout 2'


def test_settings_max_body_size():
    client.load('empty')

    assert 'success' in client.conf({'http': {'max_body_size': 5}}, 'settings')

    assert client.post(body='01234')['status'] == 200, 'status size'
    assert client.post(body='012345')['status'] == 413, 'status size max'


def test_settings_max_body_size_large():
    client.load('mirror')

    assert 'success' in client.conf(
        {'http': {'max_body_size': 32 * 1024 * 1024}}, 'settings'
    )

    body = '0123456789abcdef' * 4 * 64 * 1024
    resp = client.post(body=body, read_buffer_size=1024 * 1024)
    assert resp['status'] == 200, 'status size 4'
    assert resp['body'] == body, 'status body 4'

    body = '0123456789abcdef' * 8 * 64 * 1024
    resp = client.post(body=body, read_buffer_size=1024 * 1024)
    assert resp['status'] == 200, 'status size 8'
    assert resp['body'] == body, 'status body 8'

    body = '0123456789abcdef' * 16 * 64 * 1024
    resp = client.post(body=body, read_buffer_size=1024 * 1024)
    assert resp['status'] == 200, 'status size 16'
    assert resp['body'] == body, 'status body 16'

    body = '0123456789abcdef' * 32 * 64 * 1024
    resp = client.post(body=body, read_buffer_size=1024 * 1024)
    assert resp['status'] == 200, 'status size 32'
    assert resp['body'] == body, 'status body 32'


def test_settings_body_buffer_size():
    client.load('mirror')

    assert 'success' in client.conf(
        {
            'http': {
                'max_body_size': 64 * 1024 * 1024,
                'body_buffer_size': 32 * 1024 * 1024,
            }
        },
        'settings',
    )

    body = '0123456789abcdef'
    resp = client.post(body=body)
    assert bool(resp), 'response from application'
    assert resp['status'] == 200, 'status'
    assert resp['body'] == body, 'body'

    body = '0123456789abcdef' * 1024 * 1024
    resp = client.post(body=body, read_buffer_size=1024 * 1024)
    assert bool(resp), 'response from application 2'
    assert resp['status'] == 200, 'status 2'
    assert resp['body'] == body, 'body 2'

    body = '0123456789abcdef' * 2 * 1024 * 1024
    resp = client.post(body=body, read_buffer_size=1024 * 1024)
    assert bool(resp), 'response from application 3'
    assert resp['status'] == 200, 'status 3'
    assert resp['body'] == body, 'body 3'

    body = '0123456789abcdef' * 3 * 1024 * 1024
    resp = client.post(body=body, read_buffer_size=1024 * 1024)
    assert bool(resp), 'response from application 4'
    assert resp['status'] == 200, 'status 4'
    assert resp['body'] == body, 'body 4'


def test_settings_log_route(findall, search_in_file, wait_for_record):
    def count_fallbacks():
        return len(findall(r'"fallback" taken'))

    def check_record(template):
        assert search_in_file(template) is not None

    def check_no_record(template):
        assert search_in_file(template) is None

    def template_req_line(url):
        return rf'\[notice\].*http request line "GET {url} HTTP/1\.1"'

    def template_selected(route):
        return rf'\[notice\].*"{route}" selected'

    def template_discarded(route):
        return rf'\[info\].*"{route}" discarded'

    def wait_for_request_log(status, uri, route):
        assert client.get(url=uri)['status'] == status
        assert wait_for_record(template_req_line(uri)) is not None
        assert wait_for_record(template_selected(route)) is not None

    # routes array

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "match": {
                        "uri": "/zero",
                    },
                    "action": {"return": 200},
                },
                {
                    "action": {"return": 201},
                },
            ],
            "applications": {},
            "settings": {"http": {"log_route": True}},
        }
    )

    wait_for_request_log(200, '/zero', 'routes/0')
    check_no_record(r'discarded')

    wait_for_request_log(201, '/one', 'routes/1')
    check_record(template_discarded('routes/0'))

    # routes object

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes/main"}},
            "routes": {
                "main": [
                    {
                        "match": {
                            "uri": "/named_route",
                        },
                        "action": {"return": 200},
                    },
                    {
                        "action": {"return": 201},
                    },
                ]
            },
            "applications": {},
            "settings": {"http": {"log_route": True}},
        }
    )

    wait_for_request_log(200, '/named_route', 'routes/main/0')
    check_no_record(template_discarded('routes/main'))

    wait_for_request_log(201, '/unnamed_route', 'routes/main/1')
    check_record(template_discarded('routes/main/0'))

    # routes sequence

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes/first"}},
            "routes": {
                "first": [
                    {
                        "action": {"pass": "routes/second"},
                    },
                ],
                "second": [
                    {
                        "action": {"return": 200},
                    },
                ],
            },
            "applications": {},
            "settings": {"http": {"log_route": True}},
        }
    )

    wait_for_request_log(200, '/sequence', 'routes/second/0')
    check_record(template_selected('routes/first/0'))

    # fallback

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes/fall"}},
            "routes": {
                "fall": [
                    {
                        "action": {
                            "share": "/blah",
                            "fallback": {"pass": "routes/fall2"},
                        },
                    },
                ],
                "fall2": [
                    {
                        "action": {"return": 200},
                    },
                ],
            },
            "applications": {},
            "settings": {"http": {"log_route": True}},
        }
    )

    wait_for_request_log(200, '/', 'routes/fall2/0')
    assert count_fallbacks() == 1
    check_record(template_selected('routes/fall/0'))

    assert client.head()['status'] == 200
    assert count_fallbacks() == 2

    # disable log

    assert 'success' in client.conf({"log_route": False}, 'settings/http')

    url = '/disable_logging'
    assert client.get(url=url)['status'] == 200

    time.sleep(1)

    check_no_record(template_req_line(url))

    # total

    assert len(findall(r'\[notice\].*http request line')) == 7
    assert len(findall(r'\[notice\].*selected')) == 10
    assert len(findall(r'\[info\].*discarded')) == 2
