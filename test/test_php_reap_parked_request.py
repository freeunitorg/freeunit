"""A request parked while the only worker is reaped must still be answered.

fastcgi_finish_request() makes the PHP module report the request done while
the script keeps running.  The router takes that as the response
(NXT_APR_GOT_RESPONSE), so nxt_router_app_port_release() decrements
active_requests, moves the port to app->idle_ports and stamps idle_start --
all while the worker is still executing.

A request that arrives in that window finds "processes": {"max": 1} fully
used, so nxt_router_app_port_get() parks it in app->ack_waiting_req and
writes its headers to the shared queue.  At idle_start + idle_timeout the
reaper in nxt_router_adjust_idle_timer() deregisters that worker and sends it
QUIT.  Nothing then starts a replacement: the reaper clears port->app before
the process exits, and nxt_port_close() calls
nxt_router_app_port_close() -- which re-evaluates nxt_router_app_need_start()
-- only for a port that still has an application (src/nxt_port.c:173).  The
parked request waited for unrelated traffic to start a process; measured at
over 360 s with no 503.

The fix runs the same can_start/need_start check in the reaper, so a reap that
leaves work queued starts a replacement and the parked request is answered at
about idle_timeout.

The test makes the two intervals unambiguous: the first request's script
sleeps for SLEEP_SECONDS, which is much longer than IDLE_TIMEOUT, so an answer
to the second request within DEADLINE can only have come from a replacement
process, not from the original worker finishing its job.
"""

import time

from unit.applications.lang.php import ApplicationPHP

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()

# Seconds.  IDLE_TIMEOUT is short to keep the test quick; SLEEP_SECONDS is far
# enough past it that "the first worker finished and picked the request up"
# cannot be mistaken for "a replacement was started"; DEADLINE sits between
# them with room for a process start.
IDLE_TIMEOUT = 2
SLEEP_SECONDS = 20
DEADLINE = 10


def test_php_reap_parked_request():
    client.load(
        'detached_worker',
        processes={'max': 1, 'spare': 0, 'idle_timeout': IDLE_TIMEOUT},
    )

    # Starts the worker and returns as soon as fastcgi_finish_request() runs;
    # the script then holds the worker for SLEEP_SECONDS.
    assert (
        client.get(url=f'/?sleep={SLEEP_SECONDS}')['status'] == 200
    ), 'first request answered'

    time.sleep(0.15)

    start = time.time()
    resp = client.get(read_timeout=DEADLINE)
    elapsed = time.time() - start

    assert resp.get('status') == 200, 'parked request answered'
    assert elapsed < DEADLINE, 'parked request answered before the deadline'
    assert elapsed < SLEEP_SECONDS, 'answered by a replacement process'
