"""Proxy duplicate upstream Content-Length regression test (#113).

An upstream that sends a second Content-Length header is a classic
response-smuggling primitive: the two values disagree on where the body ends,
so a proxy that forwards them lets the client and any intermediary re-frame
the body against each other. The hardening in nxt_http_proxy_content_length
logs a warning, sets skip=1 on BOTH Content-Length fields so neither is
forwarded, and resets content_length_n to -1 so the body is framed by
read-to-EOF instead of an ambiguous advertised length.

Client-observable outcome, confirmed against the running router: a 200
response carrying NO Content-Length header at all -- FreeUnit re-frames the
body itself as Transfer-Encoding: chunked -- with the exact upstream body
bytes and a complete terminal chunk. The client never sees the two ambiguous
lengths, so there is no framing disagreement to exploit.

Note on the inconsistent flag: nxt_http_proxy_content_length sets
r->inconsistent so the downstream keepalive is disabled. This was previously
clobbered -- nxt_h1p_peer_closed reassigned the flag from the (already reset)
framing state on the clean upstream close, so the connection stayed
keep-alive. That is fixed: peer_closed now only *sets* a separate r->truncated
(which drives the terminal-chunk omission) and never clears r->inconsistent,
so a complete-but-ambiguous body disables keepalive while keeping its terminal
chunk. test_proxy_dup_cl_keepalive_disabled below asserts that directly. The
first two tests drive `Connection: close` and assert the primary defense on
the wire: zero conflicting Content-Length headers reach the client and the
relayed body framing is unambiguous.

Driven by the `dup-cl` mode of the Rust mock upstream (test/fake_upstream/):
it sends `Content-Length: 20` then `Content-Length: 6`, followed by a 20-byte
deterministic body, then closes. This test was deferred from #113 to land
with the fake_upstream harness (#100). Language-module-free (gated only on
built-in proxy support) so it runs on the minimal test build.
"""

import os
import socket
import subprocess

import pytest

from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# Deterministic body: byte at global offset i is PATTERN[i % 16] — mirrors the
# fake_upstream PATTERN so the test regenerates the exact upstream bytes.
PATTERN = '0123456789abcdef'

# Must match DUP_CL_FIRST / DUP_CL_SECOND in test/fake_upstream/src/main.rs:
# the first Content-Length advertises the real body length, the second a
# shorter, conflicting one.
DUP_CL_FIRST = 20
DUP_CL_SECOND = 6

BODY = (PATTERN * (DUP_CL_FIRST // len(PATTERN) + 1))[:DUP_CL_FIRST]

# Reserved fake_upstream ports for these cases (see test/fake_upstream/README.md).
UPSTREAM_DUP_CL_PORT = 7984
UPSTREAM_DUP_CL_RAW_PORT = 7983
UPSTREAM_DUP_CL_KA_PORT = 7979

FAKE_UPSTREAM_BIN = '/usr/local/bin/fake_upstream'

_skipif_no_fake_upstream = pytest.mark.skipif(
    not os.path.exists(FAKE_UPSTREAM_BIN),
    reason=f'{FAKE_UPSTREAM_BIN} not installed (build via test/fake_upstream)',
)


def _run(port, mode):
    proc = subprocess.Popen(
        [FAKE_UPSTREAM_BIN, '--port', str(port), '--mode', mode],
        stdout=subprocess.DEVNULL,
        # DEVNULL, not PIPE: nothing reads this stream, and an unread PIPE can
        # deadlock the child if it ever fills the OS pipe buffer.
        stderr=subprocess.DEVNULL,
    )
    # Tear the process down if it never binds — otherwise a waitforsocket
    # timeout would raise before the caller's try/finally and leak it.
    try:
        waitforsocket(port)
    except Exception:
        proc.terminate()
        proc.wait()
        raise
    return proc


def _dechunk(raw):
    # Minimal chunked-body decoder for the raw test; fails loudly on any
    # malformed framing (ValueError / IndexError fail the test).
    body = b''
    while True:
        line, raw = raw.split(b'\r\n', 1)
        size = int(line.split(b';')[0], 16)
        if size == 0:
            break
        body += raw[:size]
        raw = raw[size + 2:]
    return body


def _conf_proxy(port):
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {"action": {"proxy": f'http://127.0.0.1:{port}'}}
            ],
        }
    ), 'dup-cl proxy configuration'


@_skipif_no_fake_upstream
def test_proxy_dup_cl(skip_alert):
    # The router logs a warning about the duplicate upstream Content-Length;
    # it is [warn] not [alert], but suppress it defensively.
    skip_alert(r'upstream sent duplicate Content-Length')

    proc = _run(UPSTREAM_DUP_CL_PORT, 'dup-cl')
    try:
        _conf_proxy(UPSTREAM_DUP_CL_PORT)

        resp = client.get(port=8080)

        assert resp['status'] == 200, f'unexpected status: {resp}'

        # Neither conflicting Content-Length may reach the client: both
        # upstream fields are skipped and FreeUnit re-frames the body itself
        # (read-to-EOF, emitted as Transfer-Encoding: chunked).
        assert 'Content-Length' not in resp['headers'], (
            f'conflicting upstream Content-Length must not be forwarded: {resp}'
        )
        assert (
            resp['headers'].get('Transfer-Encoding') == 'chunked'
        ), f'body must be re-framed by FreeUnit: {resp}'

        # The body itself is relayed intact (the harness de-chunks it).
        assert resp['body'] == BODY, f'body not relayed intact: {resp}'
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_dup_cl_raw(skip_alert):
    # Same upstream, but read the raw response ourselves: the parsed-headers
    # dict above cannot distinguish "one Content-Length" from "two", so count
    # the occurrences on the wire and check the re-framed chunked body ends
    # with a proper terminal chunk (unambiguous framing, no bytes left over
    # for a smuggled response). The request carries `Connection: close`, so
    # the exchange is bounded by EOF.
    skip_alert(r'upstream sent duplicate Content-Length')

    proc = _run(UPSTREAM_DUP_CL_RAW_PORT, 'dup-cl')
    try:
        _conf_proxy(UPSTREAM_DUP_CL_RAW_PORT)

        sock = client.get(port=8080, no_recv=True)
        sock.settimeout(10)

        data = b''
        closed = False
        try:
            while True:
                part = sock.recv(4096)
                if not part:
                    closed = True
                    break
                data += part
        except socket.timeout:
            closed = False
        finally:
            sock.close()

        assert data[:12] == b'HTTP/1.1 200', f'status line: {data[:40]!r}'

        sep = data.index(b'\r\n\r\n')
        head = data[:sep].lower()
        body = data[sep + 4:]

        # Zero Content-Length headers on the wire — not merely "not two".
        cl_count = head.count(b'content-length')
        assert cl_count == 0, (
            f'client saw {cl_count} Content-Length header(s): {data[:sep]!r}'
        )

        # The re-framed chunked body is complete and unambiguous: the exact
        # upstream bytes, a proper terminal chunk, then EOF — nothing left on
        # the connection to smuggle.
        assert b'transfer-encoding: chunked' in head, f'not re-framed: {head!r}'
        assert body.endswith(b'0\r\n\r\n'), (
            f'terminal chunk missing: {body!r}'
        )
        assert _dechunk(body) == BODY.encode(), (
            f'relayed body mismatch: {body!r}'
        )

        assert closed, 'connection must be closed after the response'
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_dup_cl_keepalive_disabled(skip_alert):
    # Regression for the inconsistent-flag clobber: a duplicate upstream
    # Content-Length must disable downstream keepalive even for a keep-alive
    # client, AND the re-framed body must keep its terminal chunk (a complete
    # body is not falsely truncated).  Before the fix, nxt_h1p_peer_closed
    # reassigned r->inconsistent to 0 on the clean upstream close, so the
    # connection stayed keep-alive; a naive fix that reused that one flag would
    # instead drop the terminal chunk.  Both properties are asserted here.
    skip_alert(r'upstream sent duplicate Content-Length')

    proc = _run(UPSTREAM_DUP_CL_KA_PORT, 'dup-cl')
    try:
        _conf_proxy(UPSTREAM_DUP_CL_KA_PORT)

        # Explicit keep-alive request: the default client sends Connection:
        # close, which would mask whether the server disables keepalive.
        sock = client.get(
            port=8080,
            headers={'Host': 'localhost', 'Connection': 'keep-alive'},
            no_recv=True,
        )
        sock.settimeout(10)

        data = b''
        closed = False
        try:
            while True:
                part = sock.recv(4096)
                if not part:
                    closed = True
                    break
                data += part
                # Once the full response is in, only a short grace period is
                # needed to observe the server-initiated close; shorten the
                # timeout so a *failing* run (keepalive left enabled) does not
                # block for the full 10s.
                if b'0\r\n\r\n' in data:
                    sock.settimeout(1)
        except socket.timeout:
            closed = False
        finally:
            sock.close()

        assert data[:12] == b'HTTP/1.1 200', f'status line: {data[:40]!r}'

        sep = data.index(b'\r\n\r\n')
        head = data[:sep].lower()
        body = data[sep + 4:]

        # Keepalive is disabled despite the client's keep-alive request: the
        # server closes the connection after the response.  No explicit
        # "Connection: close" header is emitted -- nxt_h1p_request_header_send()
        # picks the Connection header from h1p->keepalive before
        # nxt_h1p_request_close() applies "keepalive &= !inconsistent" -- so the
        # socket close is the observable signal here.
        assert closed, 'server must close the connection after a dup-CL response'

        # The complete body keeps its terminal chunk: the fix decouples the
        # keepalive-disable (r->inconsistent) from truncation (r->truncated),
        # so framing stays unambiguous rather than being falsely cut.
        assert head.count(b'content-length') == 0, (
            f'conflicting Content-Length must not reach the client: {data[:sep]!r}'
        )
        assert b'transfer-encoding: chunked' in head, f'not re-framed: {head!r}'
        assert body.endswith(b'0\r\n\r\n'), f'terminal chunk missing: {body!r}'
        assert _dechunk(body) == BODY.encode(), f'relayed body mismatch: {body!r}'
    finally:
        proc.terminate()
        proc.wait()
