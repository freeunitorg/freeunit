"""Responses that RFC 9112 Sect. 6.3 defines as having no message body.

An application that writes a body on 204/304 -- or on any response to HEAD --
used to have those bytes forwarded verbatim, with no Content-Length and no
chunked framing, so a downstream parser read them as the start of the next
response.  See freeunitorg/freeunit#164.
"""

import base64
import os

import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.option import option

prerequisites = {'modules': {'php': 'all'}}

client = ApplicationPHP()

# The application writes exactly these 10 bytes on every response.
SMUGGLED = 'SMUGGLEDXX'

KEEPALIVE = {'Host': 'localhost', 'Connection': 'keep-alive'}
CLOSE = {'Host': 'localhost', 'Connection': 'close'}


@pytest.fixture(autouse=True)
def setup_method_fixture():
    client.load('no_body_status')


def split_response(raw):
    """Split a raw response into (header block, everything after it)."""

    assert '\r\n\r\n' in raw, f'response has no header terminator: {raw!r}'

    head, _, rest = raw.partition('\r\n\r\n')

    return head, rest


def request_raw(url, method='GET', sock=None, headers=None):
    kwargs = {
        'url': url,
        'headers': KEEPALIVE if headers is None else headers,
        'raw_resp': True,
        'read_timeout': 1,
        'start': True,
    }

    if sock is not None:
        kwargs['sock'] = sock

    return client.http(method, **kwargs)


@pytest.mark.parametrize('status', [204, 304])
@pytest.mark.parametrize('content_length', [False, True])
def test_no_body_status_drops_app_body(status, content_length):
    url = f'/?status={status}'
    if content_length:
        url += '&cl=1'

    raw, sock = request_raw(url)

    try:
        head, rest = split_response(raw)

        assert f' {status} ' in head.split('\r\n')[0], 'status line'
        assert (
            SMUGGLED not in raw
        ), f'body forwarded on {status}: {raw!r}'
        assert rest == '', f'trailing bytes after header: {rest!r}'
        assert (
            'Transfer-Encoding' not in head
        ), 'no chunked framing on a bodyless status'

        if status == 204:
            # RFC 9110 Sect. 8.6: no Content-Length on 204.
            assert 'Content-Length' not in head, 'no Content-Length on 204'

        # The connection must still be usable: the next request has to parse
        # as a complete response of its own, not as a continuation.
        resp = client.get(sock=sock, url='/?status=200', headers=CLOSE)

        assert resp['status'] == 200, 'keep-alive: second request status'
        assert resp['body'] == SMUGGLED, 'keep-alive: second request body'

    finally:
        sock.close()


def test_no_body_status_head_drops_app_body():
    raw, sock = request_raw('/?status=200&cl=1', method='HEAD')

    try:
        head, rest = split_response(raw)

        assert ' 200 ' in head.split('\r\n')[0], 'status line'
        assert SMUGGLED not in raw, f'body forwarded on HEAD: {raw!r}'
        assert rest == '', f'trailing bytes after header: {rest!r}'
        # A HEAD response keeps the length the equivalent GET would report.
        assert 'Content-Length: 10' in head, 'HEAD keeps Content-Length'

        resp = client.get(sock=sock, url='/?status=200', headers=CLOSE)

        assert resp['status'] == 200, 'keep-alive after HEAD: status'
        assert resp['body'] == SMUGGLED, 'keep-alive after HEAD: body'

    finally:
        sock.close()


def test_no_body_status_get_still_has_body():
    """Control: an ordinary status keeps the application's body."""

    resp = client.get(url='/?status=200')

    assert resp['status'] == 200, 'status'
    assert resp['body'] == SMUGGLED, 'body'


def test_no_body_status_proxy():
    """The proxy path must not re-introduce the body either."""

    php_dir = f'{option.test_dir}/php'

    assert 'success' in client.conf(
        {
            "listeners": {
                "*:8080": {"pass": "routes"},
                "*:8081": {"pass": "applications/no_body_status"},
            },
            "routes": [{"action": {"proxy": "http://127.0.0.1:8081"}}],
            "applications": {
                "no_body_status": {
                    "type": client.get_application_type(),
                    "processes": {"spare": 0},
                    "root": f'{php_dir}/no_body_status',
                    "working_directory": f'{php_dir}/no_body_status',
                    "index": "index.php",
                }
            },
        }
    ), 'proxy configuration'

    raw, sock = request_raw('/?status=204')

    try:
        _, rest = split_response(raw)

        assert SMUGGLED not in raw, f'proxy forwarded a 204 body: {raw!r}'
        assert rest == '', f'proxy trailing bytes: {rest!r}'

        resp = client.get(sock=sock, url='/?status=200', headers=CLOSE)

        assert resp['status'] == 200, 'proxy keep-alive: status'
        assert resp['body'] == SMUGGLED, 'proxy keep-alive: body'

    finally:
        sock.close()


def test_no_body_status_websocket_upgrade_request():
    """A WebSocket-upgrade request answered with 204 is still bodyless.

    r->websocket_handshake is set while the *request* headers are parsed, long
    before the application picks a status, so the upgrade exemption has to test
    the status too -- otherwise this request keeps the unframed path.
    """

    key = base64.b64encode(os.urandom(16)).decode()

    raw, sock = request_raw(
        '/?status=204',
        headers={
            'Host': 'localhost',
            'Connection': 'Upgrade',
            'Upgrade': 'websocket',
            'Sec-WebSocket-Key': key,
            'Sec-WebSocket-Version': 13,
        },
    )

    try:
        head, rest = split_response(raw)

        assert ' 204 ' in head.split('\r\n')[0], 'status line'
        assert SMUGGLED not in raw, f'body forwarded on upgrade+204: {raw!r}'
        assert rest == '', f'trailing bytes after header: {rest!r}'

        resp = client.get(sock=sock, url='/?status=200', headers=CLOSE)

        assert resp['status'] == 200, 'keep-alive after upgrade+204: status'
        assert resp['body'] == SMUGGLED, 'keep-alive after upgrade+204: body'

    finally:
        sock.close()


def test_no_body_status_app_transfer_encoding():
    """An application-sent Transfer-Encoding is a generic field; drop it."""

    raw, sock = request_raw('/?status=204&te=1')

    try:
        head, rest = split_response(raw)

        assert ' 204 ' in head.split('\r\n')[0], 'status line'
        assert (
            'Transfer-Encoding' not in head
        ), f'Transfer-Encoding kept on 204: {head!r}'
        assert SMUGGLED not in raw, f'body forwarded on 204: {raw!r}'
        assert rest == '', f'trailing bytes after header: {rest!r}'

        resp = client.get(sock=sock, url='/?status=200', headers=CLOSE)

        assert resp['status'] == 200, 'keep-alive: status'
        assert resp['body'] == SMUGGLED, 'keep-alive: body'

    finally:
        sock.close()


def test_no_body_status_response_headers_content_length():
    """Framing headers added by "response_headers" must go too.

    They are added as generic fields, without setting r->resp.content_length,
    so skipping that one pointer is not enough.  Note the lowercase name: the
    config validator rejects "Content-Length" here, but with a case-sensitive
    memcmp (nxt_conf_vldt_response_header()), so "content-length" is accepted
    and reaches the response.  Transfer-Encoding is not guarded at all.
    """

    php_dir = f'{option.test_dir}/php'

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {
                    # Scoped to the 204 so the follow-up 200 on the same
                    # connection is framed by the application alone.
                    "match": {"arguments": {"status": "204"}},
                    "action": {
                        "pass": "applications/no_body_status",
                        "response_headers": {
                            "content-length": "10",
                            "Transfer-Encoding": "chunked",
                        },
                    },
                },
                {"action": {"pass": "applications/no_body_status"}},
            ],
            "applications": {
                "no_body_status": {
                    "type": client.get_application_type(),
                    "processes": {"spare": 0},
                    "root": f'{php_dir}/no_body_status',
                    "working_directory": f'{php_dir}/no_body_status',
                    "index": "index.php",
                }
            },
        }
    ), 'response_headers configuration'

    raw, sock = request_raw('/?status=204')

    try:
        head, rest = split_response(raw)

        assert ' 204 ' in head.split('\r\n')[0], 'status line'
        assert (
            'content-length' not in head.lower()
        ), f'response_headers Content-Length kept on 204: {head!r}'
        assert (
            'transfer-encoding' not in head.lower()
        ), f'response_headers Transfer-Encoding kept on 204: {head!r}'
        assert SMUGGLED not in raw, f'body forwarded on 204: {raw!r}'
        assert rest == '', f'trailing bytes after header: {rest!r}'

        resp = client.get(sock=sock, url='/?status=200', headers=CLOSE)

        assert resp['status'] == 200, 'keep-alive: status'
        assert resp['body'] == SMUGGLED, 'keep-alive: body'

    finally:
        sock.close()
