"""The PROXY protocol header at the start of a connection.

A listener with ``"proxy_protocol": "v1"`` or ``"v2"`` expects the header
from HAProxy's doc/proxy-protocol.txt on every connection, and the address
it announces is the peer address from then on: ``$remote_addr`` in the
access log, ``REMOTE_ADDR`` in the application, the routes ``source`` match
and the ``forwarded`` trust check.  A v2 header can also say that the client
spoke TLS to the proxy, which the application sees as ``https``.

The client here is the proxy: it opens a raw socket, writes the header
bytes and then the request.  A connection without a valid header must be
closed without a response, and the listener must keep serving after that.
"""

import socket
import ssl
import struct
import time

import pytest

from unit.applications.tls import ApplicationTLS
from unit.option import option
from unit import port as port_map

prerequisites = {'modules': {'python': 'any', 'openssl': 'any'}}

# ApplicationTLS loads the Python echo application with a plain "python"
# type, so one client serves the plain and the TLS listeners.
client = ApplicationTLS()

PP2_SIGNATURE = b'\r\n\r\n\x00\r\nQUIT\n'
PP2_PROXY = 0x21
PP2_LOCAL = 0x20
PP2_TCP4 = 0x11
PP2_TCP6 = 0x21
PP2_TYPE_SSL = 0x20
PP2_CLIENT_SSL = 0x01

REQUEST = b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'


def pp2_header(
    src='192.0.2.10',
    dst='192.0.2.1',
    src_port=40000,
    dst_port=443,
    cmd=PP2_PROXY,
    family=None,
    tlvs=b'',
    length=None,
    signature=PP2_SIGNATURE,
):
    """A v2 header.  ``length`` overrides the declared length."""

    if ':' in src:
        family = PP2_TCP6 if family is None else family
        addrs = socket.inet_pton(socket.AF_INET6, src) + socket.inet_pton(
            socket.AF_INET6, dst
        )
    else:
        family = PP2_TCP4 if family is None else family
        addrs = socket.inet_pton(socket.AF_INET, src) + socket.inet_pton(
            socket.AF_INET, dst
        )

    body = addrs + struct.pack('!HH', src_port, dst_port) + tlvs

    if length is None:
        length = len(body)

    return signature + struct.pack('!BBH', cmd, family, length) + body


def pp2_ssl_tlv(client=PP2_CLIENT_SSL, verify=0):
    value = struct.pack('!BI', client, verify)
    return struct.pack('!BH', PP2_TYPE_SSL, len(value)) + value


def pp1_header(src='192.0.2.10', dst='192.0.2.1', src_port=40000, dst_port=443):
    proto = 'TCP6' if ':' in src else 'TCP4'
    return f'PROXY {proto} {src} {dst} {src_port} {dst_port}\r\n'.encode()


def listener(version, tls=False, **extra):
    conf = {'pass': 'applications/forwarded_header', **extra}

    if version is not None:
        conf['proxy_protocol'] = version

    if tls:
        conf['tls'] = {'certificate': 'default'}

    return client.conf(conf, 'listeners/127.0.0.1:8081')


def connect(sock_type='ipv4'):
    if sock_type == 'ipv6':
        sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        sock.connect(('::1', port_map.port(8082)))
    else:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(('127.0.0.1', port_map.port(8081)))

    sock.settimeout(5)
    return sock


def recv_all(sock):
    data = b''

    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk

    return data


def request(header, sock_type='ipv4', request=REQUEST):
    """Sends the header and the request on one connection.

    Returns the parsed response, or None when the connection was closed
    without a response.
    """

    sock = connect(sock_type)
    sock.sendall(header + request)

    try:
        data = recv_all(sock)
    finally:
        sock.close()

    if data == b'':
        return None

    return client._resp_to_dict(data.decode())


def assert_dropped(header, sock_type='ipv4', request=REQUEST):
    sock = connect(sock_type)
    sock.sendall(header + request)

    try:
        data = recv_all(sock)
    finally:
        sock.close()

    assert data == b'', 'connection closed without a response'


def ipv6_available():
    try:
        sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    except OSError:
        return False

    try:
        sock.bind(('::1', 0))
    except OSError:
        return False
    finally:
        sock.close()

    return True


def assert_serves(sock_type='ipv4'):
    resp = request(pp2_header(src='198.51.100.1'), sock_type=sock_type)
    assert resp['status'] == 200, 'listener still serves'
    assert resp['headers']['Remote-Addr'] == '198.51.100.1'


@pytest.fixture(autouse=True)
def setup_method_fixture():
    client.load('forwarded_header')

    assert 'success' in listener('v2'), 'listener configure'


# Configuration.


def test_proxy_protocol_conf():
    assert 'success' in listener('v1'), 'v1'
    assert 'success' in listener('v2'), 'v2'
    assert 'success' in listener('off'), 'off'
    assert 'success' in listener(None), 'absent'

    for bad in ['V1', 'v3', 'on', 'true', '', 'v1 v2']:
        assert 'error' in listener(bad), f'string {bad!r}'

    assert 'error' in listener(True), 'boolean'
    assert 'error' in listener(2), 'number'
    assert 'error' in listener(['v1', 'v2']), 'array'

    r = client.conf(
        {'pass': 'applications/forwarded_header', 'proxy_protocol': 'v3'},
        'listeners/127.0.0.1:8081',
    )
    assert (
        r['detail'] == 'The "proxy_protocol" value must be "v1", "v2" or "off".'
    ), 'error text'


def test_proxy_protocol_off_is_absent():
    assert 'success' in listener('off')
    assert client.get(port=8081)['headers']['Remote-Addr'] == '127.0.0.1'

    # A header is just a bad request line to a listener without the option.
    assert request(pp2_header())['status'] == 400

    assert 'success' in listener(None)
    assert client.get(port=8081)['headers']['Remote-Addr'] == '127.0.0.1'


# Valid headers.


def test_proxy_protocol_v2_tcp4():
    resp = request(pp2_header(src='203.0.113.7', src_port=51234))
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '203.0.113.7'
    assert resp['headers']['Url-Scheme'] == 'http'


def test_proxy_protocol_v2_tcp6():
    # The announced family is independent of the listener's own family.
    resp = request(pp2_header(src='2001:db8::7', dst='2001:db8::1'))
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '2001:db8::7'

    resp = request(pp2_header(src='::1', dst='::1', src_port=1))
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '::1'


def test_proxy_protocol_v2_ipv6_listener():
    if not ipv6_available():
        pytest.skip('no IPv6 on this host')

    assert 'success' in client.conf(
        {
            'pass': 'applications/forwarded_header',
            'proxy_protocol': 'v2',
        },
        'listeners/[::1]:8082',
    )

    resp = request(pp2_header(src='203.0.113.9'), sock_type='ipv6')
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '203.0.113.9'

    resp = request(
        pp2_header(src='2001:db8::9', dst='2001:db8::1'), sock_type='ipv6'
    )
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '2001:db8::9'


def test_proxy_protocol_v2_wildcard_local_addr():
    # On a wildcard listener the local address is resolved before the
    # announced address of another family replaces the peer.
    if not ipv6_available():
        pytest.skip('no IPv6 on this host')

    assert 'success' in client.conf(
        {
            'listeners': {
                '[::]:8082': {'pass': 'routes', 'proxy_protocol': 'v2'}
            },
            'routes': [
                {
                    'match': {'destination': '[::1]:8082'},
                    'action': {'return': 201},
                },
                {'action': {'return': 203}},
            ],
            'applications': {},
        }
    )

    resp = request(pp2_header(src='203.0.113.9'), sock_type='ipv6')
    assert resp['status'] == 201, 'local address of an IPv4 header'


def test_proxy_protocol_v2_ssl_tlv():
    resp = request(pp2_header(tlvs=pp2_ssl_tlv()))
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '192.0.2.10'
    assert resp['headers']['Url-Scheme'] == 'https', 'PP2_CLIENT_SSL'

    resp = request(pp2_header(tlvs=pp2_ssl_tlv(client=0)))
    assert resp['headers']['Url-Scheme'] == 'http', 'no client bit'

    # Other TLVs before and after are skipped by their length.
    noop = struct.pack('!BH', 0x04, 3) + b'abc'
    unknown = struct.pack('!BH', 0xF0, 2) + b'xy'
    resp = request(pp2_header(tlvs=noop + pp2_ssl_tlv() + unknown))
    assert resp['status'] == 200
    assert resp['headers']['Url-Scheme'] == 'https'


def test_proxy_protocol_v2_local():
    # A LOCAL command, as a health check sends: the real peer is kept.
    resp = request(pp2_header(cmd=PP2_LOCAL, src='203.0.113.7'))
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '127.0.0.1'

    header = PP2_SIGNATURE + struct.pack('!BBH', PP2_LOCAL, 0x00, 0)
    resp = request(header)
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '127.0.0.1'


def test_proxy_protocol_v2_split():
    # The header arrives in pieces, with a pause between them.
    header = pp2_header(src='203.0.113.7', tlvs=pp2_ssl_tlv())

    sock = connect()
    sock.sendall(header[:5])
    time.sleep(0.2)
    sock.sendall(header[5:20])
    time.sleep(0.2)
    sock.sendall(header[20:])
    time.sleep(0.2)
    sock.sendall(REQUEST)

    resp = client._resp_to_dict(recv_all(sock).decode())
    sock.close()

    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '203.0.113.7'
    assert resp['headers']['Url-Scheme'] == 'https'


def test_proxy_protocol_v2_keepalive():
    # The announced address holds for every request on the connection.
    sock = connect()
    sock.sendall(
        pp2_header(src='203.0.113.7', tlvs=pp2_ssl_tlv())
        + b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n'
    )

    resp = client.recvall(sock, framed=True).decode()
    assert 'Remote-Addr: 203.0.113.7' in resp
    assert 'Url-Scheme: https' in resp

    sock.sendall(REQUEST)
    resp = recv_all(sock).decode()
    sock.close()

    assert 'Remote-Addr: 203.0.113.7' in resp
    assert 'Url-Scheme: https' in resp


def test_proxy_protocol_v1():
    assert 'success' in listener('v1')

    resp = request(pp1_header(src='203.0.113.7', src_port=51234))
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '203.0.113.7'
    assert resp['headers']['Url-Scheme'] == 'http'

    resp = request(pp1_header(src='2001:db8::7', dst='2001:db8::1'))
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '2001:db8::7'

    resp = request(b'PROXY UNKNOWN\r\n')
    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '127.0.0.1', 'UNKNOWN keeps peer'

    # The configured version is the only one accepted.
    assert_dropped(pp2_header())


def test_proxy_protocol_access_log(wait_for_record):
    assert 'success' in client.conf(
        {
            'path': f'{option.temp_dir}/access.log',
            'format': '$remote_addr "$request_line" $status',
        },
        'access_log',
    ), 'access_log configure'

    resp = request(pp2_header(src='203.0.113.7'))
    assert resp['status'] == 200

    assert (
        wait_for_record(r'^203\.0\.113\.7 "GET / HTTP/1.1" 200$', 'access.log')
        is not None
    ), 'announced address in the access log'

    assert 'success' in listener('v1')

    resp = request(pp1_header(src='2001:db8::7', dst='2001:db8::1'))
    assert resp['status'] == 200

    assert (
        wait_for_record(r'^2001:db8::7 "GET / HTTP/1.1" 200$', 'access.log')
        is not None
    ), 'announced v1 address in the access log'


def test_proxy_protocol_routes_source():
    assert 'success' in client.conf(
        {
            'listeners': {
                '127.0.0.1:8081': {
                    'pass': 'routes',
                    'proxy_protocol': 'v2',
                }
            },
            'routes': [
                {
                    'match': {'source': '203.0.113.0/24'},
                    'action': {'return': 201},
                },
                {
                    'match': {'scheme': 'https'},
                    'action': {'return': 202},
                },
                {'action': {'return': 203}},
            ],
        }
    ), 'routes configure'

    assert request(pp2_header(src='203.0.113.7'))['status'] == 201
    assert request(pp2_header(src='198.51.100.7'))['status'] == 203
    assert (
        request(pp2_header(src='198.51.100.7', tlvs=pp2_ssl_tlv()))['status']
        == 202
    ), 'scheme match sees PP2_CLIENT_SSL'

    # LOCAL keeps the real peer, which is not in the subnet.
    assert request(pp2_header(cmd=PP2_LOCAL, src='203.0.113.7'))['status'] == 203


def test_proxy_protocol_forwarded():
    # The "forwarded" trust check sees the announced address as the peer.
    assert 'success' in listener(
        'v2',
        forwarded={
            'client_ip': 'X-Forwarded-For',
            'protocol': 'X-Forwarded-Proto',
            'source': '203.0.113.0/24',
        },
    )

    req = (
        b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n'
        b'X-Forwarded-For: 198.51.100.200\r\nX-Forwarded-Proto: https\r\n\r\n'
    )

    resp = request(pp2_header(src='203.0.113.7'), request=req)
    assert resp['headers']['Remote-Addr'] == '198.51.100.200', 'trusted'
    assert resp['headers']['Url-Scheme'] == 'https', 'trusted proto'

    resp = request(pp2_header(src='192.0.2.7'), request=req)
    assert resp['headers']['Remote-Addr'] == '192.0.2.7', 'untrusted'
    assert resp['headers']['Url-Scheme'] == 'http', 'untrusted proto'


# TLS listener.


def test_proxy_protocol_tls():
    client.certificate()

    assert 'success' in listener('v2', tls=True), 'tls listener configure'

    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE

    # The header goes first, in clear, then the ClientHello.
    sock = connect()
    sock.sendall(pp2_header(src='203.0.113.7'))

    ssock = context.wrap_socket(sock, server_hostname='localhost')
    ssock.sendall(REQUEST)
    resp = client._resp_to_dict(recv_all(ssock).decode())
    ssock.close()

    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '203.0.113.7'
    assert resp['headers']['Url-Scheme'] == 'https'

    # A LOCAL header, then TLS.
    sock = connect()
    sock.sendall(pp2_header(cmd=PP2_LOCAL))

    ssock = context.wrap_socket(sock, server_hostname='localhost')
    ssock.sendall(REQUEST)
    resp = client._resp_to_dict(recv_all(ssock).decode())
    ssock.close()

    assert resp['status'] == 200
    assert resp['headers']['Remote-Addr'] == '127.0.0.1'

    # A ClientHello without the header is dropped before the handshake.
    sock = connect()

    with pytest.raises((ssl.SSLError, ConnectionError, OSError)):
        context.wrap_socket(sock, server_hostname='localhost')

    sock.close()

    # A plain request without the header is dropped too, with no 400.
    assert_dropped(b'')

    # And the listener still serves.
    sock = connect()
    sock.sendall(pp2_header(src='203.0.113.8'))

    ssock = context.wrap_socket(sock, server_hostname='localhost')
    ssock.sendall(REQUEST)
    resp = client._resp_to_dict(recv_all(ssock).decode())
    ssock.close()

    assert resp['headers']['Remote-Addr'] == '203.0.113.8'


# Missing and broken headers.


def test_proxy_protocol_missing(search_in_file, findall):
    # A plain request where a header is expected.
    assert_dropped(b'')

    # A v1 line where v2 is expected.
    assert_dropped(pp1_header())

    # A ClientHello where a header is expected.
    assert_dropped(b'\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03' + b'\x00' * 40)

    assert_serves()

    assert (
        search_in_file(
            r'\[info\].*PROXY protocol v2 header is missing or invalid, '
            r'the connection from 127\.0\.0\.1:\d+ is dropped'
        )
        is not None
    ), 'drop is logged'

    assert len(findall(r'PROXY protocol v2 header is missing')) == 3, (
        'one line per dropped connection'
    )


def test_proxy_protocol_missing_v1():
    assert 'success' in listener('v1')

    assert_dropped(b'')
    assert_dropped(pp2_header())

    resp = request(pp1_header(src='203.0.113.7'))
    assert resp['headers']['Remote-Addr'] == '203.0.113.7', 'still serves'


@pytest.mark.parametrize(
    'name, header',
    [
        ('bad signature', b'\r\n\r\n\x00\r\nQUIT\r' + pp2_header()[12:]),
        ('version 1 nibble', pp2_header(cmd=0x11)),
        ('version 3 nibble', pp2_header(cmd=0x31)),
        ('command 2', pp2_header(cmd=0x22)),
        ('af_unspec', PP2_SIGNATURE + struct.pack('!BBH', PP2_PROXY, 0x00, 0)),
        (
            'af_unix',
            PP2_SIGNATURE
            + struct.pack('!BBH', PP2_PROXY, 0x31, 216)
            + b'\x00' * 216,
        ),
        ('udp4', pp2_header(family=0x12)),
        ('udp6', pp2_header(src='2001:db8::7', dst='2001:db8::1', family=0x22)),
        ('tcp4 length 11', pp2_header(length=11)),
        ('tcp6 with tcp4 length', pp2_header(family=PP2_TCP6)),
        ('tcp4 with tcp6 length', pp2_header(length=36)),
        ('tlv cut', pp2_header(tlvs=b'\x20\x00')),
        ('tlv overrun', pp2_header(tlvs=b'\x04\x00\x05')),
        ('ssl tlv short', pp2_header(tlvs=b'\x20\x00\x02\x01\x00')),
        ('length above the cap', pp2_header(length=1009)),
        ('length 65535', pp2_header(length=65535)),
    ],
)
def test_proxy_protocol_broken(name, header):
    assert_dropped(header)
    assert_serves()


def test_proxy_protocol_broken_partial_address():
    # A header cut inside the address block never applies a partial
    # address: the proxy closes, and so does the router, with no response.
    header = pp2_header(src='203.0.113.7')

    sock = connect()
    sock.sendall(header[:20])
    sock.shutdown(socket.SHUT_WR)

    assert recv_all(sock) == b''
    sock.close()

    assert_serves()


@pytest.mark.parametrize(
    'line',
    [
        b'PROXY TCP4 1.2.3.4 5.6.7.8 1\r\n',
        b'PROXY TCP4 1.2.3.4 5.6.7.8 1 2 3\r\n',
        b'PROXY TCP4 1.2.3.4  5.6.7.8 1 2\r\n',
        b'PROXY TCP4 1.2.3.4 5.6.7.8 1 2 \r\n',
        b'PROXY TCP4 1.2.3 5.6.7.8 1 2\r\n',
        b'PROXY TCP4 ::1 5.6.7.8 1 2\r\n',
        b'PROXY TCP6 1.2.3.4 ::1 1 2\r\n',
        b'PROXY TCP4 1.2.3.4 5.6.7.8 65536 2\r\n',
        b'PROXY TCP4 1.2.3.4 5.6.7.8 01 2\r\n',
        b'PROXY UDP4 1.2.3.4 5.6.7.8 1 2\r\n',
        b'PROXY tcp4 1.2.3.4 5.6.7.8 1 2\r\n',
        b'PROXY UNKNOWNX\r\n',
        b'PROXY TCP4 ' + b'1' * 100,
    ],
)
def test_proxy_protocol_broken_v1(line):
    assert 'success' in listener('v1')

    assert_dropped(line)

    resp = request(pp1_header(src='203.0.113.7'))
    assert resp['headers']['Remote-Addr'] == '203.0.113.7', 'still serves'


def test_proxy_protocol_tls_timer(search_in_file):
    # After the header the read timer belongs to the TLS idle state.
    client.certificate()

    assert 'success' in client.conf({'http': {'idle_timeout': 2}}, 'settings')
    assert 'success' in listener('v2', tls=True)

    sock = connect()
    sock.settimeout(10)
    sock.sendall(pp2_header(src='203.0.113.7'))
    data = recv_all(sock)
    sock.close()

    # The idle timeout of the TLS idle state answers 408, as it does on a
    # TLS listener without the option; the PROXY timeout sends nothing.
    assert data.startswith(b'HTTP/1.1 408'), 'closed by the idle timeout'
    assert (
        search_in_file(r'PROXY protocol header did not arrive') is None
    ), 'not the PROXY timeout'


def test_proxy_protocol_timeout(search_in_file):
    assert 'success' in client.conf({'http': {'idle_timeout': 2}}, 'settings')

    # Nothing at all, and then a partial header: both wait for the
    # listener's idle timeout and are then closed without a response.
    sock = connect()
    sock.settimeout(10)
    assert recv_all(sock) == b''
    sock.close()

    sock = connect()
    sock.settimeout(10)
    sock.sendall(pp2_header()[:10])
    assert recv_all(sock) == b''
    sock.close()

    assert (
        search_in_file(
            r'\[info\].*PROXY protocol header did not arrive within the idle '
            r'timeout, the connection from 127\.0\.0\.1:\d+ is dropped'
        )
        is not None
    ), 'timeout is logged'

    assert_serves()
