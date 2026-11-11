import socket

import pytest

from unit import port as port_map
from unit.control import Control

client = Control()


@pytest.fixture(autouse=True)
def setup_method_fixture():
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    "match": {
                        "uri": ["/pa th", "/pa+th"],
                        "arguments": {"q": "a b c"},
                    },
                    "action": {"return": 200},
                },
                {"action": {"return": 201}},
            ],
            "applications": {},
        }
    )


def request(target, version='1.1', connection='keep-alive'):
    return (
        f'GET {target} HTTP/{version}\r\n'
        f'Host: localhost\r\nConnection: {connection}\r\n\r\n'
    ).encode()


def recv_to_close(sock):
    """A timeout fails when the server is expected to close the connection."""
    data = bytearray()

    while True:
        try:
            part = sock.recv(4096)
        except ConnectionResetError:
            break

        if not part:
            break

        data += part

    return bytes(data)


@pytest.mark.parametrize('version', ['1.0', '1.1'])
@pytest.mark.parametrize(
    'target',
    [
        '/pa th',
        '/pa  th',
        '/?q=a b',
        '/./pa th',
        '//pa th',
        '/pa%20 th',
        '/#a b',
        '/ HTTP/1.0',
    ],
)
def test_http_request_target_raw_space(target, version):
    with socket.create_connection(
        ('127.0.0.1', port_map.port(8080)), timeout=10
    ) as sock:
        # An invalid first request must not expose a pipelined second one.
        sock.sendall(
            request(target, version)
            + request('/probe', connection='close')
        )
        response = recv_to_close(sock)

    assert response.startswith((b'HTTP/1.0 400 ', b'HTTP/1.1 400 ')), response
    assert response.count(b'HTTP/1.') == 1, response


@pytest.mark.parametrize(
    'target',
    ['/pa%20th?q=a%20b+c', '/pa+th?q=a+b%20c'],
)
def test_http_request_target_encoded_space_keepalive(target):
    with socket.create_connection(
        ('127.0.0.1', port_map.port(8080)), timeout=10
    ) as sock:
        sock.sendall(request(target))
        response = client.recvall(sock, framed=True, read_timeout=10)
        assert response.startswith(b'HTTP/1.1 200 '), response

        sock.sendall(request('/probe', connection='close'))
        response = recv_to_close(sock)
        assert response.startswith(b'HTTP/1.1 201 '), response
        assert response.count(b'HTTP/1.') == 1, response
