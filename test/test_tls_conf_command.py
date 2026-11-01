import ssl

import pytest

from unit.applications.tls import ApplicationTLS

prerequisites = {'modules': {'openssl': 'any'}}

client = ApplicationTLS()


@pytest.fixture(autouse=True)
def setup_method_fixture():
    client.certificate()

    assert 'success' in client.conf(
        {
            "listeners": {
                "*:8080": {
                    "pass": "routes",
                    "tls": {"certificate": "default"},
                }
            },
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    ), 'load application configuration'


# The highest protocol below each one, for the fallback probe.  Nothing
# below TLS 1.2: the client's default minimum is TLS 1.2, so a context
# pinned to TLS 1.1 fails on the client side, not on the server.
LOWER = {
    'TLSv1.3': ssl.TLSVersion.TLSv1_2,
}


def client_context(maximum):
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    context.maximum_version = maximum
    return context


def test_tls_conf_command():
    # sock.version() is the negotiated protocol.  sock.cipher()[1] is the
    # version that defines the cipher, which can be older (a TLS 1.2
    # connection with ECDHE-RSA-AES256-SHA reports TLSv1.0), so it is not
    # used for protocol checks.

    (_, sock) = client.get_ssl(start=True)
    protocol = sock.version()
    sock.close()

    # The test needs a lower protocol that both sides speak.  Prove it
    # exists before any conf_commands, with a client pinned below
    # "protocol": a failure after conf_commands is then a server failure,
    # not a missing protocol.

    lower = LOWER.get(protocol)
    if lower is None:
        pytest.skip(f'no protocol below {protocol} to fall back to')

    try:
        (_, sock) = client.get_ssl(start=True, context=client_context(lower))

    except ssl.SSLError:
        pytest.skip('One TLS protocol available only.')

    fallback = sock.version()
    sock.close()

    # Set one conf_commands (disable protocol): the default client must
    # now fall back.

    assert 'success' in client.conf(
        {
            "certificate": "default",
            "conf_commands": {"protocol": f'-{protocol}'},
        },
        'listeners/*:8080/tls',
    ), 'protocol disabled'

    (_, sock) = client.get_ssl(start=True)

    assert sock.version() == fallback, 'new protocol used'
    cipher = sock.cipher()[0]

    sock.close()

    # Set two conf_commands (disable protocol and cipher): the connection
    # must stay on the fallback protocol with another cipher.

    assert 'success' in client.conf(
        {
            "certificate": "default",
            "conf_commands": {
                "protocol": f'-{protocol}',
                "cipherstring": f'DEFAULT:!{cipher}',
            },
        },
        'listeners/*:8080/tls',
    ), 'cipher disabled'

    (_, sock) = client.get_ssl(start=True)

    assert sock.version() == fallback, 'previous protocol used'
    assert sock.cipher()[0] != cipher, 'new cipher used'

    sock.close()


def test_tls_conf_command_invalid(skip_alert):
    skip_alert(r'SSL_CONF_cmd', r'failed to apply new conf')

    def check_conf_commands(conf_commands):
        assert 'error' in client.conf(
            {"certificate": "default", "conf_commands": conf_commands},
            'listeners/*:8080/tls',
        ), 'ivalid conf_commands'

    check_conf_commands([])
    check_conf_commands("blah")
    check_conf_commands({"": ""})
    check_conf_commands({"blah": ""})
    check_conf_commands({"protocol": {}})
    check_conf_commands({"protocol": "blah"})
    check_conf_commands({"protocol": "TLSv1.2", "blah": ""})


def test_tls_conf_command_invalid_path(skip_alert):
    skip_alert(r'SSL_CONF_cmd', r'failed to apply new conf')

    # nxt_conf_vldt_object_conf_commands() iterates the object itself instead
    # of going through the instrumented iterator, so the command name has to
    # be pushed onto the validation path explicitly; otherwise the reported
    # location stops at conf_commands and does not name the failing command.
    r = client.conf(
        {"certificate": "default", "conf_commands": {"protocol": {}}},
        'listeners/*:8080/tls',
    )

    assert 'error' in r

    # builds without SSL_CONF_cmd reject the member itself, so there is no
    # command name to attribute the error to.  The validator's text is in
    # "detail"; "error" is the generic "Invalid configuration." title.
    if 'built without' in r['detail']:
        pytest.skip('built without conf_commands support')

    assert r['location']['path'].endswith('/tls/conf_commands/protocol')
