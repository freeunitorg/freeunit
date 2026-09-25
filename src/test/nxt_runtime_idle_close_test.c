/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for M-11: nxt_runtime_close_idle_connections()
 * (src/nxt_runtime.c) must install a write_state that releases the conn
 * before calling nxt_conn_close(), per CONN-INV-2 ("before nxt_conn_close(),
 * c->write_state points to a state whose ready_handler releases the conn").
 *
 * nxt_conn_close_handler() (src/nxt_conn_close.c) always ends by invoking
 * c->write_state->ready_handler once the socket is actually closed.  Before
 * this fix, the idle-connection close pass left write_state exactly as it
 * was when the conn went idle:
 *
 *   - a keep-alive h1p conn still carries nxt_h1p_request_send_state,
 *     whose ready_handler (nxt_h1p_conn_sent) is a no-op with c->write ==
 *     NULL -- the conn is closed but never freed, a leak;
 *
 *   - a conn that never received a request has write_state == NULL, and
 *     the close handler dereferences it, i.e. crashes.
 *
 * The crash case is not exercised here: reproducing it intentionally
 * segfaults the process, which is not a usable pass/fail signal for a test
 * binary that runs other cases afterward.  It was checked by hand instead
 * (see the fix commit body).  This test instead uses a stand-in "stale"
 * write_state -- playing the role of nxt_h1p_request_send_state above --
 * whose ready_handler is harmless but distinguishable, and asserts that the
 * idle-close pass overwrites it with a release state before closing.  Before
 * the fix this fails outright: the stale handler runs (or, with write_state
 * left NULL, the process crashes) and the conn is never freed.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_event_engine.h>
#include <nxt_conn.h>
#include "nxt_tests.h"

#include <unistd.h>


static nxt_uint_t  nxt_runtime_idle_close_test_stale_calls;


static void
nxt_runtime_idle_close_test_stale_handler(nxt_task_t *task, void *obj,
    void *data)
{
    /*
     * Stands in for nxt_h1p_conn_sent(): a leftover request-send
     * ready_handler that does nothing to the conn itself.  If this runs,
     * the idle-close pass left the stale write_state in place instead of
     * installing a release state, and the conn below is leaked -- caught
     * by the engine.connections check at the end, not here, since a
     * no-op handler gives nothing else to assert on directly.
     */
    nxt_runtime_idle_close_test_stale_calls++;
}


static const nxt_conn_state_t  nxt_runtime_idle_close_test_stale_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_runtime_idle_close_test_stale_handler,
};


/*
 * A minimal event interface: only .close is exercised by the close path
 * this test drives (c->socket.error is set to 1 below, so nxt_conn_close()
 * takes the close_work_queue branch directly and never touches shutdown,
 * enable/disable or poll).  Returning 0 tells nxt_conn_close_handler() that
 * nothing is pending, so it closes the fd and dispatches write_state's
 * ready_handler on the same pass -- no timer round trip to wait out.
 */
static nxt_bool_t
nxt_runtime_idle_close_test_event_close(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev)
{
    return 0;
}


static void
nxt_runtime_idle_close_test_drain(nxt_work_queue_t *wq)
{
    void                *obj, *data;
    nxt_task_t          *task;
    nxt_work_handler_t  handler;

    while (wq->head != NULL) {
        handler = nxt_work_queue_pop(wq, &task, &obj, &data);
        handler(task, obj, data);
    }
}


nxt_int_t
nxt_runtime_idle_close_test(nxt_thread_t *thr)
{
    int                  fds[2];
    nxt_mp_t             *mp;
    nxt_int_t             ret;
    nxt_conn_t           *c;
    nxt_task_t           *task;
    nxt_event_engine_t   engine, *saved_engine;
    nxt_work_queue_cache_t  cache;
    nxt_bool_t            conn_created;

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "runtime idle close test started");

    ret = NXT_ERROR;
    fds[0] = -1;
    fds[1] = -1;
    conn_created = 0;

    task = thr->task;
    task->thread = thr;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));

    engine.task = *task;
    engine.mem_pool = NULL;     /* the conn's struct lives in mp, not recycled */
    engine.event.close = nxt_runtime_idle_close_test_event_close;

    nxt_queue_init(&engine.idle_connections);
    nxt_queue_init(&engine.active_connections);
    nxt_queue_init(&engine.listen_connections);
    nxt_queue_init(&engine.joints);

    nxt_work_queue_cache_create(&cache, 1024);

    engine.fast_work_queue.cache = &cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    engine.close_work_queue.cache = &cache;
    nxt_work_queue_name(&engine.close_work_queue, "close");

    engine.shutdown_work_queue.cache = &cache;
    nxt_work_queue_name(&engine.shutdown_work_queue, "shutdown");

    saved_engine = thr->engine;
    thr->engine = &engine;

    /*
     * A real, valid fd so nxt_socket_close() in the close handler closes
     * something real rather than an already-invalid descriptor.
     */
    if (nxt_slow_path(pipe(fds) != 0)) {
        nxt_log_alert(thr->log, "runtime idle close test failed to open a "
                      "pipe");
        goto done;
    }

    c = nxt_conn_create(mp, task);
    if (nxt_slow_path(c == NULL)) {
        goto done;
    }

    /*
     * From here on, mp belongs to the conn: nxt_conn_free() (reached below
     * through the release handler this fix installs) calls
     * nxt_mp_release(mp), which destroys mp itself once its refcount hits
     * zero -- the "done:" cleanup must not destroy it a second time.
     */
    conn_created = 1;

    c->socket.fd = fds[0];
    fds[0] = -1;

    /* Bypasses the shutdown() syscall branch in nxt_conn_close(). */
    c->socket.error = 1;

    /* Idle, and eligible for the close pass (read_ready == 0). */
    c->socket.read_ready = 0;
    nxt_conn_idle(&engine, c);

    /*
     * The stand-in for a stale write_state left over from whatever this
     * conn was doing before it went idle.
     */
    c->write_state = &nxt_runtime_idle_close_test_stale_state;

    nxt_runtime_idle_close_test_stale_calls = 0;

    nxt_runtime_test_close_idle_connections(&engine);

    /* Runs nxt_conn_close_handler(), which dispatches to fast_work_queue. */
    nxt_runtime_idle_close_test_drain(&engine.close_work_queue);
    nxt_runtime_idle_close_test_drain(&engine.fast_work_queue);

    if (nxt_slow_path(nxt_runtime_idle_close_test_stale_calls != 0)) {
        nxt_log_alert(thr->log, "runtime idle close test: the stale "
                      "write_state ran -- the idle-close pass did not "
                      "install a release state before closing");
        goto done;
    }

    if (nxt_slow_path(engine.connections != 0)) {
        nxt_log_alert(thr->log, "runtime idle close test: engine still "
                      "accounts for %uD connection(s) after the release "
                      "handler ran -- nxt_conn_free() was not reached",
                      engine.connections);
        goto done;
    }

    if (nxt_slow_path(!nxt_queue_is_empty(&engine.idle_connections))) {
        nxt_log_alert(thr->log, "runtime idle close test: the conn was not "
                      "removed from idle_connections");
        goto done;
    }

    ret = NXT_OK;

done:

    if (fds[0] != -1) {
        (void) close(fds[0]);
    }

    if (fds[1] != -1) {
        (void) close(fds[1]);
    }

    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&cache);

    if (!conn_created) {
        nxt_mp_destroy(mp);
    }

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "runtime idle close test passed");
    }

    return ret;
}
