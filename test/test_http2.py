import os
import re
import shutil
import socket
import ssl
import subprocess
import time

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
    import hpack
    from hyperframe.frame import (
        ContinuationFrame,
        DataFrame,
        Frame,
        GoAwayFrame,
        HeadersFrame,
        PingFrame,
        RstStreamFrame,
        SettingsFrame,
        WindowUpdateFrame,
    )

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
            'informational': [],
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

            elif isinstance(event, h2.events.InformationalResponseReceived):
                self.streams[event.stream_id]['informational'].append(
                    event.headers
                )

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


# Stage 2: raw frames, limits and the progress timer.

PREFACE = b'PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n'

NO_ERROR = 0x0
PROTOCOL_ERROR = 0x1
REFUSED_STREAM = 0x7
ENHANCE_YOUR_CALM = 0xB


def need_h2():
    if not HAVE_H2:
        pytest.skip('needs the h2 package')


def load_conf(conf):
    """A full configuration whose "*:8080" listener speaks h2 over TLS."""

    client.certificate()

    conf['listeners']['*:8080']['tls'] = {
        'certificate': 'default',
        'http2': True,
    }
    conf.setdefault('applications', {})

    assert 'success' in client.conf(conf)


def python_app(name):
    path = f'{option.test_dir}/python/{name}'

    return {
        'type': 'python',
        'processes': {'spare': 0},
        'path': path,
        'working_directory': path,
        'module': 'wsgi',
    }


def load_mirror(settings=None):
    conf = {
        'listeners': {'*:8080': {'pass': 'applications/mirror'}},
        'applications': {'mirror': python_app('mirror')},
    }

    if settings is not None:
        conf['settings'] = {'http': settings}

    load_conf(conf)


def load_return(settings=None):
    conf = {
        'listeners': {'*:8080': {'pass': 'routes'}},
        'routes': [{'action': {'return': 200}}],
    }

    if settings is not None:
        conf['settings'] = {'http': settings}

    load_conf(conf)


class RawH2:
    """HTTP/2 frames written and read one by one, for what the h2 package
    refuses to send: stalls, floods, and limits the server announces."""

    def __init__(self, port=8080, ack_settings=True):
        raw = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.sock = ssl_context().wrap_socket(raw, server_hostname='localhost')
        assert self.sock.selected_alpn_protocol() == 'h2'

        self.ack_settings = ack_settings
        self.encoder = hpack.Encoder()
        self.decoder = hpack.Decoder()

        self.buf = b''
        self.closed = False
        self.goaway = None
        self.frames = []
        self.status = {}
        self.data = {}
        self.ended = set()
        self.rst = {}
        self.server_settings = {}
        self.server_settings_seen = False

        self.sock.sendall(PREFACE + SettingsFrame(0).serialize())

        # Settle the SETTINGS exchange first: a SETTINGS ACK sent later
        # could land inside a header block a test leaves open.
        assert self.wait(lambda: self.server_settings_seen)

    def close(self):
        self.sock.close()

    def send(self, *frames):
        self.sock.sendall(b''.join(f.serialize() for f in frames))

    def block(self, method='GET', path='/', headers=(), authority='localhost'):
        return self.encoder.encode(
            [
                (':method', method),
                (':path', path),
                (':scheme', 'https'),
                (':authority', authority),
            ]
            + list(headers)
        )

    def headers(self, sid, block, end_stream=True, end_headers=True):
        flags = []

        if end_stream:
            flags.append('END_STREAM')

        if end_headers:
            flags.append('END_HEADERS')

        return HeadersFrame(sid, data=block, flags=flags)

    def request(self, sid, end_stream=True, **kwargs):
        self.send(self.headers(sid, self.block(**kwargs), end_stream))

    def data_frame(self, sid, data, end_stream=False):
        return DataFrame(
            sid, data=data, flags=['END_STREAM'] if end_stream else []
        )

    def _read(self, timeout):
        self.sock.settimeout(max(timeout, 0.01))

        try:
            chunk = self.sock.recv(65536)

        except (socket.timeout, TimeoutError):
            return

        except (ConnectionError, ssl.SSLError, OSError):
            self.closed = True
            return

        if not chunk:
            self.closed = True
            return

        self.buf += chunk

        while len(self.buf) >= 9:
            frame, length = Frame.parse_frame_header(memoryview(self.buf[:9]))

            if len(self.buf) < 9 + length:
                break

            frame.parse_body(memoryview(self.buf[9 : 9 + length]))
            self.buf = self.buf[9 + length :]

            self._handle(frame)

    def _handle(self, frame):
        self.frames.append(frame)
        sid = frame.stream_id

        if isinstance(frame, SettingsFrame):
            if 'ACK' not in frame.flags:
                self.server_settings.update(frame.settings)
                self.server_settings_seen = True

                if self.ack_settings:
                    self.send(SettingsFrame(0, flags=['ACK']))

        elif isinstance(frame, HeadersFrame):
            for name, value in self.decoder.decode(frame.data):
                if name in (':status', b':status'):
                    self.status[sid] = int(value)

        elif isinstance(frame, DataFrame):
            self.data[sid] = self.data.get(sid, b'') + frame.data

            if frame.flow_controlled_length:
                self.send(
                    WindowUpdateFrame(
                        0, window_increment=frame.flow_controlled_length
                    ),
                )

        elif isinstance(frame, RstStreamFrame):
            self.rst[sid] = frame.error_code

        elif isinstance(frame, GoAwayFrame):
            self.goaway = (frame.error_code, frame.last_stream_id)

        elif isinstance(frame, PingFrame) and 'ACK' not in frame.flags:
            self.send(
                PingFrame(0, opaque_data=frame.opaque_data, flags=['ACK'])
            )

        if 'END_STREAM' in getattr(frame, 'flags', ()):
            self.ended.add(sid)

    def wait(self, cond, timeout=10):
        end = time.monotonic() + timeout

        while not cond() and not self.closed:
            left = end - time.monotonic()

            if left <= 0:
                break

            self._read(left)

        return cond()

    def wait_closed(self, timeout=10):
        return self.wait(lambda: self.closed, timeout)

    def wait_response(self, sid, timeout=10):
        self.wait(lambda: sid in self.ended or sid in self.rst, timeout)
        return self.status.get(sid)

    def rst_after_end(self, sid, timeout=1):
        """The RST_STREAM that follows a response, if nghttp2 sends one."""

        self.wait(lambda: sid in self.rst, timeout)
        return self.rst.get(sid)


def assert_serves():
    """A fresh connection is served: other clients are unaffected."""

    c = H2Client()
    assert c.get('/')['status'] == 200
    c.close()


def test_http2_timeout_stall_headers():
    need_h2()

    # body_read_timeout stays 30 s: a header block has header_read_timeout.
    load_return({'header_read_timeout': 2})

    stalled = RawH2()

    # HEADERS without END_HEADERS: the CONTINUATION never comes.
    stalled.send(
        stalled.headers(1, stalled.block(), end_stream=True, end_headers=False)
    )
    start = time.monotonic()

    assert_serves()

    assert stalled.wait_closed(10)
    elapsed = time.monotonic() - start

    assert 1.5 < elapsed < 6, elapsed
    assert stalled.goaway is not None
    assert stalled.goaway[0] == NO_ERROR
    assert 1 not in stalled.status
    stalled.close()

    assert_serves()


def test_http2_frame_inside_header_block():
    need_h2()
    load_return()

    c = RawH2()

    # Only CONTINUATION may follow a HEADERS frame without END_HEADERS
    # (RFC 9113, 6.10): a connection error, and the request that has begun
    # is dropped with the connection instead of holding it open.
    c.send(
        c.headers(1, c.block(), end_stream=True, end_headers=False),
        PingFrame(0, opaque_data=b'12345678'),
    )

    assert c.wait_closed(5)
    assert c.goaway is not None
    assert c.goaway[0] == PROTOCOL_ERROR
    c.close()

    assert_serves()


def test_http2_timeout_stall_body():
    need_h2()

    # header_read_timeout stays 30 s: a body has body_read_timeout.
    load_mirror({'body_read_timeout': 2})

    stalled = RawH2()
    stalled.request(
        1, end_stream=False, method='POST', headers=[('content-length', '10')]
    )
    stalled.send(stalled.data_frame(1, b'12345'))
    start = time.monotonic()

    # PING and WINDOW_UPDATE do not advance a stream and do not keep the
    # connection.
    for _ in range(4):
        stalled.send(
            PingFrame(0, opaque_data=b'12345678'),
            WindowUpdateFrame(0, window_increment=1),
        )
        assert_serves()
        time.sleep(0.4)

    assert stalled.wait_closed(10)
    elapsed = time.monotonic() - start

    assert 1.5 < elapsed < 6, elapsed
    assert stalled.goaway is not None
    assert stalled.goaway[0] == NO_ERROR
    assert stalled.goaway[1] == 1
    stalled.close()

    assert_serves()


def test_http2_timeout_progress():
    need_h2()
    load_mirror({'body_read_timeout': 2})

    c = RawH2()
    c.request(
        1, end_stream=False, method='POST', headers=[('content-length', '5')]
    )

    # One byte a second: every DATA frame is progress, so the stream
    # outlives body_read_timeout as a whole.
    for byte in b'1234':
        time.sleep(1)
        c.send(c.data_frame(1, bytes([byte])))

    c.send(c.data_frame(1, b'5', end_stream=True))

    assert c.wait_response(1) == 200
    assert c.data[1] == b'12345'
    assert not c.closed

    c.request(3)
    assert c.wait_response(3) == 200
    c.close()


def test_http2_timeout_empty_data():
    need_h2()
    load_mirror({'body_read_timeout': 2})

    # An empty DATA frame with END_STREAM still ends a body.
    c = RawH2()
    c.request(
        1, end_stream=False, method='POST', headers=[('content-length', '5')]
    )
    c.send(c.data_frame(1, b'12345'), c.data_frame(1, b'', end_stream=True))

    assert c.wait_response(1) == 200
    assert c.data[1] == b'12345'
    c.close()

    stalled = RawH2()
    stalled.request(
        1, end_stream=False, method='POST', headers=[('content-length', '10')]
    )
    stalled.send(stalled.data_frame(1, b'12345'))
    start = time.monotonic()

    # Empty and padding-only DATA frames carry no body bytes: they are not
    # progress, so they do not hold the stalled body past its timeout.
    empty = [
        stalled.data_frame(1, b''),
        DataFrame(1, data=b'', flags=['PADDED'], pad_length=8),
    ]
    n = 0

    while not stalled.closed and time.monotonic() - start < 8:
        try:
            stalled.send(empty[n % 2])

        except (ConnectionError, ssl.SSLError, OSError):
            break

        n += 1
        stalled.wait_closed(0.5)

    assert stalled.wait_closed(5)
    elapsed = time.monotonic() - start

    assert n >= 4, n
    assert 1.5 < elapsed < 4.5, elapsed
    assert stalled.goaway is not None
    assert stalled.goaway[0] == NO_ERROR
    assert stalled.goaway[1] == 1
    stalled.close()

    assert_serves()


def test_http2_timeout_idle():
    need_h2()
    load_return({'idle_timeout': 2, 'body_read_timeout': 30})

    # With no stream open the read timer is idle_timeout, as before the
    # first request.
    c = RawH2()
    c.request(1)
    assert c.wait_response(1) == 200
    start = time.monotonic()

    assert c.wait_closed(10)
    elapsed = time.monotonic() - start

    assert 1.5 < elapsed < 6, elapsed
    assert c.goaway == (NO_ERROR, 1)
    c.close()


def test_http2_timeout_application_wait():
    need_h2()

    conf = {
        'listeners': {'*:8080': {'pass': 'applications/delayed'}},
        'applications': {'delayed': python_app('delayed')},
        'settings': {
            'http': {'header_read_timeout': 1, 'body_read_timeout': 1}
        },
    }
    load_conf(conf)

    # A complete request that waits for its application is not timed.
    c = H2Client()
    resp = c.get('/', headers=[('x-delay', '3')])
    assert resp['status'] == 200
    c.close()


@pytest.mark.parametrize('content_length', [True, False], ids=['cl', 'no-cl'])
def test_http2_max_body_size(content_length):
    need_h2()
    load_mirror({'max_body_size': 1000})

    c = H2Client()

    for size, status in [(1000, 200), (1001, 413), (5000, 413)]:
        body = b'x' * size
        headers = [('content-length', str(size))] if content_length else []

        resp = c.post('/', body=body, headers=headers)

        assert resp['status'] == status, size

        if status == 200:
            assert resp['body'] == body

    # The 413 is a stream error only.
    assert c.get('/')['status'] == 200
    c.close()


@pytest.mark.parametrize('size', [1024, 1025, 70000])
@pytest.mark.parametrize('content_length', [True, False], ids=['cl', 'no-cl'])
def test_http2_body_buffer_size(size, content_length):
    need_h2()
    load_mirror({'body_buffer_size': 1024})

    body = os.urandom(size)
    headers = [('content-length', str(size))] if content_length else []

    c = H2Client()
    resp = c.post('/', body=body, headers=headers)
    c.close()

    assert resp['status'] == 200
    assert resp['body'] == body


def test_http2_content_length_mismatch():
    need_h2()
    load_mirror()

    c = RawH2()

    # Fewer bytes than announced: nghttp2 resets the stream.
    c.request(
        1, end_stream=False, method='POST', headers=[('content-length', '10')]
    )
    c.send(c.data_frame(1, b'12345', end_stream=True))
    c.wait_response(1)
    assert c.rst.get(1) == PROTOCOL_ERROR

    # More bytes than announced: the same.
    c.request(
        3, end_stream=False, method='POST', headers=[('content-length', '2')]
    )
    c.send(c.data_frame(3, b'12345', end_stream=True))
    c.wait_response(3)
    assert c.rst.get(3) == PROTOCOL_ERROR

    c.request(
        5, end_stream=False, method='POST', headers=[('content-length', '3')]
    )
    c.send(c.data_frame(5, b'123', end_stream=True))
    assert c.wait_response(5) == 200
    assert c.data[5] == b'123'
    c.close()


@pytest.mark.parametrize('content_length', [True, False], ids=['cl', 'no-cl'])
def test_http2_response_before_body(content_length):
    need_h2()
    load_mirror({'max_body_size': 1000, 'body_read_timeout': 2})

    c = RawH2()

    # Every action reads the whole body before it runs, so the response
    # that comes before the end of the body is an error: 413 at END_HEADERS
    # for a content-length over max_body_size, or once the stored body
    # grows over it.  The client still owes the rest of the body.  nghttp2
    # leaves the stream half-closed; Unit ends it with RST_STREAM(NO_ERROR)
    # (RFC 9113, 8.1) once the response is written.
    headers = [('content-length', '2000')] if content_length else []

    c.request(1, end_stream=False, method='POST', headers=headers)
    c.send(c.data_frame(1, b'x' * 1500))

    assert c.wait_response(1) == 413
    assert 1 in c.ended
    assert c.rst_after_end(1) == NO_ERROR

    # DATA the client had in flight for the stream is discarded.
    c.send(c.data_frame(1, b'x' * 100))

    # The stream is closed, so the progress timer does not run for it.
    time.sleep(3)
    c.request(3)
    assert c.wait_response(3) == 200
    assert not c.closed
    assert c.goaway is None
    c.close()


@pytest.mark.parametrize(
    'ack_settings', [True, False], ids=['acked', 'not-acked']
)
def test_http2_max_concurrent_streams(ack_settings):
    need_h2()
    load_mirror()

    # SETTINGS_MAX_CONCURRENT_STREAMS is 128.  nghttp2 enforces it as
    # acknowledged by the client, and before the ACK as announced.
    c = RawH2(ack_settings=ack_settings)
    assert c.server_settings[0x3] == 128

    # 128 requests wait for their bodies.
    c.send(
        *[
            c.headers(sid, c.block(method='POST'), end_stream=False)
            for sid in range(1, 257, 2)
        ]
    )

    c.request(257, method='POST')

    if ack_settings:
        # Over the acknowledged limit: a connection error (RFC 9113, 5.1.2).
        assert c.wait_closed()
        assert c.goaway is not None
        assert c.goaway[0] == PROTOCOL_ERROR

    else:
        # Over the limit before the client knows it: the stream is refused.
        c.wait(lambda: 257 in c.rst)
        assert c.rst[257] == REFUSED_STREAM

        # The first 128 are still served.
        c.send(c.data_frame(1, b'abc', end_stream=True))
        assert c.wait_response(1) == 200
        assert c.data[1] == b'abc'

    c.close()


def test_http2_rst_stream_flood():
    need_h2()
    load_return()

    c = RawH2()

    # The "rapid reset" pattern: open a stream and cancel it at once, 1100
    # times.  Each stream is a request, and the connection serves 1000
    # requests (NXT_H2P_MAX_REQUESTS), so the GOAWAY comes from that cap:
    # NO_ERROR with the 1000th stream as the last one.  nghttp2's reset rate
    # limit (a burst of 1000, NXT_H2P_RST_BURST) is not reached first.
    frames = []
    for sid in range(1, 2201, 2):
        frames.append(c.headers(sid, c.block(), end_stream=False))
        frames.append(RstStreamFrame(sid, error_code=0x8))

    try:
        c.send(*frames)
    except OSError:
        pass

    assert c.wait(lambda: c.goaway is not None)
    assert c.goaway == (NO_ERROR, 1999)

    # Nothing above the last stream was served.
    assert all(sid <= 1999 for sid in c.status)
    c.close()

    assert_serves()


def test_http2_continuation_flood():
    need_h2()
    load_return()

    c = RawH2()

    block = c.block(headers=[(f'x-{i}', 'v') for i in range(10)])
    step = len(block) / 10
    parts = [block[round(i * step) : round((i + 1) * step)] for i in range(10)]
    assert len(parts) == 10

    frames = [c.headers(1, parts[0], end_headers=False)]
    for part in parts[1:]:
        frames.append(ContinuationFrame(1, data=part))

    frames[-1].flags.add('END_HEADERS')

    # 9 CONTINUATION frames, one over the limit of 8.
    c.send(*frames)

    assert c.wait_closed()
    assert c.goaway is not None
    assert c.goaway[0] == ENHANCE_YOUR_CALM
    assert 1 not in c.status
    c.close()

    # 8 are accepted.
    c = RawH2()
    frames = [c.headers(1, b''.join(parts[:2]), end_headers=False)]
    for part in parts[2:]:
        frames.append(ContinuationFrame(1, data=part))

    frames[-1].flags.add('END_HEADERS')
    c.send(*frames)

    assert c.wait_response(1) == 200
    c.close()


def test_http2_settings_flood():
    need_h2()
    load_return()

    # 40 entries in one SETTINGS frame, over NXT_H2P_MAX_SETTINGS (32):
    # that is what nghttp2_option_set_max_settings() limits.
    c = RawH2()
    c.send(SettingsFrame(0, settings={0x100 + i: i for i in range(40)}))

    assert c.wait_closed()
    assert c.goaway is not None
    assert c.goaway[0] == ENHANCE_YOUR_CALM
    c.close()

    # 40 separate SETTINGS frames are only acknowledged: nghttp2 limits the
    # ACKs it has queued (1000, for a client that does not read), not the
    # frames.
    def acks():
        return sum(
            1
            for f in c.frames
            if isinstance(f, SettingsFrame) and 'ACK' in f.flags
        )

    c = RawH2()
    c.send(*[SettingsFrame(0, settings={0x4: 65535}) for _ in range(40)])
    c.request(1)

    assert c.wait_response(1) == 200

    # 40, and the one for the SETTINGS of the connection preface.
    assert c.wait(lambda: acks() == 41, 2)
    assert c.goaway is None
    c.close()


def test_http2_long_method():
    need_h2()
    load_app('empty')

    method = 'A' * 300

    # What HTTP/1 answers to the same method.
    h1 = client.get_ssl(
        method=method,
        context=ssl_context(alpn=('http/1.1',)),
    )['status']

    c = H2Client()
    resp = c.send(method, '/')
    resp = c.wait(resp)

    # HTTP/1 takes a 300-byte method, so HTTP/2 does too.
    assert h1 == 200
    assert resp['status'] == h1

    assert c.get('/')['status'] == 200
    c.close()


def test_http2_long_header_name():
    need_h2()
    load_app('empty')

    c = H2Client()

    assert c.get('/', headers=[('x' * 255, 'v')])['status'] == 200
    assert c.get('/', headers=[('x' * 300, 'v')])['status'] == 431
    assert c.get('/')['status'] == 200
    c.close()


def test_http2_expect_continue():
    need_h2()
    load_mirror()

    # As in HTTP/1, "expect: 100-continue" is ignored: no 100 is sent and
    # the final response follows the body.
    c = H2Client()
    resp = c.post(
        '/', body=b'0123456789', headers=[('expect', '100-continue')]
    )
    c.close()

    assert resp['status'] == 200
    assert resp['informational'] == []
    assert resp['body'] == b'0123456789'


def h1_chunked_post(path, body):
    """An HTTP/1.1 request with a chunked body, over TLS."""

    raw = socket.create_connection(('127.0.0.1', 8080), timeout=10)
    sock = ssl_context(alpn=('http/1.1',)).wrap_socket(
        raw, server_hostname='localhost'
    )

    sock.sendall(
        f'POST {path} HTTP/1.1\r\nHost: localhost\r\n'
        'Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n'.encode()
        + f'{len(body):x}\r\n'.encode()
        + body
        + b'\r\n0\r\n\r\n'
    )

    data = b''
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk

    sock.close()

    head, _, content = data.partition(b'\r\n\r\n')

    return int(head.split()[1]), content


def h1_or_h2(proto, path='/', body=None):
    """(status, body) of a GET, or of a POST with a body.  "h2" sends the
    body without content-length, "h2-cl" with it, "h1-chunked" chunked."""

    if proto.startswith('h2'):
        c = H2Client()

        if body is None:
            resp = c.get(path)

        else:
            headers = []
            if proto == 'h2-cl':
                headers.append(('content-length', str(len(body))))

            resp = c.post(path, body=body, headers=headers)

        c.close()
        return resp['status'], resp['body']

    if proto == 'h1-chunked' and body is not None:
        return h1_chunked_post(path, body)

    ctx = ssl_context(alpn=('http/1.1',))

    if body is None:
        resp = client.get_ssl(url=path, context=ctx)
    else:
        resp = client.post_ssl(url=path, body=body, context=ctx)

    body = resp['body']

    return resp['status'], body.encode() if isinstance(body, str) else body


@pytest.mark.parametrize('proto', ['h1', 'h2'])
def test_http2_access_log(proto, wait_for_record):
    need_h2()
    load_share()

    assert 'success' in client.conf(
        {
            'path': f'{option.temp_dir}/access.log',
            'format': '[$request_line] $status $body_bytes_sent '
            '[$response_header_connection]'
            '[$response_header_transfer_encoding]',
        },
        'access_log',
    )

    if proto == 'h2':
        version = 'HTTP/2.0'

        # HTTP/2 has neither field (RFC 9113, 8.2.2).
        tail = r'\[-\]\[-\]'

    else:
        version = 'HTTP/1.1'
        tail = r'\[close\]\[-\]'

    assert h1_or_h2(proto, path='/index.html') == (200, b'hello h2')
    assert (
        wait_for_record(
            rf'^\[GET /index.html {re.escape(version)}\] 200 8 {tail}$',
            'access.log',
        )
        is not None
    )

    status, body = h1_or_h2(proto, path='/nope?a=1')
    assert status == 404
    assert (
        wait_for_record(
            rf'^\[GET /nope\?a=1 {re.escape(version)}\] 404 {len(body)} '
            rf'{tail}$',
            'access.log',
        )
        is not None
    )


@pytest.mark.parametrize('proto', ['h1', 'h1-chunked', 'h2', 'h2-cl'])
def test_http2_proxy(proto):
    need_h2()

    # h2 (or h1) client -> Unit -> h1 upstream (Unit again) -> application.
    load_conf(
        {
            'listeners': {
                '*:8080': {'pass': 'routes'},
                '*:8081': {'pass': 'applications/mirror'},
            },
            'routes': [{'action': {'proxy': 'http://127.0.0.1:8081'}}],
            'applications': {'mirror': python_app('mirror')},
            'settings': {'http': {'chunked_transform': True}},
        }
    )

    assert h1_or_h2(proto) == (200, b'')

    # A body without content-length reaches the upstream with exactly one
    # Content-Length, the one nxt_http_request_chunked_transform() adds.
    for size in [10, 50000]:
        body = os.urandom(size // 2).hex().encode()
        assert h1_or_h2(proto, body=body) == (200, body), size
