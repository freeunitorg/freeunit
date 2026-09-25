/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for M-6: nxt_listen_event() (nxt_conn_accept.c) used to
 * return a non-NULL lev even when the spare conn could not be allocated.
 * That lev is neither armed (enable_accept) nor linked into
 * engine->listen_connections, so every caller (nxt_router.c,
 * nxt_controller.c, nxt_runtime.c) that only checks for NULL treated it as
 * success, leaving a listener that silently never accepts.  The fix frees
 * lev and returns NULL so the failure is visible.
 */

#include <nxt_main.h>
#include <nxt_event_engine.h>
#include <nxt_conn.h>
#include "nxt_tests.h"


nxt_int_t
nxt_listen_event_test(nxt_thread_t *thr)
{
    nxt_int_t             ret;
    nxt_task_t            *task;
    nxt_listen_event_t    *lev;
    nxt_listen_socket_t   ls;
    nxt_event_engine_t    engine, *saved_engine;

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "listen event test started");

    ret = NXT_ERROR;

    task = thr->task;
    task->thread = thr;

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));
    nxt_memzero(&ls, sizeof(nxt_listen_socket_t));

    nxt_queue_init(&engine.listen_connections);

    /* Dereferenced at the top of nxt_listen_event(), before the alloc. */
    engine.event.io = &nxt_unix_conn_io;

    /*
     * engine->connections < engine->max_connections is false (0 < 0), so
     * nxt_conn_accept_alloc() fails immediately without touching memory
     * pools or the listen socket's sockaddr cache -- the failure this test
     * needs, reached the cheapest way.
     */
    engine.connections = 0;
    engine.max_connections = 0;

    ls.socket = -1;

    saved_engine = thr->engine;
    thr->engine = &engine;

    lev = nxt_listen_event(task, &ls);

    if (nxt_slow_path(lev != NULL)) {
        nxt_log_alert(thr->log, "listen event test: nxt_listen_event() "
                      "returned a non-NULL lev although the spare conn "
                      "could not be allocated");
        goto done;
    }

    if (nxt_slow_path(!nxt_queue_is_empty(&engine.listen_connections))) {
        nxt_log_alert(thr->log, "listen event test: a failed "
                      "nxt_listen_event() still linked something into "
                      "listen_connections");
        goto done;
    }

    ret = NXT_OK;

done:

    thr->engine = saved_engine;

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "listen event test passed");
    }

    return ret;
}
