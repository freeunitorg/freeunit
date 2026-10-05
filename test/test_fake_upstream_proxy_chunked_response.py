"""Proxy chunked-*response* relay regression tests (FreeUnit #72).

The mirror direction of test_proxy_chunked.py: there the client sends a chunked
*request*; here the upstream sends a chunked *response* (no Content-Length) and
FreeUnit must relay the full body to the client without truncation, over both a
plain and a TLS listener.

Driven by the `chunked-stream` mode of the Rust mock upstream
(test/fake_upstream/). Deliberately language-module-free — gated only on the
built-in `openssl` support — so it runs on the minimal `--openssl --tests`
build used for fast prototyping (see test/README.md).

H1 (plain, *:8080)  isolates the chunked-relay parser
                    (nxt_http_chunk_parse.c / nxt_h1proto.c).
H2 (TLS,   *:8443)  adds the TLS send path (nxt_openssl.c) — reproduces the
                    Gitea `git clone` incident.

NOTE: we read the response ourselves (no_recv=True) into a bytearray instead of
the shared harness recvall/_parse_chunked_body — both are O(n^2) and choke on a
64 MiB relay (recvall reallocates an immutable bytes per recv; over TLS a recv
returns one ~16 KB record, so the cost explodes).
"""

import os
import socket
import ssl
import struct
import subprocess
import time

import pytest

from unit.applications.tls import ApplicationTLS
from unit.utils import waitforsocket

prerequisites = {'modules': {'openssl': 'any'}}

client = ApplicationTLS()

# Mirror of fake_upstream PATTERN / response size. The incident packfile was
# ~52 MiB; 64 MiB stays comfortably past the 2 MiB ceiling of the request tests
# and forces many read-buffer refills on the relay path.
PATTERN = b'0123456789abcdef'
SIZE_MIB = 64
SIZE = SIZE_MIB * 1024 * 1024

# Fixed upstream ports reserved for this case in test/fake_upstream/README.md.
# One documented port per test → no cross-test collisions, greppable alongside
# the `chunked-response` mode / `respond_chunked_response` Rust handler.
UPSTREAM_PORT = 7994        # plain + tls relay tests
UPSTREAM_ABORT_PORT = 7995  # client-abort mid-write test (same upstream behavior)
UPSTREAM_ABORT_MID_PORT = 7996  # upstream dies mid-stream (abort-mid)
UPSTREAM_SLOW_DRIP_PORT = 7997  # one small chunk every N ms (slow-drip)
UPSTREAM_DUP_TE_PORT = 7998     # duplicate Transfer-Encoding header (dup-te)

FAKE_UPSTREAM_BIN = '/usr/local/bin/fake_upstream'

_skipif_no_fake_upstream = pytest.mark.skipif(
    not os.path.exists(FAKE_UPSTREAM_BIN),
    reason=f'{FAKE_UPSTREAM_BIN} not installed (build via test/fake_upstream)',
)


def _run_chunked_response(port=UPSTREAM_PORT, size_mib=SIZE_MIB):
    proc = subprocess.Popen(
        [
            FAKE_UPSTREAM_BIN,
            '--port',
            str(port),
            '--mode',
            'chunked-response',
            '--size',
            str(size_mib),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    waitforsocket(port)
    return proc


def _run_mode(port, mode, size_mib=1, delay_ms=0):
    args = [FAKE_UPSTREAM_BIN, '--port', str(port), '--mode', mode,
            '--size', str(size_mib)]
    if delay_ms:
        args += ['--delay-ms', str(delay_ms)]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    waitforsocket(port)
    return proc


def _recv_all(sock, timeout=120):
    """Read until EOF into a bytearray (amortized O(1) append, unlike recvall)."""
    sock.settimeout(timeout)
    buf = bytearray()
    while True:
        try:
            part = sock.recv(1 << 20)
        except (TimeoutError, socket.timeout, ssl.SSLError, OSError):
            break
        if not part:
            break
        buf += part
    return buf


def _dechunk(body):
    """Single-pass chunked decoder over bytes; O(n) (bytes.index is C-level)."""
    out = bytearray()
    i = 0
    n = len(body)
    while i < n:
        try:
            j = body.index(b'\r\n', i)
        except ValueError:
            raise ValueError(
                f'_dechunk: no CRLF after offset {i} '
                f'(body len={n}, tail={bytes(body[i:i+40])!r})'
            )
        size = int(body[i:j].split(b';', 1)[0], 16)
        i = j + 2
        if size == 0:
            break
        out += body[i : i + size]
        i += size + 2  # skip chunk data + trailing CRLF
    return bytes(out)


def _assert_relayed(raw):
    """Split a raw HTTP response, dechunk if needed, assert byte-exact relay."""
    assert raw[:12] == b'HTTP/1.1 200', f'status line: {raw[:40]!r}'

    sep = raw.index(b'\r\n\r\n')
    head = raw[:sep]
    body = bytes(raw[sep + 4 :])

    if b'transfer-encoding: chunked' in head.lower():
        body = _dechunk(body)

    # A truncated relay shows up as a short body (early EOF); the length assert
    # pins it. PATTERN (16 B) divides SIZE exactly, so expected is one repeat.
    assert len(body) == SIZE, (
        f'relayed body truncated: got {len(body)} of {SIZE} bytes'
    )
    assert body == PATTERN * (SIZE // len(PATTERN)), 'relayed body mismatch'


@_skipif_no_fake_upstream
def test_proxy_chunked_response_plain():
    """64 MiB chunked response relayed over a plain listener. (#72 H1)"""
    proc = _run_chunked_response()
    try:
        assert 'success' in client.conf(
            {
                "listeners": {"*:8080": {"pass": "routes"}},
                "routes": [
                    {"action": {"proxy": f'http://127.0.0.1:{UPSTREAM_PORT}'}}
                ],
            }
        ), 'plain proxy configuration'

        sock = client.get(port=8080, no_recv=True)
        try:
            _assert_relayed(_recv_all(sock))
        finally:
            sock.close()
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_chunked_response_tls():
    """64 MiB chunked response relayed over a TLS listener. (#72 H2 — Gitea path)"""
    proc = _run_chunked_response()
    try:
        client.certificate()

        assert 'success' in client.conf(
            {
                "listeners": {
                    "*:8443": {
                        "pass": "routes",
                        "tls": {"certificate": "default"},
                    }
                },
                "routes": [
                    {"action": {"proxy": f'http://127.0.0.1:{UPSTREAM_PORT}'}}
                ],
            }
        ), 'TLS proxy configuration'

        sock = client.get_ssl(port=8443, no_recv=True)
        try:
            _assert_relayed(_recv_all(sock))
        finally:
            sock.close()
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_chunked_response_client_abort(findall):
    """Client RST mid-TLS-write must not log SSL_write broken pipe at [alert].

    Reproduces the Gitea `git clone` incident (#72 case 7): the client aborts
    while FreeUnit is still relaying the chunked body, so the next SSL_write to
    the client fails with EPIPE. On OpenSSL 3.x the broken pipe lands in the
    error *queue* (errno reads EAGAIN), so nxt_openssl_log_error_level() misroutes
    the system-library reason through the SSL_R_* switch and hits the NXT_LOG_ALERT
    default. A plain client disconnect must never be an alert.

    conftest's check_alerts() fails the test on any unexpected [alert]; the
    explicit assert below names the exact offender for clarity.
    """
    proc = _run_chunked_response(port=UPSTREAM_ABORT_PORT)
    try:
        client.certificate()

        assert 'success' in client.conf(
            {
                "listeners": {
                    "*:8443": {
                        "pass": "routes",
                        "tls": {"certificate": "default"},
                    }
                },
                "routes": [
                    {"action": {"proxy": f'http://127.0.0.1:{UPSTREAM_ABORT_PORT}'}}
                ],
            }
        ), 'client-abort proxy configuration'

        sock = client.get_ssl(port=8443, no_recv=True)
        # Read one record so the relay is mid-stream, then hard-close: SO_LINGER
        # with a 0 timeout sends a RST instead of a graceful FIN, so the router's
        # next SSL_write to the client deterministically hits a broken pipe.
        sock.settimeout(10)
        sock.recv(16384)
        sock.setsockopt(
            socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)
        )
        sock.close()

        # Give the router time to attempt the next write and log the result.
        time.sleep(1)

        assert not findall(r'\[alert\].*SSL_write.*failed'), (
            'client disconnect logged SSL_write failure at [alert] '
            '(nxt_openssl_log_error_level misroutes system-library EPIPE)'
        )
        assert findall(r'\[info\].*SSL_write.*failed'), (
            'SSL_write broken-pipe path never fired — '
            'fix in nxt_openssl_log_error_level may not have been exercised'
        )
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_chunked_response_abort_mid():
    """Upstream dies mid-chunked-stream: client sees truncation, router survives.

    The upstream writes 4 MiB of chunks then closes the socket without the
    terminal 0-chunk (#72 case 4). FreeUnit must relay what it received and
    close cleanly — never hang or crash the router (enforced by conftest's
    pid/alert checks). The relayed stream must therefore be truncated: it must
    not end with a valid terminal chunk.
    """
    proc = _run_mode(UPSTREAM_ABORT_MID_PORT, 'abort-mid', size_mib=4)
    try:
        assert 'success' in client.conf(
            {
                "listeners": {"*:8080": {"pass": "routes"}},
                "routes": [
                    {"action": {"proxy": f'http://127.0.0.1:{UPSTREAM_ABORT_MID_PORT}'}}
                ],
            }
        ), 'abort-mid proxy configuration'

        sock = client.get(port=8080, no_recv=True)
        try:
            raw = _recv_all(sock, timeout=30)
        finally:
            sock.close()

        assert raw[:12] == b'HTTP/1.1 200', f'status line: {raw[:40]!r}'
        assert len(raw) > 200, f'suspiciously short response: {len(raw)} bytes'
        # No terminal chunk: the relay was truncated, not completed.
        assert not raw.endswith(b'0\r\n\r\n'), 'aborted relay ended with terminal chunk'
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_chunked_response_slow_drip():
    """Slow chunked relay (drip << proxy_read_timeout) completes byte-exact.

    Upstream emits one 512-byte chunk every 1 ms (#72 case 5). Well under the
    default proxy read timeout, so the relay must complete intact.
    """
    size = 1 * 1024 * 1024
    proc = _run_mode(UPSTREAM_SLOW_DRIP_PORT, 'slow-drip', size_mib=1, delay_ms=1)
    try:
        assert 'success' in client.conf(
            {
                "listeners": {"*:8080": {"pass": "routes"}},
                "routes": [
                    {"action": {"proxy": f'http://127.0.0.1:{UPSTREAM_SLOW_DRIP_PORT}'}}
                ],
            }
        ), 'slow-drip proxy configuration'

        sock = client.get(port=8080, no_recv=True)
        try:
            raw = _recv_all(sock, timeout=60)
        finally:
            sock.close()

        assert raw[:12] == b'HTTP/1.1 200', f'status line: {raw[:40]!r}'
        sep = raw.index(b'\r\n\r\n')
        body = _dechunk(bytes(raw[sep + 4 :]))
        assert len(body) == size, f'slow-drip body truncated: {len(body)} of {size}'
        assert body == PATTERN * (size // len(PATTERN)), 'slow-drip body mismatch'
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_chunked_response_dup_te():
    """Duplicate upstream Transfer-Encoding must not be relayed twice (#1088).

    Upstream sends `Transfer-Encoding: chunked` twice with a valid chunked body
    (#72 case 6). The client must see at most one Transfer-Encoding header; if
    the response is relayed as 200/chunked, the body must still decode once,
    byte-exact.
    """
    size = 1 * 1024 * 1024
    proc = _run_mode(UPSTREAM_DUP_TE_PORT, 'dup-te', size_mib=1)
    try:
        assert 'success' in client.conf(
            {
                "listeners": {"*:8080": {"pass": "routes"}},
                "routes": [
                    {"action": {"proxy": f'http://127.0.0.1:{UPSTREAM_DUP_TE_PORT}'}}
                ],
            }
        ), 'dup-te proxy configuration'

        sock = client.get(port=8080, no_recv=True)
        try:
            raw = _recv_all(sock, timeout=30)
        finally:
            sock.close()

        assert raw[:12] in (b'HTTP/1.1 200', b'HTTP/1.1 502', b'HTTP/1.1 400'), (
            f'unexpected status: {raw[:40]!r}'
        )
        sep = raw.index(b'\r\n\r\n')
        head = raw[:sep].lower()
        te_count = head.count(b'transfer-encoding')
        assert te_count <= 1, f'client saw {te_count} Transfer-Encoding headers'

        if raw[:12] == b'HTTP/1.1 200' and b'transfer-encoding: chunked' in head:
            body = _dechunk(bytes(raw[sep + 4 :]))
            assert len(body) == size, f'dup-te body truncated: {len(body)} of {size}'
            assert body == PATTERN * (size // len(PATTERN)), 'dup-te body mismatch'
    finally:
        proc.terminate()
        proc.wait()
