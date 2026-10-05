"""Field names and methods too long for the libunit uint8_t lengths are
refused with 431 and 501 instead of reaching PHP truncated."""

from unit.applications.lang.php import ApplicationPHP

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()


def test_php_protocol_field_name_length():
    client.load('variables')

    # "HTTP_" + name must fit in 255 bytes.
    for length, status in ((250, 200), (251, 431), (253, 431), (255, 431)):
        resp = client.get(headers={'Host': 'localhost', 'X' * length: 'v'})
        assert resp['status'] == status, length

    assert client.get()['status'] == 200, 'still serving'


def test_php_protocol_method_length():
    client.load('variables')

    for length, status in ((255, 200), (256, 501), (280, 501)):
        resp = client.http('A' * length, headers={'Host': 'localhost'})
        assert resp['status'] == status, length

    assert client.get()['status'] == 200, 'still serving'
