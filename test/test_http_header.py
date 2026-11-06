import socket

import pytest

from unit.applications.lang.python import ApplicationPython

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()


def test_http_header_value_leading_sp():
    client.load('custom_header')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Custom-Header': ' ,',
            'Connection': 'close',
        }
    )

    assert resp['status'] == 200, 'value leading sp status'
    assert (
        resp['headers']['Custom-Header'] == ','
    ), 'value leading sp custom header'


def test_http_header_value_leading_htab():
    client.load('custom_header')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Custom-Header': '\t,',
            'Connection': 'close',
        }
    )

    assert resp['status'] == 200, 'value leading htab status'
    assert (
        resp['headers']['Custom-Header'] == ','
    ), 'value leading htab custom header'


def test_http_header_value_trailing_sp():
    client.load('custom_header')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Custom-Header': ', ',
            'Connection': 'close',
        }
    )

    assert resp['status'] == 200, 'value trailing sp status'
    assert (
        resp['headers']['Custom-Header'] == ','
    ), 'value trailing sp custom header'


def test_http_header_value_trailing_htab():
    client.load('custom_header')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Custom-Header': ',\t',
            'Connection': 'close',
        }
    )

    assert resp['status'] == 200, 'value trailing htab status'
    assert (
        resp['headers']['Custom-Header'] == ','
    ), 'value trailing htab custom header'


def test_http_header_value_both_sp():
    client.load('custom_header')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Custom-Header': ' , ',
            'Connection': 'close',
        }
    )

    assert resp['status'] == 200, 'value both sp status'
    assert (
        resp['headers']['Custom-Header'] == ','
    ), 'value both sp custom header'


def test_http_header_value_both_htab():
    client.load('custom_header')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Custom-Header': '\t,\t',
            'Connection': 'close',
        }
    )

    assert resp['status'] == 200, 'value both htab status'
    assert (
        resp['headers']['Custom-Header'] == ','
    ), 'value both htab custom header'


def test_http_header_value_chars():
    client.load('custom_header')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Custom-Header': r"(),/:;<=>?@[\]{}\t !#$%&'*+-.^_`|~",
            'Connection': 'close',
        }
    )

    assert resp['status'] == 200, 'value chars status'
    assert (
        resp['headers']['Custom-Header']
        == r"(),/:;<=>?@[\]{}\t !#$%&'*+-.^_`|~"
    ), 'value chars custom header'


def test_http_header_value_chars_edge():
    client.load('custom_header')

    resp = client.http(
        b"""GET / HTTP/1.1
Host: localhost
Custom-Header: \x20\xFF
Connection: close

""",
        raw=True,
        encoding='latin1',
    )

    assert resp['status'] == 200, 'value chars edge status'
    assert resp['headers']['Custom-Header'] == '\xFF', 'value chars edge'


def test_http_header_value_chars_below():
    client.load('custom_header')

    resp = client.http(
        b"""GET / HTTP/1.1
Host: localhost
Custom-Header: \x1F
Connection: close

""",
        raw=True,
    )

    assert resp['status'] == 400, 'value chars below'


def test_http_header_field_leading_sp():
    client.load('empty')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                ' Custom-Header': 'blah',
                'Connection': 'close',
            }
        )['status']
        == 400
    ), 'field leading sp'


def test_http_header_field_leading_htab():
    client.load('empty')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                '\tCustom-Header': 'blah',
                'Connection': 'close',
            }
        )['status']
        == 400
    ), 'field leading htab'


def test_http_header_field_trailing_sp():
    client.load('empty')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'Custom-Header ': 'blah',
                'Connection': 'close',
            }
        )['status']
        == 400
    ), 'field trailing sp'


def test_http_header_field_trailing_htab():
    client.load('empty')

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'Custom-Header\t': 'blah',
                'Connection': 'close',
            }
        )['status']
        == 400
    ), 'field trailing htab'


def test_http_header_content_length_big():
    client.load('empty')

    assert (
        client.post(
            headers={
                'Host': 'localhost',
                'Content-Length': str(2**64),
                'Connection': 'close',
            },
            body='X' * 1000,
        )['status']
        == 400
    ), 'Content-Length big'


def test_http_header_content_length_negative():
    client.load('empty')

    assert (
        client.post(
            headers={
                'Host': 'localhost',
                'Content-Length': '-100',
                'Connection': 'close',
            },
            body='X' * 1000,
        )['status']
        == 400
    ), 'Content-Length negative'


def test_http_header_content_length_text():
    client.load('empty')

    assert (
        client.post(
            headers={
                'Host': 'localhost',
                'Content-Length': 'blah',
                'Connection': 'close',
            },
            body='X' * 1000,
        )['status']
        == 400
    ), 'Content-Length text'


def test_http_header_content_length_multiple_values():
    client.load('empty')

    assert (
        client.post(
            headers={
                'Host': 'localhost',
                'Content-Length': '41, 42',
                'Connection': 'close',
            },
            body='X' * 1000,
        )['status']
        == 400
    ), 'Content-Length multiple value'


def test_http_header_content_length_multiple_fields():
    client.load('empty')

    assert (
        client.post(
            headers={
                'Host': 'localhost',
                'Content-Length': ['41', '42'],
                'Connection': 'close',
            },
            body='X' * 1000,
        )['status']
        == 400
    ), 'Content-Length multiple fields'


@pytest.mark.skip('not yet')
def test_http_header_host_absent():
    client.load('host')

    resp = client.get(headers={'Connection': 'close'})

    assert resp['status'] == 400, 'Host absent status'


def test_http_header_host_empty():
    client.load('host')

    resp = client.get(headers={'Host': '', 'Connection': 'close'})

    assert resp['status'] == 200, 'Host empty status'
    assert resp['headers']['X-Server-Name'] != '', 'Host empty SERVER_NAME'


def test_http_header_host_big():
    client.load('empty')

    assert (
        client.get(headers={'Host': 'X' * 10000, 'Connection': 'close'})[
            'status'
        ]
        == 431
    ), 'Host big'


def test_http_header_host_port():
    client.load('host')

    resp = client.get(
        headers={'Host': 'exmaple.com:8080', 'Connection': 'close'}
    )

    assert resp['status'] == 200, 'Host port status'
    assert (
        resp['headers']['X-Server-Name'] == 'exmaple.com'
    ), 'Host port SERVER_NAME'
    assert (
        resp['headers']['X-Http-Host'] == 'exmaple.com:8080'
    ), 'Host port HTTP_HOST'


def test_http_header_host_port_empty():
    client.load('host')

    resp = client.get(headers={'Host': 'exmaple.com:', 'Connection': 'close'})

    assert resp['status'] == 200, 'Host port empty status'
    assert (
        resp['headers']['X-Server-Name'] == 'exmaple.com'
    ), 'Host port empty SERVER_NAME'
    assert (
        resp['headers']['X-Http-Host'] == 'exmaple.com:'
    ), 'Host port empty HTTP_HOST'


def test_http_header_host_literal():
    client.load('host')

    resp = client.get(headers={'Host': '127.0.0.1', 'Connection': 'close'})

    assert resp['status'] == 200, 'Host literal status'
    assert (
        resp['headers']['X-Server-Name'] == '127.0.0.1'
    ), 'Host literal SERVER_NAME'


def test_http_header_host_literal_ipv6():
    client.load('host')

    resp = client.get(headers={'Host': '[::1]:8080', 'Connection': 'close'})

    assert resp['status'] == 200, 'Host literal ipv6 status'
    assert (
        resp['headers']['X-Server-Name'] == '[::1]'
    ), 'Host literal ipv6 SERVER_NAME'
    assert (
        resp['headers']['X-Http-Host'] == '[::1]:8080'
    ), 'Host literal ipv6 HTTP_HOST'


def test_http_header_host_trailing_period():
    client.load('host')

    resp = client.get(headers={'Host': '127.0.0.1.', 'Connection': 'close'})

    assert resp['status'] == 200, 'Host trailing period status'
    assert (
        resp['headers']['X-Server-Name'] == '127.0.0.1'
    ), 'Host trailing period SERVER_NAME'
    assert (
        resp['headers']['X-Http-Host'] == '127.0.0.1.'
    ), 'Host trailing period HTTP_HOST'


def test_http_header_host_trailing_period_2():
    client.load('host')

    resp = client.get(headers={'Host': 'EXAMPLE.COM.', 'Connection': 'close'})

    assert resp['status'] == 200, 'Host trailing period 2 status'
    assert (
        resp['headers']['X-Server-Name'] == 'example.com'
    ), 'Host trailing period 2 SERVER_NAME'
    assert (
        resp['headers']['X-Http-Host'] == 'EXAMPLE.COM.'
    ), 'Host trailing period 2 HTTP_HOST'


def test_http_header_host_case_insensitive():
    client.load('host')

    resp = client.get(headers={'Host': 'EXAMPLE.COM', 'Connection': 'close'})

    assert resp['status'] == 200, 'Host case insensitive'
    assert (
        resp['headers']['X-Server-Name'] == 'example.com'
    ), 'Host case insensitive SERVER_NAME'


def test_http_header_host_double_dot():
    client.load('empty')

    assert (
        client.get(headers={'Host': '127.0.0..1', 'Connection': 'close'})[
            'status'
        ]
        == 400
    ), 'Host double dot'


def test_http_header_host_slash():
    client.load('empty')

    assert (
        client.get(headers={'Host': '/localhost', 'Connection': 'close'})[
            'status'
        ]
        == 400
    ), 'Host slash'


def test_http_header_host_multiple_fields():
    client.load('empty')

    assert (
        client.get(
            headers={
                'Host': ['localhost', 'example.com'],
                'Connection': 'close',
            }
        )['status']
        == 400
    ), 'Host multiple fields'


def test_http_discard_unsafe_fields():
    client.load('header_fields')

    def check_status(header):
        resp = client.get(
            headers={
                'Host': 'localhost',
                header: 'blah',
                'Connection': 'close',
            }
        )

        assert resp['status'] == 200
        return resp

    resp = check_status("!Custom-Header")
    assert 'CUSTOM' not in resp['headers']['All-Headers']

    resp = check_status("Custom_Header")
    assert 'CUSTOM' not in resp['headers']['All-Headers']

    assert 'success' in client.conf(
        {'http': {'discard_unsafe_fields': False}},
        'settings',
    )

    resp = check_status("!#$%&'*+.^`|~Custom_Header")
    assert 'CUSTOM' in resp['headers']['All-Headers']

    assert 'success' in client.conf(
        {'http': {'discard_unsafe_fields': True}},
        'settings',
    )

    resp = check_status("!Custom-Header")
    assert 'CUSTOM' not in resp['headers']['All-Headers']

    resp = check_status("Custom_Header")
    assert 'CUSTOM' not in resp['headers']['All-Headers']


def test_http_header_fields_inline_spill():
    # The parser stores the first 16 header fields inline and spills any
    # further fields into a list; every field must still reach the
    # application regardless of where it lands.  Exercise counts below, at,
    # and well past the 16-field boundary.
    client.load('header_fields')

    for count in [1, 15, 16, 17, 32, 48]:
        headers = {'Host': 'localhost', 'Connection': 'close'}
        for i in range(count):
            headers[f'X-Test-{i}'] = str(i)

        resp = client.get(headers=headers)

        assert resp['status'] == 200, f'{count} headers status'

        got = resp['headers']['All-Headers']
        for i in range(count):
            assert f'HTTP_X_TEST_{i}' in got, f'field {i} of {count} present'


def test_http_header_fields_keepalive_spill():
    # The parser struct is reused across keep-alive requests (num_inline_fields
    # is reset per request).  Vary the header count on the same connection --
    # inline-only, spilled past 16, then back below the boundary -- and confirm
    # each request's fields are all delivered, so a stale count from the
    # previous request cannot corrupt the next one.
    client.load('header_fields')

    sock = None
    for count in [3, 20, 5, 32]:
        headers = {'Host': 'localhost', 'Connection': 'keep-alive'}
        for i in range(count):
            headers[f'X-Test-{i}'] = str(i)

        kwargs = {'headers': headers, 'start': True, 'read_timeout': 1}
        if sock is not None:
            kwargs['sock'] = sock

        resp, sock = client.get(**kwargs)

        assert resp['status'] == 200, f'{count}-header keep-alive status'

        got = resp['headers']['All-Headers']
        for i in range(count):
            assert f'HTTP_X_TEST_{i}' in got, f'keep-alive field {i} of {count}'

    sock.close()


def test_http_header_large_buffers_above_255():
    # The parser rewinds only to the start of an incomplete field, so a
    # header of many short fields takes one large buffer for every few
    # fields.  The first buffer has the default size of 2048 bytes and holds
    # about 68 fields.  With 128-byte large buffers and 30-byte fields, 4
    # fields fit in one buffer, so 1200 fields take about 283 large buffers
    # and 1350 fields take about 320.  The count of
    # buffers was 8 bits wide, so a limit of 256 or more never fired.
    client.load('empty')

    assert 'success' in client.conf(
        {
            'http': {
                'large_header_buffer_size': 128,
                'large_header_buffers': 300,
            }
        },
        'settings',
    )

    def fields(count, expect):
        headers = {'Host': 'localhost', 'Connection': 'close'}
        for i in range(count):
            headers[f'X-{i:04}'] = 'a' * 20

        assert client.get(headers=headers)['status'] == expect, count

    fields(1200, 200)
    fields(1350, 431)


def connection_exchange(version, lines):
    """Send a request with the "Connection" field lines "lines", read its
    response, then send a second request on the same socket.

    Return the head of the first response, in lower case, and the first
    bytes of the answer to the second request.  An empty answer means that
    the server closed the connection after the first response: the read gave
    EOF, or the socket was reset.  A server that neither answers nor closes
    within 10 seconds fails the test.
    """
    request = f'GET / HTTP/{version}\r\nHost: localhost\r\n'

    for line in lines:
        request += f'Connection: {line}\r\n'

    sock = client.http(f'{request}\r\n'.encode(), raw=True, no_recv=True)

    try:
        first = client.recvall(sock, framed=True, read_timeout=10)

        # recvall() also returns b'' when its read_timeout ends, so the
        # second answer is read with the timeout of the socket.
        sock.settimeout(10)

        try:
            sock.sendall(
                b'GET / HTTP/1.1\r\nHost: localhost\r\n'
                b'Connection: close\r\n\r\n'
            )
            second = sock.recv(4096)

        except socket.timeout:
            pytest.fail('no answer and no close in 10 seconds')

        except OSError:
            # The server closed the connection, and the socket was reset.
            second = b''

    finally:
        sock.close()

    return first.partition(b'\r\n\r\n')[0].lower(), second


@pytest.mark.parametrize(
    'version, lines, keep',
    [
        # RFC 9112, 9.6: "close" closes the connection, also when the
        # request has "keep-alive" too, in one field line or in two.
        pytest.param('1.1', ['close, keep-alive'], False, id='close-first'),
        pytest.param('1.1', ['keep-alive, close'], False, id='close-last'),
        pytest.param('1.1', ['close', 'keep-alive'], False, id='lines'),
        pytest.param('1.1', ['keep-alive', 'close'], False, id='lines-close'),
        pytest.param('1.0', ['close, keep-alive'], False, id='http10'),
        # RFC 9110, 5.6.1: spaces and tabs around an element, and empty
        # elements.
        pytest.param('1.1', ['  close ,'], False, id='ows'),
        pytest.param('1.1', [',\tClose\t, ,'], False, id='ows-tab'),
        # RFC 9110, 7.6.1: only a whole token is an option.
        pytest.param('1.1', ['X-Closer'], True, id='x-closer'),
        pytest.param('1.0', ['Keep-Alive-Extension'], False, id='ka-ext'),
        pytest.param('1.0', ['keep-alive'], True, id='ka'),
        pytest.param('1.0', ['KEEP-ALIVE'], True, id='ka-upper'),
        pytest.param('1.0', ['TE, Keep-Alive'], True, id='ka-list'),
    ],
)
def test_http_header_connection(version, lines, keep):
    client.load('empty')

    head, second = connection_exchange(version, lines)

    assert head.startswith(b'http/1.1 200 '), head

    connection = [
        line
        for line in head.split(b'\r\n')
        if line.startswith(b'connection:')
    ]

    # The response names only what differs from the default of the version.
    if version == '1.1':
        assert connection == ([] if keep else [b'connection: close'])

    else:
        assert connection == ([b'connection: keep-alive'] if keep else [])

    if keep:
        assert second.startswith(b'HTTP/1.1 200 '), 'second request answered'

    else:
        assert second == b'', 'connection closed after the first response'


def test_http_header_connection_upgrade_token():
    # "upgrade" inside a longer token is not the "upgrade" option, so this
    # request is no WebSocket handshake.
    client.load('empty')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'Connection': 'X-Upgrade-Info, close',
            'Upgrade': 'websocket',
        }
    )

    assert resp['status'] == 200, 'not a handshake'
