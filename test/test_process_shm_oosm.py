"""Router handling of an application that runs out of shared memory.

What this drives: a PHP application whose shared-memory budget
(``applications/<name>/limits/shm``) is one segment, writing a response several
times larger than that segment.  Every write goes through
``nxt_unit_response_write()``, which allocates out of the application's
outgoing segments, and those chunks stay busy until the router has written them
to the client and completed the buffers.  The client stalls before reading, so
the queued response keeps the segment's free map full while the application
still has output left; ``nxt_unit_mmap_get()`` (src/nxt_unit.c) then reports
the out-of-shared-memory condition to the router and blocks until it is
acknowledged.

Why that matters here: the OOSM message is the only thing that reaches
``nxt_router_oosm_handler()`` (src/nxt_router.c), and the ACK the application
waits for is the only thing that reaches ``nxt_process_broadcast_shm_ack()``
(src/nxt_port_memory.c), which the router sends from the ``hdr->oosm`` branch
of ``nxt_port_mmap_buf_completion()`` as it frees the chunks.  All three look
the peer process up by pid, and all three were converted from
``nxt_runtime_process_find()``, which returns a process without a reference, to
``nxt_runtime_process_ref()``.  Nothing else in this suite sends an OOSM
message, so before this test those conversions had no coverage at all.

Which engine runs what: ``nxt_unit_send_oosm()`` always sends to
``lib->router_port``, so in practice ``nxt_router_oosm_handler()`` runs on the
router's *main* engine -- confirmed by the thread id in its log record -- even
though ``.oosm`` is in the worker port handler table too.  The reference is
load-bearing anyway, because the engine that runs the handler is not the only
one manipulating the process refcount: with ``listen_threads`` above one, the
worker engines take and drop references of their own on every shared-memory
message, and one of those drops can be the last.  ``nxt_port_mmap_buf_completion()``
does run on a worker engine, since that is where the response was written out.

What this proves and what it does not: it proves the code ran -- the assertions
are on log records emitted from inside those handlers, not on a response that
would look identical either way.  It does *not* prove the absence of the
use-after-free they were converted to fix; reaching the handler is necessary
for that race, not sufficient, and an uninstrumented build will usually survive
a live one.  Read a pass as "the OOSM/ACK round trip happened and the daemon
stayed correct".  The value of the test is as a driver for the sanitiser legs,
where a stale process pointer becomes a hard failure instead of a coin flip.

Cheap on purpose: one request and one deliberate client stall, about a second
of work once the PHP worker is warm.
"""

import time

import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.log import Log

prerequisites = {'modules': {'php': 'all'}}

client = ApplicationPHP()

APP = 'big_response'

# One segment is PORT_MMAP_DATA_SIZE == 10 MiB (src/nxt_port_memory_int.h), and
# nxt_unit_init() rounds "limits"/"shm" up to whole segments with a floor of
# one, so any value at or below 10 MiB leaves the application a single segment.
SHM_LIMIT = 1

# Comfortably past that 10 MiB even after the kernel socket buffers have
# absorbed their share of the response.  Measured on this tree: 12 MiB produces
# no OOSM at all, because the socket buffers alone hold the overflow; 16 MiB
# produces 1-9 messages depending on timing; 24 MiB produced 12-14 on every
# run.  Sized for the margin, not for the minimum.
RESPONSE_MB = 24
RESPONSE_SIZE = RESPONSE_MB * 1024 * 1024

# The client sends, then does not read for this long, so the router's queued
# response buffers - which are the application's shared memory, busy until the
# write to the client completes - pile up instead of draining.
STALL = 0.3

# > 1 so router worker engines exist and are taking and dropping their own
# references on the same process while the OOSM round trip runs.
LISTEN_THREADS = 4

READ_TIMEOUT = 30


def _body(resp, size):
    """Trailing `size` bytes of a raw response, i.e. the body: the application
    sets Content-Length and the connection is not chunked.  Kept as bytes so a
    24 MiB response is not decoded into a second copy."""

    return resp[-size:]


def test_process_shm_oosm():
    client.load(APP, limits={"shm": SHM_LIMIT}, processes=1)

    assert 'success' in client.conf(
        {"listen_threads": LISTEN_THREADS}, 'settings'
    ), 'listen_threads'

    sock = client.get(url=f'/?mb={RESPONSE_MB}', no_recv=True)

    time.sleep(STALL)

    resp = client.recvall(sock, read_timeout=READ_TIMEOUT, buff_size=65536)
    sock.close()

    body = _body(resp, RESPONSE_SIZE)

    assert len(body) == RESPONSE_SIZE, 'whole response'
    assert body.count(b'x') == RESPONSE_SIZE, 'intact response'

    log = Log.read()

    # The records asserted on below are nxt_debug()/nxt_unit_debug(), compiled
    # out unless Unit was configured with --debug.  Skip loudly rather than
    # let the test degrade into a smoke test that proves nothing about the
    # path it claims to cover.  The sanitiser workflow builds --debug, which
    # is the leg this test exists for.
    if '[debug]' not in log:
        pytest.skip('OOSM evidence needs a --debug build')

    # nxt_router_oosm_handler() logs this on entry, immediately before the
    # reference it now takes; its presence is the proof that the converted
    # call site ran.
    assert 'oosm in ' in log, 'router handled an OOSM message'

    # The application logs "oosm: retry" only after nxt_unit_wait_shm_ack()
    # has returned OK, and that happens only on an NXT_PORT_MSG_SHM_ACK
    # message (src/nxt_unit.c).  The router's only sender of that message is
    # nxt_port_broadcast_shm_ack(), reached through the converted
    # nxt_process_broadcast_shm_ack() -- so this record is proof that the
    # cross-engine post, and the reference it now carries, ran too.
    #
    # Which of the two callers broadcast is deliberately not asserted: with
    # the segment this full, nxt_router_oosm_handler() almost never finds a
    # free chunk to acknowledge with ("oosm: already free" is absent from
    # these runs) and the ACK comes from nxt_port_mmap_buf_completion().
    assert 'oosm: retry' in log, 'application got a SHM_ACK back'

    # Still serving, from the same worker, after the round trip.
    sock = client.get(url='/?mb=1', no_recv=True)
    resp = client.recvall(sock, read_timeout=READ_TIMEOUT, buff_size=65536)
    sock.close()

    assert _body(resp, 1024 * 1024).count(b'x') == 1024 * 1024, (
        'still serving'
    )

    Log.check_alerts()
