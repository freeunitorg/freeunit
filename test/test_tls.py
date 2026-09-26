import io
import os
import shutil
import ssl
import subprocess
import time
from pathlib import Path

import pytest

from conftest import unit_run, unit_stop
from unit.applications.tls import ApplicationTLS
from unit.option import option

prerequisites = {'modules': {'python': 'any', 'openssl': 'any'}}

client = ApplicationTLS()


def add_tls(application='empty', cert='default', port=8080):
    assert 'success' in client.conf(
        {
            "pass": f"applications/{application}",
            "tls": {"certificate": cert},
        },
        f'listeners/*:{port}',
    )


def ca(cert='root', out='localhost'):
    subprocess.check_output(
        [
            'openssl',
            'ca',
            '-batch',
            '-config',
            f'{option.temp_dir}/ca.conf',
            '-keyfile',
            f'{option.temp_dir}/{cert}.key',
            '-cert',
            f'{option.temp_dir}/{cert}.crt',
            '-in',
            f'{option.temp_dir}/{out}.csr',
            '-out',
            f'{option.temp_dir}/{out}.crt',
        ],
        stderr=subprocess.STDOUT,
    )


def context_cert_req(cert='root'):
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_REQUIRED
    context.verify_flags &= ~ssl.VERIFY_X509_STRICT
    context.load_verify_locations(f'{option.temp_dir}/{cert}.crt')

    return context


def generate_ca_conf():
    Path(f'{option.temp_dir}/ca.conf').write_text(
        f"""[ ca ]
default_ca = myca

[ myca ]
new_certs_dir = {option.temp_dir}
database = {option.temp_dir}/certindex
default_md = sha256
policy = myca_policy
serial = {option.temp_dir}/certserial
default_days = 1
x509_extensions = myca_extensions
copy_extensions = copy

[ myca_policy ]
commonName = optional

[ myca_extensions ]
basicConstraints = critical,CA:TRUE""",
        encoding='utf-8',
    )

    Path(f'{option.temp_dir}/certserial').write_text('1000', encoding='utf-8')
    Path(f'{option.temp_dir}/certindex').touch()
    Path(f'{option.temp_dir}/certindex.attr').touch()


def replace_cert(name='default'):
    # Make a new key pair under the same name and store it over the old one.
    client.certificate(name, False)
    return client.certificate_load(name)


def remove_tls(application='empty', port=8080):
    assert 'success' in client.conf(
        {"pass": f"applications/{application}"}, f'listeners/*:{port}'
    )


def req(name='localhost', subject=None):
    subj = subject if subject is not None else f'/CN={name}/'

    subprocess.check_output(
        [
            'openssl',
            'req',
            '-new',
            '-subj',
            subj,
            '-config',
            f'{option.temp_dir}/openssl.conf',
            '-out',
            f'{option.temp_dir}/{name}.csr',
            '-keyout',
            f'{option.temp_dir}/{name}.key',
        ],
        stderr=subprocess.STDOUT,
    )


def test_tls_listener_option_add():
    client.load('empty')

    client.certificate()

    add_tls()

    assert client.get_ssl()['status'] == 200, 'add listener option'


def test_tls_listener_option_remove():
    client.load('empty')

    client.certificate()

    add_tls()

    client.get_ssl()

    remove_tls()

    assert client.get()['status'] == 200, 'remove listener option'


def test_tls_certificate_remove():
    client.load('empty')

    client.certificate()

    assert 'success' in client.conf_delete(
        '/certificates/default'
    ), 'remove certificate'


def test_tls_certificate_remove_used():
    client.load('empty')

    client.certificate()

    add_tls()

    assert 'error' in client.conf_delete(
        '/certificates/default'
    ), 'remove certificate'


def test_tls_certificate_remove_nonexisting():
    client.load('empty')

    client.certificate()

    add_tls()

    assert 'error' in client.conf_delete(
        '/certificates/blah'
    ), 'remove nonexistings certificate'


def test_tls_certificate_update():
    client.load('empty')

    client.certificate()

    add_tls()

    cert_old = ssl.get_server_certificate(('127.0.0.1', 8080))

    # The listener names the bundle, so Unit applies the configuration
    # again before it answers.
    assert (
        replace_cert().get('success') == 'Certificate chain updated.'
    ), 'replaced'

    assert cert_old != ssl.get_server_certificate(
        ('127.0.0.1', 8080)
    ), 'update certificate'

    assert 'chain' in client.conf_get('/certificates/default'), 'listed'


def test_tls_certificate_update_unused():
    client.load('empty')

    client.certificate()
    client.certificate('unused')

    add_tls()

    # No listener names the bundle, so there is no reconfiguration.
    assert (
        replace_cert('unused').get('success') == 'Certificate chain uploaded.'
    ), 'stored'


def test_tls_certificate_update_keepalive():
    client.load('empty')

    client.certificate()

    add_tls()

    (resp, sock) = client.get_ssl(
        headers={'Host': 'localhost', 'Connection': 'keep-alive'},
        start=True,
        read_timeout=1,
    )

    assert resp['status'] == 200, 'keepalive 1'

    cert_old = sock.getpeercert(True)

    assert 'success' in replace_cert(), 'replaced'

    # The accepted connection keeps its old TLS context.
    (resp, sock) = client.get_ssl(
        headers={'Host': 'localhost', 'Connection': 'close'},
        sock=sock,
        start=True,
    )

    assert resp['status'] == 200, 'keepalive 2'
    assert sock.getpeercert(True) == cert_old, 'old connection, old cert'
    sock.close()

    (resp, sock) = client.get_ssl(
        headers={'Host': 'localhost', 'Connection': 'close'}, start=True
    )

    assert resp['status'] == 200, 'new connection'
    assert sock.getpeercert(True) != cert_old, 'new connection, new cert'


def test_tls_certificate_update_inflight():
    client.load('empty')

    client.certificate()

    add_tls()

    # Send half of the request before the replacement and the rest after.
    sock = client.http(
        b'GET / HTTP/1.1\r\nHost: localhost\r\n',
        raw=True,
        no_recv=True,
        wrapper=client._default_context.wrap_socket,
    )

    cert_old = sock.getpeercert(True)

    assert 'success' in replace_cert(), 'replaced'

    resp = client.http(b'Connection: close\r\n\r\n', raw=True, sock=sock)

    assert resp['status'] == 200, 'in-flight request'
    assert cert_old != ssl.get_server_certificate(
        ('127.0.0.1', 8080)
    ), 'new handshake, new cert'


def test_tls_certificate_update_mismatch(skip_alert):
    skip_alert(r'certificate and private key do not match')

    client.load('empty')

    client.certificate()

    add_tls()

    cert_old = ssl.get_server_certificate(('127.0.0.1', 8080))

    # Unit refuses a bundle with the wrong key.  Nothing changes.
    client.certificate('other', False)

    assert 'error' in client.certificate_load('default', 'other'), 'refused'

    assert cert_old == ssl.get_server_certificate(
        ('127.0.0.1', 8080)
    ), 'old certificate still served'

    assert 'chain' in client.conf_get('/certificates/default'), 'still listed'


def test_tls_certificate_update_store_fail(skip_alert):
    skip_alert(r'unlink.*failed', r'failed to store certificate')

    client.load('empty')

    client.certificate()

    add_tls()

    cert_old = ssl.get_server_certificate(('127.0.0.1', 8080))
    info_old = client.conf_get('/certificates/default')

    # A directory with a file in it blocks the temporary file of main.
    tmp = f'{option.temp_dir}/state/certs/.store.tmp'
    os.makedirs(tmp)
    Path(f'{tmp}/block').touch()

    assert (
        replace_cert()['error'] == 'Failed to store certificate.'
    ), 'store failed'

    # The old metadata is back, and the old bundle stays in use.
    assert client.conf_get('/certificates/default') == info_old, 'old info'
    assert cert_old == ssl.get_server_certificate(
        ('127.0.0.1', 8080)
    ), 'old certificate still served'

    # A new name is removed from the metadata.
    assert 'error' in replace_cert('new'), 'new store failed'
    assert 'new' not in client.conf_get('/certificates'), 'no new info'
    assert 'new' not in client.conf_get('/')['certificates'], 'not in root'

    shutil.rmtree(tmp)


def test_tls_certificate_update_long_name():
    client.certificate('default', False)

    with open(f'{option.temp_dir}/default.key', 'rb') as k, open(
        f'{option.temp_dir}/default.crt', 'rb'
    ) as c:
        bundle = k.read() + c.read()

    # The longest file name on most file systems.  The temporary file of
    # main has a fixed name, so the store does not need a longer name.
    name = 'a' * 255

    for _ in range(2):
        assert 'success' in client.conf(
            bundle, f'/certificates/{name}'
        ), 'stored'

    assert 'chain' in client.conf_get(f'/certificates/{name}'), 'listed'


def test_tls_certificate_update_sni():
    client.load('empty')

    client.certificate('default')
    client.certificate('localhost')

    add_tls(cert=['default', 'localhost'])

    def peer_cert(host):
        (resp, sock) = client.get_ssl(
            headers={'Host': host, 'Connection': 'close'}, start=True
        )

        assert resp['status'] == 200, host
        return sock.getpeercert(True)

    default_old = peer_cert('default')
    localhost_old = peer_cert('localhost')

    assert default_old != localhost_old, 'sni selects the bundle'

    # Replace one bundle of the array.  The other bundle does not change.
    assert (
        replace_cert('localhost').get('success')
        == 'Certificate chain updated.'
    ), 'replaced'

    assert peer_cert('default') == default_old, 'other element untouched'
    assert peer_cert('localhost') != localhost_old, 'element replaced'


def test_tls_certificate_update_restart(requires_restart):
    client.certificate()

    # Use no application.  unit_run() expects a restarted instance to have
    # no applications.
    assert 'success' in client.conf(
        {
            "listeners": {
                "*:8080": {"pass": "routes", "tls": {"certificate": "default"}}
            },
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    )

    assert 'success' in replace_cert(), 'replaced'

    cert_new = ssl.get_server_certificate(('127.0.0.1', 8080))

    temp_dir_old = option.temp_dir
    statedir = f'{temp_dir_old}/state'

    # Main renamed the bundle into place.  No temporary file is left.
    assert [
        name for name in os.listdir(f'{statedir}/certs') if name[0] == '.'
    ] == [], 'no temporary left'

    unit_stop()

    try:
        # The fixture owns the new instance.  The fixture stops it, checks
        # its log, and removes its temp dir.  This test removes the old one.
        unit_run(state_dir=statedir)

        assert cert_new == ssl.get_server_certificate(
            ('127.0.0.1', 8080)
        ), 'replaced bundle survives a restart'

        assert 'chain' in client.conf_get('/certificates/default'), 'listed'

    finally:
        unit_stop()
        shutil.rmtree(temp_dir_old, ignore_errors=True)


def test_tls_certificate_too_large():
    client.load('empty')

    # The bundle is over the 1 MiB limit.  The controller refuses it, not main.
    resp = client.put(
        **client._get_args('/certificates/big', b'-' * (1024 * 1024 + 1))
    )

    assert resp['status'] == 413, resp['body']
    assert 'too large' in resp['body'], 'too large message'

    assert 'error' in client.conf_get('/certificates/big'), 'not stored'


def test_tls_certificate_key_incorrect(skip_alert):
    skip_alert(r'certificate and private key do not match')

    client.load('empty')

    client.certificate('first', False)
    client.certificate('second', False)

    # The bundle is structurally valid (a private key + a certificate), but
    # the key belongs to another certificate.  The store refuses it at
    # upload, so a listener can never name it.
    assert 'error' in client.certificate_load(
        'first', 'second'
    ), 'mismatched bundle refused'

    assert 'error' in client.conf_get('/certificates/first'), 'not stored'


def test_tls_certificate_dot_name():
    client.load('empty')

    client.certificate('default', False)

    # Names starting with "." are reserved for the store's own files.
    for name in ['.', '..', '.default', '.default.tmp']:
        assert 'error' in client.conf(
            b'', f'/certificates/{name}'
        ), f'dot name {name}'

    assert 'success' in client.certificate_load('default'), 'plain name'


def test_tls_certificate_change():
    client.load('empty')

    client.certificate()
    client.certificate('new')

    add_tls()

    cert_old = ssl.get_server_certificate(('127.0.0.1', 8080))

    add_tls(cert='new')

    assert cert_old != ssl.get_server_certificate(
        ('127.0.0.1', 8080)
    ), 'change certificate'


def test_tls_certificate_key_rsa():
    client.load('empty')

    client.certificate()

    assert (
        client.conf_get('/certificates/default/key') == 'RSA (2048 bits)'
    ), 'certificate key rsa'


def test_tls_certificate_key_ec(temp_dir):
    client.load('empty')

    client.openssl_conf()

    subprocess.check_output(
        [
            'openssl',
            'ecparam',
            '-noout',
            '-genkey',
            '-out',
            f'{temp_dir}/ec.key',
            '-name',
            'prime256v1',
        ],
        stderr=subprocess.STDOUT,
    )

    subprocess.check_output(
        [
            'openssl',
            'req',
            '-x509',
            '-new',
            '-subj',
            '/CN=ec/',
            '-config',
            f'{temp_dir}/openssl.conf',
            '-key',
            f'{temp_dir}/ec.key',
            '-out',
            f'{temp_dir}/ec.crt',
        ],
        stderr=subprocess.STDOUT,
    )

    client.certificate_load('ec')

    assert (
        client.conf_get('/certificates/ec/key') == 'ECDH'
    ), 'certificate key ec'


def test_tls_certificate_chain_options(date_to_sec_epoch, sec_epoch):
    client.load('empty')
    date_format = '%b %d %X %Y %Z'

    client.certificate()

    chain = client.conf_get('/certificates/default/chain')

    assert len(chain) == 1, 'certificate chain length'

    cert = chain[0]

    assert (
        cert['subject']['common_name'] == 'default'
    ), 'certificate subject common name'
    assert (
        cert['issuer']['common_name'] == 'default'
    ), 'certificate issuer common name'

    assert (
        abs(
            sec_epoch
            - date_to_sec_epoch(cert['validity']['since'], date_format)
        )
        < 60
    ), 'certificate validity since'
    assert (
        date_to_sec_epoch(cert['validity']['until'], date_format)
        - date_to_sec_epoch(cert['validity']['since'], date_format)
        == 2592000
    ), 'certificate validity until'


def test_tls_certificate_chain(temp_dir):
    client.load('empty')

    client.certificate('root', False)

    req('int')
    req('end')

    generate_ca_conf()

    ca(cert='root', out='int')
    ca(cert='int', out='end')

    crt_path = f'{temp_dir}/end-int.crt'
    end_path = f'{temp_dir}/end.crt'
    int_path = f'{temp_dir}/int.crt'

    with open(crt_path, 'wb') as crt, open(end_path, 'rb') as end, open(
        int_path, 'rb'
    ) as inter:
        crt.write(end.read() + inter.read())

    # incomplete chain

    assert 'success' in client.certificate_load(
        'end', 'end'
    ), 'certificate chain end upload'

    chain = client.conf_get('/certificates/end/chain')
    assert len(chain) == 1, 'certificate chain end length'
    assert (
        chain[0]['subject']['common_name'] == 'end'
    ), 'certificate chain end subject common name'
    assert (
        chain[0]['issuer']['common_name'] == 'int'
    ), 'certificate chain end issuer common name'

    add_tls(cert='end')

    ctx_cert_req = context_cert_req()
    try:
        resp = client.get_ssl(context=ctx_cert_req)
    except ssl.SSLError:
        resp = None

    assert resp is None, 'certificate chain incomplete chain'

    # intermediate

    assert 'success' in client.certificate_load(
        'int', 'int'
    ), 'certificate chain int upload'

    chain = client.conf_get('/certificates/int/chain')
    assert len(chain) == 1, 'certificate chain int length'
    assert (
        chain[0]['subject']['common_name'] == 'int'
    ), 'certificate chain int subject common name'
    assert (
        chain[0]['issuer']['common_name'] == 'root'
    ), 'certificate chain int issuer common name'

    add_tls(cert='int')

    assert client.get_ssl()['status'] == 200, 'certificate chain intermediate'

    # intermediate server

    assert 'success' in client.certificate_load(
        'end-int', 'end'
    ), 'certificate chain end-int upload'

    chain = client.conf_get('/certificates/end-int/chain')
    assert len(chain) == 2, 'certificate chain end-int length'
    assert (
        chain[0]['subject']['common_name'] == 'end'
    ), 'certificate chain end-int int subject common name'
    assert (
        chain[0]['issuer']['common_name'] == 'int'
    ), 'certificate chain end-int int issuer common name'
    assert (
        chain[1]['subject']['common_name'] == 'int'
    ), 'certificate chain end-int end subject common name'
    assert (
        chain[1]['issuer']['common_name'] == 'root'
    ), 'certificate chain end-int end issuer common name'

    add_tls(cert='end-int')

    assert (
        client.get_ssl(context=ctx_cert_req)['status'] == 200
    ), 'certificate chain intermediate server'


def test_tls_certificate_chain_long(temp_dir):
    client.load('empty')

    generate_ca_conf()

    # Minimum chain length is 3.
    chain_length = 10

    for i in range(chain_length):
        if i == 0:
            client.certificate('root', False)
        elif i == chain_length - 1:
            req('end')
        else:
            req(f'int{i}')

    for i in range(chain_length - 1):
        if i == 0:
            ca(cert='root', out='int1')
        elif i == chain_length - 2:
            ca(cert=f'int{(chain_length - 2)}', out='end')
        else:
            ca(cert=f'int{i}', out=f'int{(i + 1)}')

    for i in range(chain_length - 1, 0, -1):
        path = (
            f'{temp_dir}/end.crt'
            if i == chain_length - 1
            else f'{temp_dir}/int{i}.crt'
        )

        with open(f'{temp_dir}/all.crt', 'a', encoding='utf-8') as chain, open(
            path, encoding='utf-8'
        ) as cert:
            chain.write(cert.read())

    assert 'success' in client.certificate_load(
        'all', 'end'
    ), 'certificate chain upload'

    chain = client.conf_get('/certificates/all/chain')
    assert len(chain) == chain_length - 1, 'certificate chain length'

    add_tls(cert='all')

    assert (
        client.get_ssl(context=context_cert_req())['status'] == 200
    ), 'certificate chain long'


def test_tls_certificate_empty_cn():
    client.certificate('root', False)

    req(subject='/')

    generate_ca_conf()
    ca()

    assert 'success' in client.certificate_load('localhost', 'localhost')

    cert = client.conf_get('/certificates/localhost')
    assert cert['chain'][0]['subject'] == {}, 'empty subject'
    assert cert['chain'][0]['issuer']['common_name'] == 'root', 'issuer'


def test_tls_certificate_empty_cn_san():
    client.certificate('root', False)

    client.openssl_conf(
        rewrite=True, alt_names=["example.com", "www.example.net"]
    )

    req(subject='/')

    generate_ca_conf()
    ca()

    assert 'success' in client.certificate_load('localhost', 'localhost')

    cert = client.conf_get('/certificates/localhost')
    assert cert['chain'][0]['subject'] == {
        'alt_names': ['example.com', 'www.example.net']
    }, 'subject alt_names'
    assert cert['chain'][0]['issuer']['common_name'] == 'root', 'issuer'


def test_tls_certificate_empty_cn_san_ip():
    client.certificate('root', False)

    client.openssl_conf(
        rewrite=True,
        alt_names=['example.com', 'www.example.net', 'IP|10.0.0.1'],
    )

    req(subject='/')

    generate_ca_conf()
    ca()

    assert 'success' in client.certificate_load('localhost', 'localhost')

    cert = client.conf_get('/certificates/localhost')
    assert cert['chain'][0]['subject'] == {
        'alt_names': ['example.com', 'www.example.net']
    }, 'subject alt_names'
    assert cert['chain'][0]['issuer']['common_name'] == 'root', 'issuer'


def test_tls_keepalive():
    client.load('mirror')

    assert client.get()['status'] == 200, 'init'

    client.certificate()

    add_tls(application='mirror')

    (resp, sock) = client.post_ssl(
        headers={
            'Host': 'localhost',
            'Connection': 'keep-alive',
        },
        start=True,
        body='0123456789',
        read_timeout=1,
    )

    assert resp['body'] == '0123456789', 'keepalive 1'

    resp = client.post_ssl(
        headers={
            'Host': 'localhost',
            'Connection': 'close',
        },
        sock=sock,
        body='0123456789',
    )

    assert resp['body'] == '0123456789', 'keepalive 2'


def test_tls_no_close_notify():
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

    (_, sock) = client.get_ssl(start=True)

    time.sleep(5)

    sock.close()


def test_tls_write_abrupt_close():
    """Regression: SSL_write busy-loop when client closes mid-response.

    If the router spins, the final get_ssl() will time out and fail.
    Covers both SSL_ERROR_SYSCALL(errno=0) and SSL_ERROR_ZERO_RETURN on
    the write path (issue #28).
    """
    client.load('body_generate')

    client.certificate()

    add_tls(application='body_generate')

    # SSL_write fails with errno=0 → nxt_socket_error_level(0) → NXT_LOG_ALERT.
    # These alerts are expected; suppress them so teardown does not fail.
    # Match only the syscall/zero-return signatures this test provokes,
    # so unrelated SSL_write regressions are not silently masked.
    option.skip_alerts += [
        r'SSL_write\([^)]+\) failed \(0: Success\)',
        r'SSL_write\([^)]+\) failed \(\d+: Connection reset by peer\)',
        r'SSL_write\([^)]+\) failed \(\d+: Broken pipe\)',
    ]

    # Body must exceed the kernel send buffer so the server is still
    # writing when the client tears the connection down.  16 MB beats
    # autotuned SO_SNDBUF on common Linux configurations.
    body_size = 16 * 1024 * 1024

    headers = {
        'Host': 'localhost',
        'Connection': 'close',
        'X-Length': str(body_size),
    }

    # Case 1: abrupt TCP close without TLS close_notify.
    # Triggers SSL_ERROR_SYSCALL(errno=0 or ECONNRESET) on server write.
    sock = client.get_ssl(headers=headers, no_recv=True)
    sock.recv(256)
    sock.close()

    time.sleep(0.2)

    # Case 2: TLS close_notify while server is still writing.
    # Triggers SSL_ERROR_ZERO_RETURN on server write.
    sock = client.get_ssl(headers=headers, no_recv=True)
    sock.recv(256)
    try:
        plain = sock.unwrap()
        plain.close()
    except OSError:
        sock.close()

    time.sleep(0.2)

    # Router must still be responsive — not stuck in a busy-loop.
    assert client.get_ssl(read_timeout=5).get('status') == 200, \
        'router hung after aborted TLS write (issue #28)'


@pytest.mark.skip('not yet')
def test_tls_keepalive_certificate_remove():
    client.load('empty')

    assert client.get()['status'] == 200, 'init'

    client.certificate()

    add_tls()

    (resp, sock) = client.get_ssl(
        headers={'Host': 'localhost', 'Connection': 'keep-alive'},
        start=True,
        read_timeout=1,
    )

    assert 'success' in client.conf(
        {"pass": "applications/empty"}, 'listeners/*:8080'
    )
    assert 'success' in client.conf_delete('/certificates/default')

    try:
        resp = client.get_ssl(sock=sock)

    except KeyboardInterrupt:
        raise

    except:
        resp = None

    assert resp is None, 'keepalive remove certificate'


@pytest.mark.skip('not yet')
def test_tls_certificates_remove_all():
    client.load('empty')

    client.certificate()

    assert 'success' in client.conf_delete(
        '/certificates'
    ), 'remove all certificates'


def test_tls_application_respawn(findall, skip_alert, wait_for_record):
    client.load('mirror')

    client.certificate()

    assert 'success' in client.conf('1', 'applications/mirror/processes')

    add_tls(application='mirror')

    (_, sock) = client.post_ssl(
        headers={
            'Host': 'localhost',
            'Connection': 'keep-alive',
        },
        start=True,
        body='0123456789',
        read_timeout=1,
    )

    app_id = findall(r'(\d+)#\d+ "mirror" application started')[0]

    subprocess.check_output(['kill', '-9', app_id])

    skip_alert(fr'process {app_id} exited on signal 9')

    wait_for_record(fr' (?!{app_id}#)(\d+)#\d+ "mirror" application started')

    resp = client.post_ssl(sock=sock, body='0123456789')

    assert resp['status'] == 200, 'application respawn status'
    assert resp['body'] == '0123456789', 'application respawn body'


def test_tls_url_scheme():
    client.load('variables')

    assert (
        client.post(
            headers={
                'Host': 'localhost',
                'Content-Type': 'text/html',
                'Custom-Header': '',
                'Connection': 'close',
            }
        )['headers']['Wsgi-Url-Scheme']
        == 'http'
    ), 'url scheme http'

    client.certificate()

    add_tls(application='variables')

    assert (
        client.post_ssl(
            headers={
                'Host': 'localhost',
                'Content-Type': 'text/html',
                'Custom-Header': '',
                'Connection': 'close',
            }
        )['headers']['Wsgi-Url-Scheme']
        == 'https'
    ), 'url scheme https'


def test_tls_big_upload():
    client.load('upload')

    client.certificate()

    add_tls(application='upload')

    filename = 'test.txt'
    data = '0123456789' * 9000

    res = client.post_ssl(
        body={
            'file': {
                'filename': filename,
                'type': 'text/plain',
                'data': io.StringIO(data),
            }
        }
    )
    assert res['status'] == 200, 'status ok'
    assert res['body'] == f'{filename}{data}'


def test_tls_multi_listener():
    client.load('empty')

    client.certificate()

    add_tls()
    add_tls(port=8081)

    assert client.get_ssl()['status'] == 200, 'listener #1'

    assert client.get_ssl(port=8081)['status'] == 200, 'listener #2'


def test_tls_certificate_cstring_nul():
    client.load('empty')
    client.certificate()

    # The "certificate" name is used as a NUL-terminated C-string store name;
    # an embedded NUL (which survives JSON parsing in a length-tracked
    # nxt_str_t) or an empty value must be rejected by the c-string validator,
    # before the certificate-store lookup.
    def conf_cert(cert):
        return client.conf(
            {"pass": "applications/empty", "tls": {"certificate": cert}},
            'listeners/*:8080',
        )

    # The certificate-store lookup is length-aware and would also reject
    # these values (as "not found"), so assert the validator's own diagnostic
    # to prove the c-string guard ran rather than the lookup merely failing.
    assert 'success' in conf_cert("default"), 'valid'

    resp = conf_cert("default\0junk")
    assert 'null character' in resp.get('detail', ''), 'nul'

    resp = conf_cert("")
    assert 'must not be empty' in resp.get('detail', ''), 'empty'

    resp = conf_cert(["default\0junk"])
    assert 'null character' in resp.get('detail', ''), 'array nul'
