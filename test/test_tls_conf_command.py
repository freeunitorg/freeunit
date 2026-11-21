import re
import ssl
import subprocess

import pytest

from unit.applications.tls import ApplicationTLS
from unit import port as port_map

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


# The TLS policy of a listener is "conf_commands": MinProtocol, MaxProtocol,
# CipherString (TLS 1.2 and below) and CipherSuites (TLS 1.3).

LEGACY = 'DEFAULT:@SECLEVEL=0'

TLS13_SUITES = [
    'TLS_AES_256_GCM_SHA384',
    'TLS_CHACHA20_POLY1305_SHA256',
    'TLS_AES_128_GCM_SHA256',
]


def conf_commands(commands):
    return client.conf(
        {"certificate": "default", "conf_commands": commands},
        'listeners/*:8080/tls',
    )


def pinned_context(version, ciphers=None):
    context = client_context(version)
    context.minimum_version = version

    if ciphers is not None:
        context.set_ciphers(ciphers)

    return context


def handshake(context=None):
    """The negotiated protocol and cipher, or None if the handshake failed."""
    kwargs = {} if context is None else {'context': context}

    try:
        (_, sock) = client.get_ssl(start=True, **kwargs)

    except (ssl.SSLError, ConnectionError):
        return None

    result = (sock.version(), sock.cipher()[0])
    sock.close()

    return result


def s_client(*args):
    """The cipher "openssl s_client" negotiated, or None.  Python cannot
    choose the TLS 1.3 suites it offers before 3.14."""
    out = subprocess.run(
        [
            'openssl',
            's_client',
            '-connect',
            f'127.0.0.1:{port_map.port(8080)}',
            *args,
        ],
        input=b'',
        capture_output=True,
        timeout=30,
        check=False,
    ).stdout.decode(errors='replace')

    found = re.search(r'^(?:New|Reused), \S+ Cipher is (\S+)$', out, re.M)

    if found is None or found.group(1) == '(NONE)':
        return None

    return found.group(1)


@pytest.mark.filterwarnings('ignore:ssl.TLSVersion.TLSv1_1 is deprecated')
def test_tls_conf_command_min_protocol():
    tls11 = pinned_context(ssl.TLSVersion.TLSv1_1, LEGACY)
    tls12 = pinned_context(ssl.TLSVersion.TLSv1_2)
    tls13 = pinned_context(ssl.TLSVersion.TLSv1_3)

    # TLS 1.1 needs security level 0 on both sides.  It has to work before
    # the floor is raised: only then is a refusal the floor's.

    assert 'success' in conf_commands(
        {"CipherString": LEGACY, "MinProtocol": "TLSv1.1"}
    )

    below = handshake(tls11)
    if below is None:
        pytest.skip('TLS 1.1 is not available')

    assert below[0] == 'TLSv1.1', 'below the future floor'

    assert 'success' in conf_commands(
        {"CipherString": LEGACY, "MinProtocol": "TLSv1.2"}
    )

    assert handshake(tls11) is None, 'floor 1.2 refuses 1.1'
    assert handshake(tls12)[0] == 'TLSv1.2', 'floor 1.2 accepts 1.2'
    assert handshake(tls13)[0] == 'TLSv1.3', 'floor 1.2 accepts 1.3'

    assert 'success' in conf_commands({"MinProtocol": "TLSv1.3"})

    assert handshake(tls12) is None, 'floor 1.3 refuses 1.2'
    assert handshake(tls13)[0] == 'TLSv1.3', 'floor 1.3 accepts 1.3'


def test_tls_conf_command_max_protocol():
    tls12 = pinned_context(ssl.TLSVersion.TLSv1_2)
    tls13 = pinned_context(ssl.TLSVersion.TLSv1_3)

    assert handshake()[0] == 'TLSv1.3', 'no ceiling'

    assert 'success' in conf_commands({"MaxProtocol": "TLSv1.2"})

    assert handshake(tls13) is None, 'ceiling 1.2 refuses 1.3'
    assert handshake(tls12)[0] == 'TLSv1.2', 'ceiling 1.2 accepts 1.2'
    assert handshake()[0] == 'TLSv1.2', 'ceiling 1.2, default client'


def test_tls_conf_command_ciphersuites():
    tls12 = pinned_context(ssl.TLSVersion.TLSv1_2)
    tls13 = pinned_context(ssl.TLSVersion.TLSv1_3)

    default13 = handshake(tls13)[1]
    default12 = handshake(tls12)[1]

    suite = next(s for s in TLS13_SUITES if s != default13)

    assert 'success' in conf_commands({"CipherSuites": suite})

    assert handshake(tls13)[1] == suite, 'the only suite is negotiated'
    assert s_client('-tls1_3', '-ciphersuites', suite) == suite, 'offered'
    assert (
        s_client('-tls1_3', '-ciphersuites', default13) is None
    ), 'a client without the suite is refused'

    assert handshake(tls12)[1] == default12, 'TLS 1.2 is not affected'


def test_tls_conf_command_cipherstring_not_tls13():
    # "CipherString" alone leaves every default TLS 1.3 suite on offer.

    tls12 = pinned_context(ssl.TLSVersion.TLSv1_2)
    tls13 = pinned_context(ssl.TLSVersion.TLSv1_3)

    default13 = handshake(tls13)[1]
    default12 = handshake(tls12)[1]

    cipher = 'ECDHE-RSA-AES128-GCM-SHA256'
    if cipher == default12:
        cipher = 'ECDHE-RSA-AES256-GCM-SHA384'

    assert 'success' in conf_commands({"CipherString": cipher})

    assert handshake(tls12)[1] == cipher, 'TLS 1.2 restricted'
    assert handshake(tls13)[1] == default13, 'TLS 1.3 suite unchanged'

    for suite in TLS13_SUITES:
        assert (
            s_client('-tls1_3', '-ciphersuites', suite) == suite
        ), f'{suite} still served'


def test_tls_conf_command_invalid_value():
    # A name or a value that only OpenSSL can judge is refused by the
    # validation, with the command in the location, and never reaches the
    # router.

    assert 'success' in conf_commands({"MinProtocol": "TLSv1.2"})

    def check(commands, command, reason):
        r = conf_commands(commands)

        assert 'error' in r, commands

        if 'built without' in r['detail']:
            pytest.skip('built without conf_commands support')

        location = '/tls/conf_commands'

        if command is not None:
            location += f'/{command}'

        assert r['error'] == 'Invalid configuration.', commands
        assert r['location']['path'].endswith(location), commands
        assert reason in r['detail'], commands

    check({"MinProtocol": "TLSv9"}, 'MinProtocol', 'bad value')
    check({"CipherSuites": "TLS_NOPE"}, 'CipherSuites', 'no cipher match')
    check({"MinProtocl": "TLSv1.2"}, 'MinProtocl', 'unknown cmd name')
    check(
        {"MinProtocol": "TLSv1.2", "MaxProtocol": "TLSv1.3", "Nope": "x"},
        'Nope',
        'unknown cmd name',
    )

    # OpenSSL takes these two as they are, and the listener would then
    # refuse every handshake.  The bounds are checked after all commands,
    # so the order of the members does not matter.
    check(
        {"MinProtocol": "TLSv1.3", "MaxProtocol": "TLSv1.2"},
        None,
        'above the maximum',
    )
    check(
        {"MaxProtocol": "TLSv1.2", "MinProtocol": "TLSv1.3"},
        None,
        'above the maximum',
    )

    # The configuration before the refusals is still in effect.

    assert handshake(pinned_context(ssl.TLSVersion.TLSv1_2)) is not None
    assert (
        client.conf_get('listeners/*:8080/tls/conf_commands/MinProtocol')
        == 'TLSv1.2'
    )


def test_tls_conf_command_bounds_order():
    # A pair that is valid as a whole is accepted in either order.

    for commands in (
        {"MinProtocol": "TLSv1.3", "MaxProtocol": "TLSv1.3"},
        {"MaxProtocol": "TLSv1.3", "MinProtocol": "TLSv1.3"},
    ):
        r = conf_commands(commands)

        if 'error' in r and 'built without' in r['detail']:
            pytest.skip('built without conf_commands support')

        assert 'success' in r, commands


def test_tls_conf_command_server_info_file(temp_dir):
    # OpenSSL attaches the server info to the certificate, which only the
    # router has: the validation leaves file commands to it.

    path = f'{temp_dir}/serverinfo.pem'

    # One extension of unassigned type 1000 with two bytes of data.
    with open(path, 'w', encoding='utf-8') as f:
        f.write(
            '-----BEGIN SERVERINFO FOR TEST-----\n'
            'A+gAAgAA\n'
            '-----END SERVERINFO FOR TEST-----\n'
        )

    r = conf_commands({"ServerInfoFile": path})

    if 'error' in r and 'built without' in r['detail']:
        pytest.skip('built without conf_commands support')

    assert 'success' in r, r
    assert handshake(pinned_context(ssl.TLSVersion.TLSv1_2)) is not None
