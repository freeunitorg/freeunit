"""Proxy Content-Length *overrun* relay regression test.

An upstream that sends more body bytes than its Content-Length advertises must
not have the excess relayed downstream: FreeUnit already sent that same
Content-Length to the client, so forwarding the surplus would smuggle bytes
into the next response on the connection (response splitting). The hardening in
nxt_h1p_peer_body_process truncates the buf chain to the advertised length,
flags the response inconsistent (disabling keep-alive) and closes the
connection.

Driven by the `overrun-cl` mode of the Rust mock upstream
(test/fake_upstream/): it declares `Content-Length: 100` but writes 150 bytes.
Deliberately language-module-free (gated only on the built-in proxy support) so
it runs on the minimal test build.
"""

import os
import subprocess

import pytest

from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# Deterministic body: byte at global offset i is PATTERN[i % 16] — mirrors the
# fake_upstream PATTERN so the test regenerates the exact advertised bytes.
PATTERN = '0123456789abcdef'

# Must match OVERRUN_DECLARED / OVERRUN_EXCESS in test/fake_upstream/src/main.rs.
DECLARED = 100
EXCESS = 50

# Reserved fake_upstream port for this case (see test/fake_upstream/README.md).
UPSTREAM_OVERRUN_PORT = 7989

FAKE_UPSTREAM_BIN = '/usr/local/bin/fake_upstream'

_skipif_no_fake_upstream = pytest.mark.skipif(
    not os.path.exists(FAKE_UPSTREAM_BIN),
    reason=f'{FAKE_UPSTREAM_BIN} not installed (build via test/fake_upstream)',
)


def _run_overrun_cl(port=UPSTREAM_OVERRUN_PORT):
    proc = subprocess.Popen(
        [FAKE_UPSTREAM_BIN, '--port', str(port), '--mode', 'overrun-cl'],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    waitforsocket(port)
    return proc


@_skipif_no_fake_upstream
def test_proxy_overrun_cl(skip_alert):
    # The router logs a warning about the upstream overrun; don't let it fail
    # the run on the alert check.
    skip_alert(r'upstream sent .* body bytes past Content-Length')

    proc = _run_overrun_cl()
    try:
        assert 'success' in client.conf(
            {
                "listeners": {"*:8080": {"pass": "routes"}},
                "routes": [
                    {
                        "action": {
                            "proxy": f'http://127.0.0.1:{UPSTREAM_OVERRUN_PORT}'
                        }
                    }
                ],
            }
        ), 'overrun-cl proxy configuration'

        resp = client.get(port=8080)

        assert resp['status'] == 200, 'status'
        assert (
            resp['headers']['Content-Length'] == str(DECLARED)
        ), 'relayed Content-Length unchanged'

        # The excess bytes must be dropped: the client sees exactly the
        # advertised length, never DECLARED + EXCESS.
        expected = (PATTERN * (DECLARED // len(PATTERN) + 1))[:DECLARED]
        assert len(resp['body']) == DECLARED, (
            f'relayed body not truncated to Content-Length: '
            f'got {len(resp["body"])} bytes'
        )
        assert resp['body'] == expected, 'relayed body mismatch'
    finally:
        proc.terminate()
        proc.wait()
