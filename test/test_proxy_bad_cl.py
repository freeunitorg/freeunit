"""Proxy invalid upstream Content-Length regression test.

When a proxied upstream sends a syntactically invalid Content-Length (a value
that does not parse or overflows nxt_off_t), FreeUnit must not leave
content_length_n at -1 and mis-frame the relayed body. The hardening in
nxt_http_proxy_content_length logs the bad value and marks the response
inconsistent (disabling keep-alive, closing the connection).

Client-observable outcome, confirmed on CI: Unit relays the response (200 +
body) and even forwards the unusable `Content-Length` header verbatim, but the
inconsistent flag forces `Connection: close` -- so the broken framing is bounded
by connection close and cannot desync a kept-alive connection. The invalid
value is also logged at [warn]. This test pins that outcome; assertions carry
the full response so a future behavior change surfaces the actual response.

Driven by the `bad-cl` mode of the Rust mock upstream (test/fake_upstream/): it
sends `Content-Length: notanumber` followed by a short body, then closes.
Language-module-free (gated only on built-in proxy support).
"""

import os
import subprocess

import pytest

from unit.applications.proto import ApplicationProto
from unit.utils import waitforsocket

client = ApplicationProto()

# Must match BAD_CL_BODY / BAD_CL_VALUE in test/fake_upstream/src/main.rs.
BAD_CL_BODY = '0123456789'
BAD_CL_VALUE = 'notanumber'

# Reserved fake_upstream port for this case (see test/fake_upstream/README.md).
UPSTREAM_BAD_CL_PORT = 7988

FAKE_UPSTREAM_BIN = '/usr/local/bin/fake_upstream'

_skipif_no_fake_upstream = pytest.mark.skipif(
    not os.path.exists(FAKE_UPSTREAM_BIN),
    reason=f'{FAKE_UPSTREAM_BIN} not installed (build via test/fake_upstream)',
)


def _run_bad_cl(port=UPSTREAM_BAD_CL_PORT):
    proc = subprocess.Popen(
        [FAKE_UPSTREAM_BIN, '--port', str(port), '--mode', 'bad-cl'],
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


@_skipif_no_fake_upstream
def test_proxy_bad_cl(skip_alert):
    # The router logs a warning about the invalid upstream Content-Length;
    # it is [warn] not [alert], but suppress it defensively.
    skip_alert(r'upstream Content-Length .* is invalid')

    proc = _run_bad_cl()
    try:
        assert 'success' in client.conf(
            {
                "listeners": {"*:8080": {"pass": "routes"}},
                "routes": [
                    {
                        "action": {
                            "proxy": f'http://127.0.0.1:{UPSTREAM_BAD_CL_PORT}'
                        }
                    }
                ],
            }
        ), 'bad-cl proxy configuration'

        resp = client.get(port=8080)

        # The response is relayed with its body intact...
        assert resp['status'] == 200, f'unexpected status: {resp}'
        assert resp['body'] == BAD_CL_BODY, f'body not relayed intact: {resp}'

        # ...but the invalid Content-Length marks the response inconsistent, so
        # the connection is closed rather than kept alive -- bounding the
        # unusable framing to a single connection (no keep-alive desync).
        assert (
            resp['headers'].get('Connection') == 'close'
        ), f'inconsistent response must close the connection: {resp}'
    finally:
        proc.terminate()
        proc.wait()
