import os
import socket
import time

import pytest

from conftest import run_process
from unit.applications.proto import ApplicationProto
from unit.option import option
from unit.utils import waitforsocket
from unit import port as port_map

client = ApplicationProto()
UPSTREAM_PORT = port_map.port(7999)


# A raw upstream that records the exact request head Unit forwards, so the test
# can check what the proxy actually put on the wire.
def capture_upstream(server_port, out_file):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', server_port))
    sock.listen(5)

    resp = b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n'

    while True:
        conn, _ = sock.accept()

        data = b''
        while b'\r\n\r\n' not in data:
            part = conn.recv(4096)
            if not part:
                break
            data += part

        with open(out_file, 'wb') as f:
            f.write(data)

        conn.sendall(resp)
        conn.close()


def upstream_file():
    return f'{option.temp_dir}/proxy_target_req'


@pytest.fixture(autouse=True)
def setup_method_fixture():
    out_file = upstream_file()

    if os.path.exists(out_file):
        os.unlink(out_file)

    run_process(capture_upstream, UPSTREAM_PORT, out_file)
    waitforsocket(UPSTREAM_PORT)

    # A rewrite re-parses its result, so "$uri" turns the decoded request path
    # back into the proxied target; the second route then proxies it upstream.
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes/rewrite"}},
            "routes": {
                "rewrite": [
                    {"action": {"rewrite": "$uri", "pass": "routes/proxy"}}
                ],
                "proxy": [
                    {"action": {"proxy": f"http://127.0.0.1:{UPSTREAM_PORT}"}}
                ],
            },
        }
    ), 'proxy rewrite configuration'


def test_proxy_rewrite_target_crlf():
    # The request path carries a percent-encoded CR/LF.  The main parser decodes
    # it into a raw CR/LF in r->path.  The rewrite writes the value of "$uri"
    # encoded and parses it again, so r->path holds the raw CR/LF again.  The
    # proxied upstream request line must carry them percent-encoded, not raw --
    # a raw CR/LF would inject a header into, or smuggle a request to, the
    # upstream.
    client.get(
        url='/x%0D%0AInjected:%20smuggled',
        headers={'Host': 'localhost', 'Connection': 'close'},
    )

    out_file = upstream_file()

    for _ in range(50):
        if os.path.exists(out_file):
            break
        time.sleep(0.1)

    assert os.path.exists(out_file), 'upstream received the proxied request'

    with open(out_file, 'rb') as f:
        sent = f.read()

    # No raw CR/LF injection: the "Injected" marker must not begin a new line.
    assert b'\r\nInjected' not in sent, 'no raw CRLF injected upstream'
    # The rewritten target reached the upstream percent-encoded instead.
    assert b'%0D%0AInjected' in sent, 'rewritten target is re-encoded'


def proxied_target(url):
    out_file = upstream_file()

    if os.path.exists(out_file):
        os.unlink(out_file)

    client.get(url=url, headers={'Host': 'localhost', 'Connection': 'close'})

    for _ in range(50):
        if os.path.exists(out_file) and os.path.getsize(out_file) > 0:
            break
        time.sleep(0.1)

    assert os.path.exists(out_file), f'upstream received {url}'

    with open(out_file, 'rb') as f:
        line = f.read().split(b'\r\n', 1)[0]

    print(f'client {url!r} upstream {line!r}')

    return line.split(b' ')[1]


def test_proxy_rewrite_target_delimiters():
    # "$request_uri" is parsed again as an encoded target, so "%3F" and
    # "%23" become a "?" and a "#" in the path.  They are path data: the
    # upstream must get them encoded, not as a query or fragment delimiter.
    # The query is sent as the router holds it: a rewrite does not change
    # it, so the text after a raw "?" of a rewrite result is not sent, and a
    # "%3F" in a rewrite result is path data too.  A target that was not
    # rewritten goes out as the client sent it.
    assert 'success' in client.conf(
        {
            "rewrite": [
                {
                    "match": {"uri": "/r/*"},
                    "action": {
                        "rewrite": "$request_uri",
                        "pass": "routes/proxy",
                    },
                },
                {
                    "match": {"uri": "/q/*"},
                    "action": {"rewrite": "/qq?k=v", "pass": "routes/proxy"},
                },
                {
                    "match": {"uri": "/p/*"},
                    "action": {"rewrite": "/pp%3Fk=v", "pass": "routes/proxy"},
                },
                {"action": {"pass": "routes/proxy"}},
            ],
            "proxy": [
                {"action": {"proxy": f"http://127.0.0.1:{UPSTREAM_PORT}"}}
            ],
        },
        'routes',
    ), 'delimiter routes'

    cases = [
        ('/r/a%3Fb', b'/r/a%3Fb'),
        ('/r/a%23b', b'/r/a%23b'),
        ('/r/a%3Fb?q=1', b'/r/a%3Fb?q=1'),
        ('/r/a%23b%3Fc?q=%3F', b'/r/a%23b%3Fc?q=%3F'),
        ('/r/a%3Fb?', b'/r/a%3Fb'),
        ('/r/a%2520b?x', b'/r/a%2520b?x'),
        ('/r/plain', b'/r/plain'),
        ('/r/plain?q=1', b'/r/plain?q=1'),
        ('/n/a%3Fb?q=%23', b'/n/a%3Fb?q=%23'),
        ('/n/a%3fb', b'/n/a%3fb'),
        ('/q/x', b'/qq'),
        ('/q/x?a=1', b'/qq?a=1'),
        ('/p/x?a=1', b'/pp%3Fk=v?a=1'),
    ]

    seen = [(url, proxied_target(url)) for url, _ in cases]

    assert seen == [(url, want) for url, want in cases]


def test_proxy_rewrite_target_uri_value():
    # "$uri" is decoded once.  A rewrite keeps its value as data, so a "?"
    # or "#" in it is part of the path, and the upstream gets it encoded.
    # A path that the client encoded twice stays encoded once.
    assert 'success' in client.conf(
        {
            "rewrite": [
                {"action": {"rewrite": "/app$uri", "pass": "routes/proxy"}}
            ],
            "proxy": [
                {"action": {"proxy": f"http://127.0.0.1:{UPSTREAM_PORT}"}}
            ],
        },
        'routes',
    ), 'uri value routes'

    cases = [
        ('/v1/a%3Fb', b'/app/v1/a%3Fb'),
        ('/v1/a%23b?q=1', b'/app/v1/a%23b?q=1'),
        ('/v1/a%253Fb', b'/app/v1/a%253Fb'),
        ('/v1/a%0D%0Ab', b'/app/v1/a%0D%0Ab'),
        ('/v1/plain?q=1', b'/app/v1/plain?q=1'),
    ]

    seen = [(url, proxied_target(url)) for url, _ in cases]

    assert seen == [(url, want) for url, want in cases]
