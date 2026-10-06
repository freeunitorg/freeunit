import io
import os
import shutil
import socket
import ssl
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
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


def test_tls_certificate_update_unchanged():
    client.load('empty')

    client.certificate()

    # Tickets are off by default.  With "tickets": true and no key in the
    # configuration, the key is a random one of the SSL_CTX that issued the
    # ticket.  A reconfiguration builds a new context with a new key, so a
    # resumed session proves that the listener kept its context.
    assert 'success' in client.conf(
        {
            "pass": "applications/empty",
            "tls": {"certificate": "default", "session": {"tickets": True}},
        },
        'listeners/*:8080',
    )

    path = f'{option.temp_dir}/state/certs/default'
    stored = os.stat(path)

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    context.maximum_version = ssl.TLSVersion.TLSv1_2

    def connect(session=None):
        with socket.create_connection(('127.0.0.1', 8080)) as sock:
            with context.wrap_socket(sock, session=session) as ssock:
                ssock.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n')

                resp = b''
                while b'\r\n\r\n' not in resp:
                    resp += ssock.recv(4096)

                assert resp.startswith(b'HTTP/1.1 200'), 'request'

                return ssock.session, ssock.session_reused

    try:
        session, reused = connect()
    except ssl.SSLError as e:
        # Ticket resumption needs TLS 1.2.  A system crypto policy can
        # turn it off.
        pytest.skip(f'TLS 1.2 is not available: {e}')

    assert not reused, 'new session'

    _, reused = connect(session)
    assert reused, 'session resumed'

    # The same bundle again.  The answer is the one a store gives, but
    # main writes nothing, and the router keeps its contexts.
    assert (
        client.certificate_load('default').get('success')
        == 'Certificate chain updated.'
    ), 'unchanged in use'

    stored_default = os.stat(path)
    assert (stored.st_ino, stored.st_mtime_ns) == (
        stored_default.st_ino,
        stored_default.st_mtime_ns,
    ), 'not stored again'

    _, reused = connect(session)
    assert reused, 'no reconfiguration'

    # The same bundle under a name no listener uses.
    client.certificate('unused')

    path_unused = f'{option.temp_dir}/state/certs/unused'
    stored = os.stat(path_unused)

    assert (
        client.certificate_load('unused').get('success')
        == 'Certificate chain uploaded.'
    ), 'unchanged unused'

    after = os.stat(path_unused)
    assert stored.st_ino == after.st_ino, 'unused not stored again'

    # A new certificate is stored and applied, as before.
    assert (
        replace_cert().get('success') == 'Certificate chain updated.'
    ), 'replaced'

    assert os.stat(path).st_ino != stored_default.st_ino, 'new bundle stored'

    _, reused = connect(session)
    assert not reused, 'reconfigured'


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


def test_tls_certificate_upload_stored():
    """The 200 of an upload comes only when main has stored the bundle.
    Main stores it in a child process
    (https://github.com/freeunitorg/freeunit/issues/516) and answers when
    the child exits.  Two uploads sent at the same time both land."""

    names = ('first', 'second', 'third')

    bundles = {}

    for name in names:
        client.certificate(name, False)

        with open(f'{option.temp_dir}/{name}.key', 'rb') as k, open(
            f'{option.temp_dir}/{name}.crt', 'rb'
        ) as c:
            bundles[name] = k.read() + c.read()

    certs = Path(f'{option.temp_dir}/state/certs')

    # One upload.  The file is complete when the answer arrives.
    assert 'success' in client.conf(
        bundles['first'], '/certificates/first'
    ), 'first uploaded'
    assert (certs / 'first').read_bytes() == bundles['first'], 'first stored'

    # Two uploads at the same time.  The controller runs one after the
    # other, and main stores each one.
    def upload(name):
        return ApplicationTLS().conf(bundles[name], f'/certificates/{name}')

    with ThreadPoolExecutor(max_workers=2) as pool:
        answers = list(pool.map(upload, ('second', 'third')))

    for name, answer in zip(('second', 'third'), answers):
        assert 'success' in answer, f'{name} uploaded'
        assert (certs / name).read_bytes() == bundles[name], f'{name} stored'

    assert set(names) <= set(client.conf_get('/certificates')), 'listed'
    assert not (certs / '.store.tmp').exists(), 'no temporary file'


def unit_children(unit_pid, name):
    out = subprocess.check_output(
        ['ps', 'ax', '-o', 'pid=,ppid=,args=']
    ).decode()
    return [
        f[0]
        for f in (line.split(None, 2) for line in out.splitlines())
        if len(f) == 3 and f[1] == str(unit_pid) and name in f[2]
    ]


def trace_fsync(unit_pid):
    """Delay each fsync(2) of main and its children by 2 s with strace.
    Skip the test when strace is not installed or cannot attach."""

    strace = shutil.which('strace')
    if strace is None:
        pytest.skip('strace is not installed')

    tracer = subprocess.Popen(
        [
            strace,
            '-f',
            '-p',
            str(unit_pid),
            '-o',
            '/dev/null',
            '-e',
            'trace=fsync',
            '-e',
            'inject=fsync:delay_exit=2000000',
        ],
        stderr=subprocess.PIPE,
    )

    # strace says "Process N attached" on stderr once it traces main.
    line = tracer.stderr.readline().decode()
    if 'attached' not in line:
        tracer.kill()
        tracer.wait()
        pytest.skip(f'strace cannot attach: {line.strip()}')

    return tracer


def test_tls_certificate_upload_router_restart(
    skip_alert, skip_fds_check, unit_pid
):
    """A router that restarts while main stores a bundle does not make the
    controller run the upload again.  The upload gets one 200, a change
    sent during the store runs after it, and the controller survives.

    strace delays each fsync(2) of main and its children by 2 s, so the
    store child of the upload runs for about 4 s."""

    def children(name):
        return unit_children(unit_pid, name)

    client.certificate('slow', False)

    with open(f'{option.temp_dir}/slow.key', 'rb') as k, open(
        f'{option.temp_dir}/slow.crt', 'rb'
    ) as c:
        bundle = k.read() + c.read()

    controller = children('unit: controller')
    router = children('unit: router')
    assert len(controller) == 1 and len(router) == 1, 'processes'

    tracer = trace_fsync(unit_pid)

    done = {}
    stored = Path(f'{option.temp_dir}/state/certs/slow')

    def upload():
        body = ApplicationTLS().conf(bundle, '/certificates/slow')
        on_disk = stored.exists() and stored.read_bytes() == bundle
        done['upload'] = (body, on_disk)

    def change():
        start = time.monotonic()
        body = ApplicationTLS().conf(
            {"http": {"idle_timeout": 77}}, 'settings'
        )
        done['change'] = (time.monotonic() - start, body)

    try:
        with ThreadPoolExecutor(max_workers=2) as pool:
            first = pool.submit(upload)

            # The store child is in its first fsync(2) now.
            time.sleep(1)

            skip_fds_check(router=True)
            skip_alert(fr'process {router[0]} exited on signal 9')
            subprocess.call(['kill', '-9', router[0]])

            for _ in range(50):
                new = children('unit: router')
                if new and new != router:
                    break
                time.sleep(0.1)

            # Let the new router get the configuration.
            time.sleep(0.5)

            second = pool.submit(change)

            first.result(timeout=30)
            second.result(timeout=30)

    finally:
        tracer.terminate()
        tracer.wait(10)

    assert 'success' in done['upload'][0], 'one 200 for the upload'
    assert done['upload'][1], 'the bundle is on disk at the 200'
    assert 'success' in done['change'][1], 'the change is applied'

    # The store runs about 4 s, and it started about 1.5 s before the
    # change was sent.  A change that did not wait takes milliseconds.
    assert done['change'][0] > 1, 'the change waited for the store'

    # Main has answered the upload; the controller must still run.
    time.sleep(0.5)

    assert children('unit: controller') == controller, 'controller survived'
    assert client.conf_get('settings/http/idle_timeout') == 77


def test_tls_certificate_update_router_restart(
    skip_alert, skip_fds_check, unit_pid
):
    """A listener uses the bundle, and the router restarts while main
    stores it.  The store ends while the new router still applies its
    first configuration, because the application takes 8 s to start.  The
    controller sends the configuration again only after that apply ends.
    Before, it sent it at once.  The two applies ran at the same time in
    the router: the upload got 500, the listener stopped answering, and the
    next change crashed the router.

    strace delays each fsync(2) of main and its children by 2 s, so the
    store child of the upload runs for about 4 s."""

    def children(name):
        return unit_children(unit_pid, name)

    bundles = {}

    for name in ('old', 'new'):
        client.certificate(name, False)

        with open(f'{option.temp_dir}/{name}.key', 'rb') as k, open(
            f'{option.temp_dir}/{name}.crt', 'rb'
        ) as c:
            bundles[name] = k.read() + c.read()

    def served():
        pem = ssl.get_server_certificate(('127.0.0.1', 8080), timeout=10)
        return ssl.PEM_cert_to_DER_cert(pem)

    def der(name):
        pem = Path(f'{option.temp_dir}/{name}.crt').read_text(encoding='utf-8')
        return ssl.PEM_cert_to_DER_cert(pem)

    assert 'success' in client.conf(bundles['old'], '/certificates/c')

    app = f'{option.test_dir}/python/slow_start'

    assert 'success' in client.conf(
        {
            "listeners": {
                "*:8080": {
                    "pass": "applications/slow_start",
                    "tls": {"certificate": "c"},
                }
            },
            "applications": {
                "slow_start": {
                    "type": "python",
                    "processes": 1,
                    "path": app,
                    "working_directory": app,
                    "module": "wsgi",
                    "environment": {"UNIT_SLOW_START": "8"},
                }
            },
        }
    )

    assert served() == der('old'), 'the old certificate before'

    router = children('unit: router')
    assert len(router) == 1, 'one router'

    stored = Path(f'{option.temp_dir}/state/certs/c')

    tracer = trace_fsync(unit_pid)

    done = {}

    def upload():
        start = time.monotonic()
        body = ApplicationTLS().conf(bundles['new'], '/certificates/c')
        done['upload'] = (time.monotonic() - start, body)

    try:
        with ThreadPoolExecutor(max_workers=1) as pool:
            first = pool.submit(upload)

            # The store child is in its first fsync(2) now.
            time.sleep(1)

            assert stored.read_bytes() == bundles['old'], 'store not done'

            skip_fds_check(router=True)
            skip_alert(fr'process {router[0]} exited on signal 9')
            subprocess.call(['kill', '-9', router[0]])

            first.result(timeout=60)

    finally:
        tracer.terminate()
        tracer.wait(10)

    elapsed, body = done['upload']

    assert body.get('success') == 'Certificate chain updated.', 'applied'

    # The store ends after about 4 s.  The first configuration of the new
    # router ends after the application starts, 8 s after the kill.
    assert elapsed > 6, 'the upload waited for the first configuration'

    assert served() == der('new'), 'the new certificate'

    router = children('unit: router')
    assert len(router) == 1, 'one new router'

    assert 'success' in client.conf(
        {"http": {"idle_timeout": 77}}, 'settings'
    ), 'a later change'

    assert served() == der('new'), 'the new certificate after the change'
    assert client.get_ssl()['status'] == 200, 'the listener answers'
    assert children('unit: router') == router, 'the router survived'


def test_tls_certificate_update_controller_restart(
    skip_alert, skip_fds_check, unit_pid
):
    """A listener uses the bundle, and the controller exits while main
    stores a new one.  Main starts the new controller only when the store
    ends, so the new controller reads the new bundle from disk.  It lists
    the new certificate, and the router serves it.  When main started the
    controller at once, the new controller read the old bundle.  The answer
    of the store went to the controller that had gone, so the router served
    the old certificate after the store ended.

    strace delays each fsync(2) of main and its children by 2 s, so the
    store child of the upload runs for about 4 s."""

    def children(name):
        return unit_children(unit_pid, name)

    bundles = {}

    for name in ('old', 'new'):
        client.certificate(name, False)

        with open(f'{option.temp_dir}/{name}.key', 'rb') as k, open(
            f'{option.temp_dir}/{name}.crt', 'rb'
        ) as c:
            bundles[name] = k.read() + c.read()

    def served():
        pem = ssl.get_server_certificate(('127.0.0.1', 8080), timeout=10)
        return ssl.PEM_cert_to_DER_cert(pem)

    def der(name):
        pem = Path(f'{option.temp_dir}/{name}.crt').read_text(encoding='utf-8')
        return ssl.PEM_cert_to_DER_cert(pem)

    client.load('empty')

    assert 'success' in client.conf(bundles['old'], '/certificates/c')

    add_tls(cert='c')

    assert served() == der('old'), 'the old certificate before'

    controller = children('unit: controller')
    assert len(controller) == 1, 'one controller'

    stored = Path(f'{option.temp_dir}/state/certs/c')

    tracer = trace_fsync(unit_pid)

    done = {}

    def upload():
        done['upload'] = ApplicationTLS().put(
            url='/certificates/c',
            sock_type='unix',
            addr=f'{option.temp_dir}/control.unit.sock',
            body=bundles['new'],
        )

    try:
        with ThreadPoolExecutor(max_workers=1) as pool:
            first = pool.submit(upload)

            # The store child is in its first fsync(2) now.
            time.sleep(1)

            assert stored.read_bytes() == bundles['old'], 'store not done'

            skip_fds_check(controller=True)
            skip_alert(fr'process {controller[0]} exited on signal 9')
            subprocess.call(['kill', '-9', controller[0]])

            first.result(timeout=30)

            # The store child is a fork of main, with the title of main.
            for _ in range(100):
                if not children('unit: main'):
                    break
                time.sleep(0.1)

            assert not children('unit: main'), 'the store ended'

            for _ in range(100):
                new = children('unit: controller')
                if new and new != controller:
                    break
                time.sleep(0.1)

            assert new and new != controller, 'a new controller'

    finally:
        tracer.terminate()
        tracer.wait(10)

    assert done['upload'] == {}, 'the controller exited before it answered'
    assert stored.read_bytes() == bundles['new'], 'the new bundle on disk'

    # The new controller listens after the router applies its configuration.
    assert (
        client.conf_get('/certificates/c/chain/0/subject/common_name')
        == 'new'
    ), 'the controller lists the new certificate'

    assert served() == der('new'), 'the router serves the new certificate'
    assert client.get_ssl()['status'] == 200, 'the listener answers'


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

    # One byte more cannot be a file name.  The controller refuses it before
    # main sees it, so the answer is 400 and not 500.
    assert (
        client.conf(bundle, f'/certificates/{name}a').get('error')
        == 'Invalid certificate name.'
    ), 'name too long'

    assert 'error' in client.conf_get(f'/certificates/{name}a'), 'not stored'


def test_tls_certificate_fingerprint():
    client.certificate()

    # The same value that the OpenSSL command line prints, after the "=".
    out = subprocess.check_output(
        [
            'openssl',
            'x509',
            '-noout',
            '-fingerprint',
            '-sha256',
            '-in',
            f'{option.temp_dir}/default.crt',
        ]
    )

    expected = out.decode().split('=', 1)[1].strip()

    assert len(expected) == 95, 'openssl fingerprint'

    assert (
        client.conf_get('/certificates/default/fingerprint') == expected
    ), 'fingerprint'

    assert (
        client.conf_get('/certificates')['default']['fingerprint'] == expected
    ), 'fingerprint listed'

    # A new certificate under the same name gets a new fingerprint.
    assert 'success' in replace_cert(), 'replaced'

    assert (
        client.conf_get('/certificates/default/fingerprint') != expected
    ), 'fingerprint changed'


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
