import re
from pathlib import Path

import pytest

from unit import port as port_map
from unit import port_lint
from unit.control import Control

TEST_DIR = Path(__file__).parent


@pytest.fixture
def off_base():
    saved = port_map.base()
    port_map.set_base(18080)

    yield

    port_map.set_base(saved)


def test_port_map_set_base():
    saved = port_map.base()

    try:
        # A base whose shifted band lands on a literal is refused; 8080 is
        # the identity and is not.
        for base in (port_map.MIN_BASE, 8080, 18080, port_map.MAX_BASE):
            port_map.set_base(base)

        for base in (port_map.MIN_BASE - 1, port_map.MAX_BASE + 1, 8081, 7999):
            with pytest.raises(ValueError):
                port_map.set_base(base)

    finally:
        port_map.set_base(saved)


def test_port_map_remap_str(off_base):
    assert port_map.remap('"*:8080" "*:8081" "127.0.0.1:7999"') == (
        '"*:18080" "*:18081" "127.0.0.1:17999"'
    )
    assert port_map.remap('"*:8080-8090"') == '"*:18080-18090"'
    assert port_map.remap('"http://[::1]:8082/x"') == '"http://[::1]:18082/x"'
    assert port_map.remap('"*:08080" 80800 %08d') == '"*:08080" 80800 %08d'
    assert port_map.remap('"[2001:8080::8080]:8080"') == (
        '"[2001:8080::8080]:18080"'
    )


def test_port_map_remap_port_fields_only(off_base):
    # A literal that is not a port field is data: it reaches Unit unchanged.
    body = (
        '{"*:8080": {"pass": "applications/app"}, '
        '"environment": {"PORT": "8080", "N": "8081-8090"}, '
        '"return": 7999, "uri": "/8443"}'
    )

    assert port_map.remap(body) == body.replace('"*:8080"', '"*:18080"', 1)
    assert port_map.remap(body.encode()) == port_map.remap(body).encode()


def test_port_map_remap_utf8_bytes(off_base):
    body = '{"*:8080": {"pass": "routes/ü"}}'.encode()

    remapped = port_map.remap(body)

    assert remapped == '{"*:18080": {"pass": "routes/ü"}}'.encode()

    remapped = port_map.remap(bytearray(body))

    assert isinstance(remapped, bytearray)
    assert remapped == '{"*:18080": {"pass": "routes/ü"}}'.encode()


def test_port_map_remap_non_utf8_bytes(off_base):
    assert port_map.remap(b'\xff"*:8081"') == b'\xff"*:18081"'


def test_port_map_remap_idempotent(off_base):
    body = b'{"*:8080": {}}'

    assert port_map.remap(port_map.remap(body)) == port_map.remap(body)


def test_port_map_control_config_only(off_base):
    control = Control()
    pem = b'-----BEGIN CERTIFICATE-----\nMIIB8080x\n'

    args = control._get_args('/certificates/8080', pem)

    assert args['url'] == '/certificates/8080'
    assert args['body'] == pem

    args = control._get_args('/js_modules/m', 'export default 8080;')

    assert args['body'] == 'export default 8080;'

    args = control._get_args('/config/listeners/*:8080', '{"*:8081": {}}')

    assert args['url'] == '/config/listeners/*:18080'
    assert args['body'] == '{"*:18081": {}}'

    args = control._get_args('/config', '{"*:8080": {}}')

    assert args['body'] == '{"*:18080": {}}'


def test_port_map_helper_block(off_base):
    # The helper block starts at 7976 (test_proxy_te.py).
    assert port_map.port(7976) == 18080 - 104
    assert port_map.port(7978) == 18080 - 102
    assert port_map.port(7999) == 18080 - 81
    assert port_map.port(7975) == 7975
    assert port_map.port(None) is None


def test_port_map_helper_registry():
    # Every port the fake_upstream registry reserves is in the map, so a new
    # slot cannot escape --port silently.
    readme = (TEST_DIR / 'fake_upstream' / 'README.md').read_text()
    registered = {int(p) for p in re.findall(r'^\| (\d{4}) \|', readme, re.M)}

    assert registered, 'registry table found'
    assert registered <= port_map.LITERALS, registered - port_map.LITERALS


def test_port_map_lint():
    # The map is the identity at the default base, so a raw literal that
    # bypasses it passes every default-base run.  The lint reads the sources
    # instead: the tree is clean, and each planted form is reported.
    findings = [
        f'{path}:{line}: {message}'
        for path, line, message in port_lint.find([TEST_DIR])
    ]

    assert findings == [], '\n'.join(findings)


def test_port_map_lint_catches(tmp_path):
    source = tmp_path / 'test_planted.py'
    source.write_text(
        '''
UPSTREAM_PORT = 7975                                                  # 2
OTHER_PORT = port_map.port(7974)                                      # 3

def connect(port=8080):
    return socket.create_connection(('127.0.0.1', port))

def test_planted():
    sock.connect(('127.0.0.1', 8080))                                 # 9
    ssl.get_server_certificate(('127.0.0.1', 8443))                   # 10
    waitforsocket(7994)                                               # 11
    assert headers['X-Server-Port'] == '8080'                         # 12
    assert headers['Location'] == f'http://localhost:8080/{x}'        # 13
    client.get(port=8081)
    client.get(port=8081 if ipv4 else 8082)
    client.conf({"*:8080": {}}, 'listeners/*:8080')
    assert client.conf_get() == port_map.expected({"*:8080": {}})
    connect(port=8082)
    connect(port_map.port(8083))
    client.get(headers={'Host': 'localhost:8080'})
    assert server_name('localhost:8080') == 'localhost'
    assert headers['X-Host'] == '[::1]:8080'
    s.bind(('127.0.0.1', 0))
'''
    )

    lines = [line for _, line, _ in port_lint.find([source])]

    assert lines == [2, 3, 9, 10, 11, 12, 13]
