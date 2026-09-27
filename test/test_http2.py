import os
import shutil
import socket
import ssl
import subprocess

import pytest

from unit.applications.tls import ApplicationTLS
from unit.option import option

# HTTP/2 over TLS (ADR 0005, phase 1).  The client is the pure-Python "h2"
# package when it is installed; otherwise the nghttp CLI drives the tests
# that need only a status and a body, and the ones that need raw frames are
# skipped.

try:
    import h2.config
    import h2.connection
    import h2.events

    HAVE_H2 = True

except ImportError:  # pragma: no cover
    HAVE_H2 = False

NGHTTP = shutil.which('nghttp')

prerequisites = {'modules': {'python': 'any', 'openssl': 'any', 'http2': 'any'}}

client = ApplicationTLS()

pytestmark = pytest.mark.skipif(
    not HAVE_H2 and NGHTTP is None, reason='neither the h2 package nor nghttp'
)


def setup_listener(pass_to, http2=True, cert='default'):
    tls = {'certificate': cert}

    if http2:
        tls['http2'] = True

    assert 'success' in client.conf(
        {'pass': pass_to, 'tls': tls}, 'listeners/*:8080'
    )


def load_share():
    share = f'{option.temp_dir}/share'
    os.makedirs(share, exist_ok=True)

    with open(f'{share}/index.html', 'w', encoding='utf-8') as f:
        f.write('hello h2')

    # Larger than one DATA frame and than the h2 read buffer.
    with open(f'{share}/big.txt', 'wb') as f:
        f.write(os.urandom(150000).hex().encode())

    assert 'success' in client.conf(
        {
            'listeners': {'*:8080': {'pass': 'routes'}},
            'routes': [{'action': {'share': f'{share}$uri'}}],
            'applications': {},
        }
    )

    client.certificate()
    setup_listener('routes')

    return share


def load_app(name):
    client.load(name)
    client.certificate()
    setup_listener(f'applications/{name}')


def ssl_context(alpn=('h2',)):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    if alpn:
        ctx.set_alpn_protocols(list(alpn))

    return ctx


class H2Client:
    """A minimal HTTP/2 client on the h2 package: one TLS connection,
    several streams."""

    def __init__(self, port=8080, alpn=('h2',)):
        raw = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.sock = ssl_context(alpn).wrap_socket(
            raw, server_hostname='localhost'
        )
        self.alpn = self.sock.selected_alpn_protocol()

        config = h2.config.H2Configuration(
            client_side=True,
            validate_inbound_headers=False,
            validate_outbound_headers=False,
            normalize_outbound_headers=False,
        )
        self.conn = h2.connection.H2Connection(config=config)
        self.conn.initiate_connection()
        self.flush()

        self.streams = {}
        self.goaway = None

    def close(self):
        self.sock.close()

    def flush(self):
        data = self.conn.data_to_send()

        if data:
            self.sock.sendall(data)

    def send(
        self, method='GET', path='/', headers=None, body=None, authority='localhost'
    ):
        sid = self.conn.get_next_available_stream_id()

        hdrs = [(':method', method), (':path', path), (':scheme', 'https')]

        if authority is not None:
            hdrs.append((':authority', authority))

        hdrs.extend(headers or [])

        self.streams[sid] = {
            'status': None,
            'headers': {},
            'body': b'',
            'done': False,
            'reset': None,
        }

        self.conn.send_headers(sid, hdrs, end_stream=body is None)
        self.flush()

        if body is not None:
            self._send_body(sid, body)

        return sid

    def _send_body(self, sid, body):
        pos = 0

        while pos < len(body):
            window = min(
                self.conn.local_flow_control_window(sid),
                self.conn.max_outbound_frame_size,
            )

            if window == 0:
                self._recv_once()
                continue

            chunk = body[pos : pos + window]
            pos += len(chunk)

            self.conn.send_data(sid, chunk, end_stream=pos == len(body))
            self.flush()

        if len(body) == 0:
            self.conn.end_stream(sid)
            self.flush()

    def _recv_once(self):
        data = self.sock.recv(65536)

        if not data:
            raise ConnectionError('connection closed')

        for event in self.conn.receive_data(data):
            if isinstance(event, h2.events.ResponseReceived):
                stream = self.streams[event.stream_id]

                for name, value in event.headers:
                    name = name.decode() if isinstance(name, bytes) else name
                    value = (
                        value.decode() if isinstance(value, bytes) else value
                    )

                    if name == ':status':
                        stream['status'] = int(value)
                    else:
                        stream['headers'][name] = value

            elif isinstance(event, h2.events.DataReceived):
                self.streams[event.stream_id]['body'] += event.data
                self.conn.acknowledge_received_data(
                    event.flow_controlled_length, event.stream_id
                )

            elif isinstance(event, h2.events.StreamEnded):
                self.streams[event.stream_id]['done'] = True

            elif isinstance(event, h2.events.StreamReset):
                self.streams[event.stream_id]['reset'] = event.error_code
                self.streams[event.stream_id]['done'] = True

            elif isinstance(event, h2.events.ConnectionTerminated):
                self.goaway = event.error_code

                for stream in self.streams.values():
                    stream['done'] = True

        self.flush()

    def wait(self, sid):
        stream = self.streams[sid]

        while not stream['done']:
            self._recv_once()

        return stream

    def get(self, path='/', **kwargs):
        return self.wait(self.send('GET', path, **kwargs))

    def post(self, path='/', body=b'', **kwargs):
        return self.wait(self.send('POST', path, body=body, **kwargs))


def nghttp_get(path, method='GET', body=None):
    """The nghttp CLI fallback: returns a dict like H2Client.wait()."""

    url = f'https://127.0.0.1:8080{path}'
    args = [NGHTTP]

    if body is not None:
        # "-d" takes a file: nghttp sends it as the request body (POST).
        body_file = f'{option.temp_dir}/nghttp-body'

        with open(body_file, 'wb') as f:
            f.write(body)

        args += ['-d', body_file]

    out = subprocess.run(
        args + ['-nv', url], capture_output=True, check=False
    ).stdout.decode()

    status = None
    for line in out.splitlines():
        if ':status:' in line and 'recv' in line:
            status = int(line.split(':status:')[1].strip())

    data = subprocess.run(args + [url], capture_output=True, check=False).stdout

    return {'status': status, 'body': data, 'headers': {}, 'reset': None}


def h2_get(path):
    if HAVE_H2:
        c = H2Client()
        assert c.alpn == 'h2'
        resp = c.get(path)
        c.close()
        return resp

    return nghttp_get(path)


def test_http2_static_get():
    load_share()

    resp = h2_get('/index.html')

    assert resp['status'] == 200
    assert resp['body'] == b'hello h2'


def test_http2_static_large():
    share = load_share()

    with open(f'{share}/big.txt', 'rb') as f:
        expect = f.read()

    resp = h2_get('/big.txt')

    assert resp['status'] == 200
    assert resp['body'] == expect


def test_http2_static_not_found():
    load_share()

    assert h2_get('/nope')['status'] == 404


def test_http2_python_get():
    load_app('empty')

    resp = h2_get('/')

    assert resp['status'] == 200
    assert resp['body'] == b''


def test_http2_python_environment():
    load_app('variables')

    if not HAVE_H2:
        pytest.skip('needs the h2 package')

    c = H2Client()
    resp = c.get(
        '/path/here?arg=1',
        headers=[('custom-header', 'blah'), ('content-type', 'text/html')],
    )
    c.close()

    assert resp['status'] == 200
    assert resp['headers']['request-method'] == 'GET'
    assert resp['headers']['request-uri'] == '/path/here?arg=1'
    assert resp['headers']['http-host'] == 'localhost'
    assert resp['headers']['server-protocol'] == 'HTTP/2.0'
    assert resp['headers']['custom-header'] == 'blah'


@pytest.mark.parametrize('size', [10, 100000], ids=['memory', 'temp-file'])
def test_http2_python_post(size):
    load_app('mirror')

    body = os.urandom(size // 2).hex().encode()[:size]

    if HAVE_H2:
        c = H2Client()
        resp = c.post('/', body=body)
        c.close()

    else:
        if size > 100:
            pytest.skip('needs the h2 package')

        resp = nghttp_get('/', body=body)

    assert resp['status'] == 200
    assert resp['body'] == body


def test_http2_concurrent_streams():
    if not HAVE_H2:
        pytest.skip('needs the h2 package')

    share = load_share()

    with open(f'{share}/big.txt', 'rb') as f:
        big = f.read()

    c = H2Client()

    sids = []
    for _ in range(4):
        sids.append(c.send('GET', '/big.txt'))
        sids.append(c.send('GET', '/index.html'))
        sids.append(c.send('GET', '/missing'))

    results = [c.wait(sid) for sid in sids]
    c.close()

    for i, resp in enumerate(results):
        kind = i % 3

        if kind == 0:
            assert resp['status'] == 200
            assert resp['body'] == big

        elif kind == 1:
            assert resp['status'] == 200
            assert resp['body'] == b'hello h2'

        else:
            assert resp['status'] == 404


def test_http2_alpn_fallback_http1():
    load_app('empty')

    ctx = ssl_context(alpn=('http/1.1',))
    resp = client.get_ssl(context=ctx)

    assert resp['status'] == 200

    # A client that does not offer ALPN at all is an HTTP/1 client too.
    resp = client.get_ssl(context=ssl_context(alpn=None))

    assert resp['status'] == 200

    raw = socket.create_connection(('127.0.0.1', 8080), timeout=5)
    sock = ssl_context(alpn=('http/1.1',)).wrap_socket(
        raw, server_hostname='localhost'
    )
    assert sock.selected_alpn_protocol() == 'http/1.1'
    sock.close()


def test_http2_option_off():
    client.load('empty')
    client.certificate()
    setup_listener('applications/empty', http2=False)

    # "h2" is offered but the listener has no http2 option, so the server
    # does not take part in ALPN at all, exactly as before the option.
    raw = socket.create_connection(('127.0.0.1', 8080), timeout=5)
    sock = ssl_context(alpn=('h2', 'http/1.1')).wrap_socket(
        raw, server_hostname='localhost'
    )
    assert sock.selected_alpn_protocol() is None
    sock.close()

    resp = client.get_ssl(context=ssl_context(alpn=('h2', 'http/1.1')))

    assert resp['status'] == 200


def test_http2_bad_authority():
    if not HAVE_H2:
        pytest.skip('needs the h2 package')

    load_app('empty')

    c = H2Client()

    # Accepted by nghttp2's character check, refused by
    # nxt_http_validate_host() through the Host handler: 400 as in HTTP/1.
    assert c.get('/', authority='a..b')['status'] == 400

    # Refused by nghttp2's own check before it reaches the router.
    for authority in ['bad host', 'example.com/path']:
        resp = c.get('/', authority=authority)
        assert resp['reset'] is not None or resp['status'] == 400, authority

    assert c.get('/', authority='localhost')['status'] == 200
    c.close()


def test_http2_host_mismatch():
    if not HAVE_H2:
        pytest.skip('needs the h2 package')

    load_app('empty')

    c = H2Client()

    # RFC 9113, 8.3.1: "host" must agree with ":authority".
    resp = c.get('/', headers=[('host', 'other')])
    assert resp['reset'] is not None

    resp = c.get('/', headers=[('host', 'localhost')])
    assert resp['status'] == 200

    c.close()


def test_http2_bad_path():
    if not HAVE_H2:
        pytest.skip('needs the h2 package')

    load_app('empty')

    c = H2Client()

    # nghttp2's HTTP messaging layer resets these before the router sees
    # them; the connection and the next request are unaffected.
    for path in ['', 'nope', 'http://localhost/']:
        resp = c.get(path)
        assert resp['reset'] is not None or resp['status'] == 400, path

    # Dot segments and %XX are normalized as the h1 parser does.
    assert c.get('/../../')['status'] == 400
    assert c.get('/a/../')['status'] == 200
    assert c.get('/%2e/')['status'] == 200
    assert c.get('/')['status'] == 200
    c.close()


def test_http2_header_list_too_large():
    if not HAVE_H2:
        pytest.skip('needs the h2 package')

    load_app('empty')

    c = H2Client()

    # Over large_header_buffer_size * large_header_buffers (32 KiB).
    headers = [(f'x-{i}', 'v' * 1000) for i in range(40)]
    assert c.get('/', headers=headers)['status'] == 431

    assert c.get('/')['status'] == 200
    c.close()


def test_http2_config_invalid():
    client.load('empty')
    client.certificate()

    assert 'error' in client.conf(
        {'pass': 'applications/empty', 'tls': {'certificate': 'default', 'http2': 'yes'}},
        'listeners/*:8080',
    )

    assert 'error' in client.conf(
        {'pass': 'applications/empty', 'http2': True},
        'listeners/*:8080',
    )
