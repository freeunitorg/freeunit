"""Proxy chunked-response relay edge cases: chunk-extensions and trailers.

The upstream sends a chunked response whose chunk-size lines carry a
chunk-extension (`<size>;ext=val`), or which appends a trailer field after the
terminal `0` chunk. FreeUnit must relay the body transparently and intact in
both cases -- extensions are not part of the payload, and a trailer must not
corrupt or truncate the relayed body.

Driven by the `chunked-ext` / `chunked-trailer` modes of the Rust mock upstream
(test/fake_upstream/), each sending the same 40-byte deterministic body split
into two chunks. Language-module-free (gated only on built-in proxy support).
"""

import os
import subprocess

import pytest

from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# Deterministic body: byte at global offset i is PATTERN[i % 16] — mirrors the
# fake_upstream PATTERN. Must match CHUNKED_EDGE_LEN in main.rs.
PATTERN = '0123456789abcdef'
EDGE_LEN = 40
EXPECTED_BODY = (PATTERN * (EDGE_LEN // len(PATTERN) + 1))[:EDGE_LEN]

# Reserved fake_upstream ports (see test/fake_upstream/README.md).
UPSTREAM_CHUNKED_TRAILER_PORT = 7986
UPSTREAM_CHUNKED_EXT_PORT = 7985

FAKE_UPSTREAM_BIN = '/usr/local/bin/fake_upstream'

_skipif_no_fake_upstream = pytest.mark.skipif(
    not os.path.exists(FAKE_UPSTREAM_BIN),
    reason=f'{FAKE_UPSTREAM_BIN} not installed (build via test/fake_upstream)',
)


def _run(port, mode):
    proc = subprocess.Popen(
        [FAKE_UPSTREAM_BIN, '--port', str(port), '--mode', mode],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        waitforsocket(port)
    except Exception:
        proc.terminate()
        proc.wait()
        raise
    return proc


def _dechunk(body):
    """Minimal chunked decoder: strips chunk-extensions and stops at the
    terminal 0-chunk (ignoring any trailer), so it is robust to both edge
    cases regardless of how the harness would parse them."""
    out = []
    i = 0
    while i < len(body):
        j = body.find('\r\n', i)
        if j < 0:
            break
        size = int(body[i:j].split(';', 1)[0], 16)
        i = j + 2
        if size == 0:
            break
        out.append(body[i : i + size])
        i += size + 2  # chunk data + trailing CRLF
    return ''.join(out)


def _relayed_body(port):
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"proxy": f'http://127.0.0.1:{port}'}}],
        }
    ), 'chunked-edge proxy configuration'

    raw = client.get(port=8080, raw_resp=True)
    head, _, body = raw.partition('\r\n\r\n')

    assert head.split('\r\n', 1)[0].startswith('HTTP/1.1 200'), f'status: {raw!r}'

    if 'transfer-encoding: chunked' in head.lower():
        body = _dechunk(body)

    return body, raw


@_skipif_no_fake_upstream
def test_proxy_chunked_ext():
    proc = _run(UPSTREAM_CHUNKED_EXT_PORT, 'chunked-ext')
    try:
        body, raw = _relayed_body(UPSTREAM_CHUNKED_EXT_PORT)
        assert body == EXPECTED_BODY, f'chunk-extension body mismatch: {raw!r}'
    finally:
        proc.terminate()
        proc.wait()


@_skipif_no_fake_upstream
def test_proxy_chunked_trailer():
    proc = _run(UPSTREAM_CHUNKED_TRAILER_PORT, 'chunked-trailer')
    try:
        body, raw = _relayed_body(UPSTREAM_CHUNKED_TRAILER_PORT)
        assert body == EXPECTED_BODY, f'trailer body mismatch: {raw!r}'
    finally:
        proc.terminate()
        proc.wait()
