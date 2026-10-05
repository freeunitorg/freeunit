"""
Graceful vs fast shutdown: signal-handler split.

Background
----------
SIGTERM and SIGQUIT used to share an identical handler in
src/nxt_main_process.c -- both routed straight to nxt_runtime_quit()
without distinguishing fast from graceful exit.  The handlers are now
split: SIGQUIT sets rt->quit_mode = NXT_PORT_QUIT_GRACEFUL, and the
NXT_PORT_MSG_QUIT message dispatched by nxt_runtime_stop_app_processes()
carries that byte.  libunit already parses this wire format and
dispatches to nxt_unit_quit(), which lets in-flight requests drain when
the byte is NXT_PORT_QUIT_GRACEFUL.

The plumbing is what these tests exercise behaviourally:

  * SIGQUIT -> in-flight request must complete with status 200.
  * SIGTERM -> in-flight request is dropped (connection reset or truncated).

A third placeholder test asserts the wire-format intent but is skipped
because verifying the actual quit_param byte would require C-level
instrumentation -- the behavioural pair above already covers reachability.
"""

import os
import signal
import socket
import time

import pytest

from unit.applications.lang.python import ApplicationPython
from unit.log import Log

prerequisites = {'modules': {'python': 'all'}}

client = ApplicationPython()


@pytest.fixture(autouse=True)
def _require_restart_flag(request):
    """
    Every test in this module sends SIGTERM/SIGQUIT to the unitd master.
    The autouse `run` fixture in conftest.py only rmtrees the temp dir
    when --restart is set; otherwise it tries to PUT /config on the now
    dead daemon during teardown and crashes with KeyError: 'body'.

    Skip with an actionable message when the flag is missing instead of
    pretending to fail for the wrong reason.
    """
    if not request.config.getoption('--restart'):
        pytest.skip(
            'test_graceful_reload.py signals the unitd master process; '
            'rerun with --restart so conftest.py rmtrees the temp dir '
            'instead of trying /config PUT on a dead daemon'
        )


# Long enough that a non-graceful SIGTERM cannot accidentally let the
# request finish; short enough to keep the test suite snappy.  Bumped
# above the previous 3 s so fast machines have less chance of racing
# the response to completion before the signal lands.
INFLIGHT_DELAY = 5

# Cap for the curl-equivalent recv loop.  Must exceed INFLIGHT_DELAY plus
# graceful-drain overhead so a working SIGQUIT path has room to complete.
RESPONSE_TIMEOUT = 30


def _start_inflight_request(
    delay: int, body: bytes = b'', parts: int = 1
) -> socket.socket:
    """
    Open a TCP connection to Unit, send a request that takes *delay*
    seconds inside the handler, and return the socket with the request
    fully written.  Caller is responsible for receiving and closing.

    With an empty *body* the delayed app sleeps once before finishing
    the response.  With a non-empty *body* it echoes the body back in
    *parts* chunks, sleeping *delay* seconds after each chunk -- the
    response is demonstrably mid-stream while the handler sleeps, so a
    truncated echo is positive proof the worker died mid-response.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(RESPONSE_TIMEOUT)
    sock.connect(('127.0.0.1', 8080))

    req = (
        f'POST / HTTP/1.1\r\n'
        f'Host: localhost\r\n'
        f'X-Delay: {delay}\r\n'
        f'X-Parts: {parts}\r\n'
        f'Content-Length: {len(body)}\r\n'
        f'Connection: close\r\n'
        f'\r\n'
    ).encode() + body
    sock.sendall(req)

    return sock


def _recv_all(sock: socket.socket) -> bytes:
    """
    Drain the socket until EOF or timeout.  Returns whatever bytes
    arrived; an empty/truncated reply is a signal that the peer reset
    the connection mid-response, which is exactly what SIGTERM should do.
    """
    chunks = []
    deadline = time.monotonic() + RESPONSE_TIMEOUT
    while time.monotonic() < deadline:
        try:
            data = sock.recv(4096)
        except (ConnectionResetError, socket.timeout):
            break
        if not data:
            break
        chunks.append(data)
    return b''.join(chunks)


def _wait_app_ready(module: str, tries: int = 100) -> None:
    """
    Block until the app actually serves a request before the test fires
    its in-flight request.  The delayed app spawns its worker on demand,
    and under the sanitized (slow) build the worker may still be starting
    when the request lands -- the router then answers 503 before any
    signal is involved, a readiness race unrelated to shutdown.  Warm up
    with delay=0 requests until one returns 200 (or give up after ~10 s).
    """
    head = b''
    for _ in range(tries):
        # The listener may not be accepting yet on the first tries; a
        # refused/reset connection is just another not-ready signal, so
        # swallow OSError and retry rather than crashing the warm-up.
        try:
            sock = _start_inflight_request(0)
        except OSError:
            time.sleep(0.1)
            continue

        try:
            head = _recv_all(sock).split(b'\r\n', 1)[0]
        except OSError:
            # Reset/abort mid-read is just another not-ready signal.
            head = b''
        finally:
            sock.close()

        if b'200' in head:
            return

        time.sleep(0.1)

    raise AssertionError(
        f'{module} app never became ready to serve (last status: {head!r})'
    )


def _wait_for_socket_closed(sock: socket.socket,
                            timeout: float = RESPONSE_TIMEOUT) -> bool:
    """Poll the socket until the peer closes it or *timeout* elapses."""
    sock.settimeout(timeout)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            data = sock.recv(4096)
        except (ConnectionResetError, socket.timeout):
            return True
        if not data:
            return True
    return False


@pytest.mark.parametrize('module', ['wsgi', 'asgi'])
def test_sigquit_completes_inflight_request(unit_pid, skip_alert, module):
    """
    SIGQUIT to the unitd master must let an in-flight request complete.

    Previously the SIGQUIT path was identical to SIGTERM (both called
    nxt_runtime_quit() with no quit_param plumbing), so libunit defaulted
    to NXT_QUIT_NORMAL and the worker exited immediately.  Now the
    main signal handler stores rt->quit_mode = NXT_QUIT_GRACEFUL and the
    QUIT message body now carries that byte; libunit's nxt_unit_quit()
    drains the active request before tearing the context down.

    Both application flavours matter and exercise different points:

      * wsgi: the worker is busy in time.sleep() and only reads the QUIT
        message after the response -- guards against the worker being
        torn down while it is not polling libunit.  The full response is
        produced before QUIT is seen, so it completes end-to-end.
      * asgi: `await sleep()` yields to the asyncio loop, so libunit
        processes the QUIT byte *while the response is mid-stream* --
        this is the variant that exercises the GRACEFUL worker drain.
        The request echoes a body in two chunks with a sleep in between.
        The response *start* (200 + header-complete) is contractual; the
        full-body echo is only delivered once the router also drains its
        in-flight connections (roadmap P5), so a short body is xfail'd --
        but ONLY after confirming the worker actually drained.  A short
        body accompanied by the "active request on ctx quit" marker means
        SIGQUIT regressed to the NORMAL fast exit, which is still a hard
        failure (see below).
    """
    client.load('delayed', module=module)

    # Spawn and warm the worker before the in-flight request so a slow
    # (sanitized) startup cannot 503 the request before SIGQUIT is even
    # sent -- that readiness race is not what this test measures.
    _wait_app_ready(module)

    skip_alert(r'process \d+ exited on signal')
    skip_alert(r'sendmsg.+failed')
    skip_alert(r'last message send failed')

    if module == 'asgi':
        payload = b'0123456789abcdef' * 8
        # Two chunks with a 3 s pause: the signal lands inside the first
        # pause, while half the echo is still unsent.
        sock = _start_inflight_request(3, body=payload, parts=2)
    else:
        payload = b''
        sock = _start_inflight_request(INFLIGHT_DELAY)

    # Give the handler a moment to enter its sleep before the signal.
    time.sleep(0.5)

    os.kill(unit_pid, signal.SIGQUIT)

    body = _recv_all(sock)
    sock.close()

    status_line = body.split(b'\r\n', 1)[0]
    assert b'200' in status_line, (
        f'SIGQUIT must let in-flight request complete with 200, got '
        f'status line: {status_line!r} (full body: {body!r})'
    )

    # An incomplete header section would mean the worker was killed
    # mid-write -- a regression we are guarding against.
    assert b'\r\n\r\n' in body, (
        f'graceful response must be header-complete, got: {body!r}'
    )

    if module == 'asgi':
        received = body.split(b'\r\n\r\n', 1)[1]

        # Whatever body bytes did arrive must be an uncorrupted prefix of
        # the echo: an in-flight response may be cut short, but it must
        # never be garbled or reordered.
        assert payload.startswith(received), (
            f'in-flight response body must be an uncorrupted prefix of the '
            f'{len(payload)}-byte echo; got {received!r}'
        )

        if received != payload:
            # A short body has two possible causes and only one is
            # acceptable, so disambiguate before deciding -- otherwise a real
            # regression would silently xfail-pass.  Use the same positive log
            # evidence as test_sigterm_drops_inflight_request: libunit emits
            # "active request on ctx quit" only when nxt_unit_quit() walks a
            # non-empty active_req on the NORMAL branch.  That branch is
            # unreachable on GRACEFUL (it returns early while a request is in
            # flight), so the marker is proof the fast-exit path ran.
            #
            #   * marker present -> SIGQUIT regressed to the NORMAL fast exit
            #     and tore the in-flight request down: the #107 regression
            #     this test MUST catch (hard fail).
            #   * marker absent  -> the worker drained (GRACEFUL branch), but
            #     the full body was not delivered end-to-end because the
            #     router tears its own connections down on its QUIT.  Closing
            #     that needs server-initiated connection draining (roadmap P5,
            #     see roadmap/plan-graceful-shutdown.md), not yet implemented
            #     -- an accepted shortfall, so xfail.  By the time _recv_all()
            #     has returned the quit is long processed and a present marker
            #     is already written, so a short wait (wait counts 0.1 s ticks
            #     -> ~2 s) catches a regression without hanging the xfail path.
            regressed = Log.wait_for_record(
                r'active request on ctx quit', wait=20
            ) is not None

            assert not regressed, (
                'SIGQUIT took the NORMAL fast-exit path: libunit logged '
                '"active request on ctx quit", tearing down the in-flight '
                'request instead of draining it -- quit-mode plumbing '
                'regression (src/nxt_main_process.c handler split, '
                'src/nxt_runtime.c nxt_runtime_quit_buf).'
            )

            pytest.xfail(
                'worker-level drain worked (no NORMAL-teardown marker) but '
                'the full in-flight body was not delivered end-to-end: needs '
                'router connection draining (roadmap P5), not yet implemented'
            )


def test_sigterm_drops_inflight_request(unit_pid, skip_alert):
    """
    SIGTERM remains the fast-exit path: in-flight requests are dropped.

    On SIGTERM, rt->quit_mode = NXT_PORT_QUIT_NORMAL and the QUIT
    message body carries 0; libunit's nxt_unit_quit() returns immediately
    and calls close_handler() on every active request.

    Why ASGI here?  In synchronous WSGI a worker that is busy in
    time.sleep() never pumps libunit's message loop, so the QUIT byte
    sits in the queue until the request has finished anyway.  The
    behavioural difference between QUIT_NORMAL and QUIT_GRACEFUL is only
    observable when libunit can actually process the QUIT message while
    a request is still in-flight -- which is what the asyncio-driven
    ASGI handler enables.  See test/python/delayed/asgi.py: the
    `await sleep(delay)` yields control back to the asyncio loop,
    letting libunit's add_reader callback run the QUIT path.

    Regression evidence is the "active request on ctx quit" warning --
    libunit emits it from the for-loop that walks
    active_req inside nxt_unit_quit() when quit_param == NXT_QUIT_NORMAL.
    The for-loop is unreachable on the GRACEFUL path (which returns
    early when the active_req queue is non-empty), so the warning's
    presence is positive proof that the NORMAL fast-exit branch ran.
    Absence of the warning would mean SIGTERM accidentally took the
    GRACEFUL branch -- the regression this test must catch.
    """
    client.load('delayed', module='asgi')

    # See test_sigquit_completes_inflight_request: warm the worker first so
    # the request reaches a live app (a 503 from a still-starting worker
    # would produce no "active request on ctx quit" marker and fail below).
    _wait_app_ready('asgi')

    skip_alert(r'process \d+ exited on signal')
    skip_alert(r'sendmsg.+failed')
    skip_alert(r'last message send failed')
    skip_alert(r'active request on ctx quit')

    sock = _start_inflight_request(INFLIGHT_DELAY)

    time.sleep(0.5)

    os.kill(unit_pid, signal.SIGTERM)

    body = _recv_all(sock)
    sock.close()

    # Positive log evidence beats body-shape inference: it is robust to
    # the timing race where a fast machine completes the response before
    # the signal lands.  wait_for_record polls the unit log up to ~15 s
    # for the marker; on a real regression to the GRACEFUL branch the
    # marker never appears and the assertion fails.
    assert Log.wait_for_record(r'active request on ctx quit') is not None, (
        'SIGTERM did not take the NORMAL fast-exit path: libunit drained '
        'in-flight requests as if quit_param == NXT_PORT_QUIT_GRACEFUL. '
        'Regression in the quit-mode plumbing -- see src/nxt_main_process.c '
        'sigterm/sigquit handler split and src/nxt_runtime.c '
        'nxt_runtime_quit_buf().'
    )


@pytest.mark.skip(
    reason='needs C-level instrumentation; covered by test 1+2 behaviorally'
)
def test_quit_message_carries_quit_param():
    """
    Direct assertion that NXT_PORT_MSG_QUIT carries a one-byte body
    encoding rt->quit_mode.  Verifying this from Python would require
    intercepting the AF_UNIX port socket between unitd master and the
    application worker, which is not straightforward without a debug
    build that exposes the wire bytes.

    Tests 1 and 2 above exercise the same plumbing behaviourally:
      * graceful-drain on SIGQUIT  => byte == NXT_QUIT_GRACEFUL
      * fast-exit on SIGTERM       => byte == NXT_QUIT_NORMAL
    """
