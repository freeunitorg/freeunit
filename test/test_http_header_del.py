import socket
import time

import pytest

from conftest import run_process
from unit.applications.lang.python import ApplicationPython
from unit.option import option
from unit.utils import waitforsocket
from unit import port as port_map

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()
SERVER_PORT = port_map.port(7997)

# Each test that waits for an answer bounds the wait well below the timeout
# that the old parser waited for, so a test that gets its answer in time
# shows that the line was rejected when the scan read DEL.
FAST = 5


def run_server(server_port):
    """An upstream that answers with a field line that has DEL and stays open.

    The request line chooses the answer: "/early" sends a 103 with the line,
    any other path a 200.  The line is not ended, so only the DEL can end the
    response before the proxy timeout.
    """

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', server_port))
    sock.listen(5)

    conns = []

    while True:
        connection, _ = sock.accept()
        data = connection.recv(4096)

        if b' /early ' in data:
            connection.sendall(b'HTTP/1.1 103 Early Hints\r\nX: a\x7f')

        else:
            connection.sendall(b'HTTP/1.1 200 OK\r\nX: a\x7f')

        # Keep the connection open, as a stalled upstream does.
        conns.append(connection)


@pytest.fixture(autouse=True)
def setup_method_fixture():
    run_process(run_server, SERVER_PORT)
    waitforsocket(SERVER_PORT)

    assert 'success' in client.conf(
        {
            "listeners": {
                "*:8080": {"pass": "applications/empty"},
                "*:8081": {"pass": "routes"},
            },
            "routes": [
                {"action": {"proxy": f'http://127.0.0.1:{SERVER_PORT}'}}
            ],
            "applications": {
                "empty": {
                    "type": client.get_application_type(),
                    "processes": {"spare": 0},
                    "path": f'{option.test_dir}/python/empty',
                    "working_directory": f'{option.test_dir}/python/empty',
                    "module": "wsgi",
                }
            },
        }
    )


def get_raw(data, port=8080):
    start = time.monotonic()
    sock = client.http(data, raw=True, no_recv=True, port=port)
    resp = client.recvall(sock, read_timeout=FAST).decode(errors='replace')
    sock.close()

    return client._resp_to_dict(resp), time.monotonic() - start


def test_http_header_del_complete_line():
    resp, _ = get_raw(b'GET / HTTP/1.1\r\nHost: localhost\r\nX: a\x7f\r\n\r\n')
    assert resp['status'] == 400, 'complete line with DEL'


def test_http_header_del_open_line():
    # The client sends DEL and stops.  The old parser waited for the end of
    # the line and answered 408 at "header_read_timeout".
    assert 'success' in client.conf(
        {'http': {'header_read_timeout': 30}}, 'settings'
    )

    resp, took = get_raw(b'GET / HTTP/1.1\r\nHost: localhost\r\nX: a\x7f')
    assert resp['status'] == 400, 'open line with DEL'
    assert took < FAST, 'rejected at once'


def test_http_header_del_overflow():
    # The line with DEL does not fit the header buffers.  The old parser
    # answered 431, as for any line that is too long.
    value = b'a\x7f' + b'b' * 16384
    resp, _ = get_raw(b'GET / HTTP/1.1\r\nHost: localhost\r\nX: ' + value)
    assert resp['status'] == 400, 'line with DEL that overflows'


def test_http_header_del_overflow_control():
    # The same line without DEL still gets 431.
    value = b'a' + b'b' * 16384
    resp, _ = get_raw(b'GET / HTTP/1.1\r\nHost: localhost\r\nX: ' + value)
    assert resp['status'] == 431, 'line without DEL that overflows'


@pytest.mark.parametrize('path', ['/', '/early'])
def test_http_header_del_upstream(path):
    # The upstream sends a line with DEL and holds the connection.  The old
    # parser waited for the end of the line and answered 504 at the proxy
    # timeout.
    resp, took = get_raw(
        f'GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n'
        '\r\n'.encode(),
        port=8081,
    )
    assert resp['status'] == 502, f'upstream line with DEL {path}'
    assert took < FAST, 'rejected at once'
