import pytest

from unit import port as port_map


@pytest.fixture
def off_base():
    saved = port_map.base()
    port_map.set_base(18080)

    yield

    port_map.set_base(saved)


def test_port_map_remap_str(off_base):
    assert port_map.remap('"*:8080" "*:8081" 7999') == (
        '"*:18080" "*:18081" 17999'
    )
    assert port_map.remap('"08080" 80800 %08d') == '"08080" 80800 %08d'


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
