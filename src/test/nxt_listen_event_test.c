/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for M-6: nxt_listen_event() (nxt_conn_accept.c) used to
 * return a non-NULL lev even when the spare conn could not be allocated,
 * and that lev was neither armed (enable_accept) nor linked into
 * engine->listen_connections.  Every caller (nxt_router.c, nxt_controller.c,
 * nxt_runtime.c) only checks for NULL, so the listener silently never
 * accepted on that engine, and the router could not even find it again for
 * a later update or close.
 *
 * The fix keeps the listener: lev is linked, left unarmed, and handed to the
 * same recovery the accept path already uses when a spare cannot be
 * allocated -- nxt_conn_accept_close_idle(), which closes idle conns and
 * arms the 100ms listen timer.  nxt_conn_listen_timer_handler() then
 * retries the spare and, once it succeeds, arms accept.
 *
 * Both halves are driven here on a fixture engine:
 *
 *   1. with engine->connections == engine->max_connections the spare
 *      allocation fails; nxt_listen_event() must still return a lev that is
 *      linked, unarmed, without a spare, with the retry timer pending;
 *
 *   2. a slot is freed (max_connections raised), the listen timer handler
 *      installed on lev->timer is run; it must allocate the spare, arm
 *      accept once and invoke the accept handler with that spare.
 */

#include <nxt_main.h>
#include <nxt_event_engine.h>
#include <nxt_conn.h>
#include "nxt_tests.h"


static nxt_uint_t  nxt_listen_event_test_enable_accept_calls;
static nxt_uint_t  nxt_listen_event_test_accept_calls;
static nxt_conn_t  *nxt_listen_event_test_accepted_spare;


static void
nxt_listen_event_test_enable_accept(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev)
{
    nxt_listen_event_test_enable_accept_calls++;

    /* What a real enable_accept does: the read side becomes active. */
    ev->read = NXT_EVENT_ACTIVE;
}


static void
nxt_listen_event_test_disable_read(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev)
{
    /*
     * Must not be reached: the listener is never armed before the retry
     * succeeds, and the M-5 guard in nxt_conn_accept_close_idle() skips an
     * inactive listener.  Counted through the enable_accept counter going
     * negative would be obscure, so use a separate alert instead.
     */
    nxt_thread_log_alert("listen event test: disable_read() on a listener "
                         "that was never armed");

    ev->read = NXT_EVENT_INACTIVE;
}


static void
nxt_listen_event_test_accept(nxt_task_t *task, void *obj, void *data)
{
    /*
     * Stands in for nxt_conn_io_accept(): the timer handler calls
     * lev->accept with the spare it just allocated.  Record it instead of
     * running accept(2) on the fixture's closed listen fd.
     */
    nxt_listen_event_test_accept_calls++;
    nxt_listen_event_test_accepted_spare = data;
}


static nxt_conn_io_t  nxt_listen_event_test_io = {
    .accept = nxt_listen_event_test_accept,
};


nxt_int_t
nxt_listen_event_test(nxt_thread_t *thr)
{
    nxt_int_t               ret;
    nxt_task_t              *task;
    nxt_sockaddr_t          sa;
    nxt_listen_event_t      *lev;
    nxt_listen_socket_t     ls;
    nxt_event_engine_t      engine, *saved_engine;
    nxt_work_queue_cache_t  cache;

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "listen event test started");

    ret = NXT_ERROR;
    lev = NULL;

    task = thr->task;
    task->thread = thr;

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));
    nxt_memzero(&ls, sizeof(nxt_listen_socket_t));
    nxt_memzero(&sa, sizeof(nxt_sockaddr_t));

    if (nxt_slow_path(nxt_timers_init(&engine.timers, 4) != NXT_OK)) {
        nxt_log_alert(thr->log, "listen event test failed to init timers");
        return NXT_ERROR;
    }

    /* Backs the conn structs and the sockaddr cache of the retry below. */
    engine.mem_pool = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(engine.mem_pool == NULL)) {
        nxt_free(engine.timers.changes);
        return NXT_ERROR;
    }

    engine.task = *task;
    engine.event.io = &nxt_listen_event_test_io;
    engine.event.enable_accept = nxt_listen_event_test_enable_accept;
    engine.event.disable_read = nxt_listen_event_test_disable_read;

    nxt_queue_init(&engine.listen_connections);
    nxt_queue_init(&engine.idle_connections);
    nxt_queue_init(&engine.active_connections);

    nxt_work_queue_cache_create(&cache, 1024);

    engine.fast_work_queue.cache = &cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    engine.close_work_queue.cache = &cache;
    nxt_work_queue_name(&engine.close_work_queue, "close");

    /*
     * Enough of a listen socket for nxt_conn_accept_alloc(): the spare's
     * sockaddr is sized and typed from it.  The fd is never used, since
     * accept is stubbed above.
     */
    sa.type = SOCK_STREAM;
    sa.u.sockaddr.sa_family = AF_INET;

    ls.socket = -1;
    ls.sockaddr = &sa;
    ls.socklen = sizeof(struct sockaddr_in);
    ls.address_length = NXT_INET_ADDR_STR_LEN;

    /*
     * engine->connections < engine->max_connections is false (0 < 0), so
     * nxt_conn_accept_alloc() fails immediately without touching memory
     * pools or the listen socket's sockaddr cache -- the failure this test
     * needs, reached the cheapest way.
     */
    engine.connections = 0;
    engine.max_connections = 0;

    saved_engine = thr->engine;
    thr->engine = &engine;

    nxt_listen_event_test_enable_accept_calls = 0;
    nxt_listen_event_test_accept_calls = 0;
    nxt_listen_event_test_accepted_spare = NULL;

    lev = nxt_listen_event(task, &ls);

    if (nxt_slow_path(lev == NULL)) {
        nxt_log_alert(thr->log, "listen event test: nxt_listen_event() "
                      "returned NULL although only the spare conn could "
                      "not be allocated");
        goto done;
    }

    if (nxt_slow_path(nxt_queue_is_empty(&engine.listen_connections)
                      || nxt_queue_first(&engine.listen_connections)
                         != &lev->link))
    {
        nxt_log_alert(thr->log, "listen event test: the listener was not "
                      "linked into listen_connections");
        goto done;
    }

    if (nxt_slow_path(lev->next != NULL)) {
        nxt_log_alert(thr->log, "listen event test: a spare conn is set "
                      "although its allocation must have failed");
        goto done;
    }

    if (nxt_slow_path(nxt_listen_event_test_enable_accept_calls != 0
                      || nxt_fd_event_is_active(lev->socket.read)))
    {
        nxt_log_alert(thr->log, "listen event test: accept was armed "
                      "without a spare conn");
        goto done;
    }

    if (nxt_slow_path(!nxt_timer_is_in_tree(&lev->timer)
                      && lev->timer.change == NXT_TIMER_NO_CHANGE))
    {
        nxt_log_alert(thr->log, "listen event test: the listen timer was "
                      "not armed, the spare would never be retried");
        goto done;
    }

    if (nxt_slow_path(lev->timer.handler == NULL)) {
        nxt_log_alert(thr->log, "listen event test: no listen timer "
                      "handler installed");
        goto done;
    }

    /*
     * nxt_conn_accept_close_idle() also queued the idle-close pass.  There
     * is nothing idle on the fixture, so it is a no-op here; run it anyway
     * so the queue is empty when the cache is destroyed below.
     */
    while (engine.close_work_queue.head != NULL) {
        void                *obj, *data;
        nxt_task_t          *t;
        nxt_work_handler_t  handler;

        handler = nxt_work_queue_pop(&engine.close_work_queue, &t, &obj,
                                     &data);
        handler(t, obj, data);
    }

    /*
     * A connection slot frees up.  Run what the 100ms timer would run:
     * nxt_conn_listen_timer_handler(), installed on lev->timer by
     * nxt_listen_event().  It must now get the spare, arm accept and hand
     * the spare to the accept handler.
     */
    engine.max_connections = 1;

    lev->timer.handler(task, &lev->timer, NULL);

    if (nxt_slow_path(lev->next == NULL)) {
        nxt_log_alert(thr->log, "listen event test: the retry did not "
                      "allocate the spare conn");
        goto done;
    }

    if (nxt_slow_path(nxt_listen_event_test_enable_accept_calls != 1
                      || !nxt_fd_event_is_active(lev->socket.read)))
    {
        nxt_log_alert(thr->log, "listen event test: the retry did not arm "
                      "accept exactly once (%ui calls)",
                      nxt_listen_event_test_enable_accept_calls);
        goto done;
    }

    if (nxt_slow_path(nxt_listen_event_test_accept_calls != 1
                      || nxt_listen_event_test_accepted_spare != lev->next))
    {
        nxt_log_alert(thr->log, "listen event test: the retry did not "
                      "call the accept handler with the new spare "
                      "(%ui calls)", nxt_listen_event_test_accept_calls);
        goto done;
    }

    ret = NXT_OK;

done:

    if (lev != NULL) {
        /* What nxt_router_listen_event_release() does on the last ref. */

        if (lev->next != NULL) {
            nxt_sockaddr_cache_free(&engine, lev->next);
            nxt_conn_free(task, lev->next);
        }

        nxt_timer_delete(&engine, &lev->timer);
        nxt_queue_remove(&lev->link);
        nxt_free(lev);
    }

    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&cache);
    nxt_mp_destroy(engine.mem_pool);
    nxt_free(engine.timers.changes);

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "listen event test passed");
    }

    return ret;
}
