/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for M-11: nxt_runtime_close_idle_connections()
 * (src/nxt_runtime.c) must close every idle conn through the conn's own
 * protocol -- c->read_state->close_handler, the contract
 * nxt_conn_accept_close_idle_handler() already uses under fd pressure --
 * and must leave a conn that has no protocol state yet alone.
 *
 * Before this fix, the idle-close pass called nxt_conn_close() directly:
 *
 *   - a keep-alive h1p conn still carries nxt_h1p_request_send_state,
 *     whose ready_handler (nxt_h1p_conn_sent) is a no-op with c->write ==
 *     NULL -- the conn was closed but never freed, a leak (CONN-INV-2);
 *
 *   - a conn that never received a request has write_state == NULL, and
 *     nxt_conn_close_handler() dereferenced it, i.e. crashed;
 *
 *   - a generic release (just nxt_conn_free()) is not enough either: the
 *     protocol free handlers also return c->remote to the engine's sockaddr
 *     cache (nxt_conn_accept_alloc() took it from there) and, for a router
 *     conn, drop the listener reference taken in nxt_conn_accept(), without
 *     which a draining listener never reaches its final release.
 *
 * The real protocol states (nxt_h1p_keepalive_state,
 * nxt_controller_conn_read_state) are private to their modules and need a
 * live listener configuration, so the protocol-owned conn below carries a
 * stand-in read_state whose close_handler does what nxt_h1p_conn_close()
 * ends up doing -- install a close state and nxt_conn_close() -- and whose
 * close state's ready_handler does exactly what nxt_h1p_conn_free() does:
 * nxt_sockaddr_cache_free(), nxt_conn_free() and the real
 * nxt_router_listen_event_release().  What the test asserts is the
 * dispatch contract of the idle-close pass: the protocol close handler is
 * what runs, to completion, and the not-yet-initialized conn is untouched.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_port.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include <nxt_conn.h>
#include "nxt_tests.h"

#include <unistd.h>


static nxt_uint_t  nxt_runtime_idle_close_test_stale_calls;
static nxt_uint_t  nxt_runtime_idle_close_test_close_calls;
static nxt_uint_t  nxt_runtime_idle_close_test_free_calls;
static nxt_conn_t  *nxt_runtime_idle_close_test_closed_conn;


static void
nxt_runtime_idle_close_test_stale_handler(nxt_task_t *task, void *obj,
    void *data)
{
    /*
     * Stands in for nxt_h1p_conn_sent(): the leftover request-send
     * ready_handler of a keep-alive conn, which does nothing to the conn
     * itself.  If this runs, the idle-close pass bypassed the protocol and
     * closed the conn with its stale write_state.
     */
    nxt_runtime_idle_close_test_stale_calls++;
}


static const nxt_conn_state_t  nxt_runtime_idle_close_test_stale_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_runtime_idle_close_test_stale_handler,
};


static void
nxt_runtime_idle_close_test_proto_free(nxt_task_t *task, void *obj,
    void *data)
{
    nxt_conn_t          *c;
    nxt_listen_event_t  *lev;
    nxt_event_engine_t  *engine;

    c = obj;

    /* The body of nxt_h1p_conn_free(). */

    nxt_runtime_idle_close_test_free_calls++;

    engine = task->thread->engine;

    nxt_sockaddr_cache_free(engine, c);

    lev = c->listen;

    nxt_conn_free(task, c);

    nxt_router_listen_event_release(&engine->task, lev, NULL);
}


static const nxt_conn_state_t  nxt_runtime_idle_close_test_proto_close_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_runtime_idle_close_test_proto_free,
};


static void
nxt_runtime_idle_close_test_proto_close(nxt_task_t *task, void *obj,
    void *data)
{
    nxt_conn_t  *c;

    c = obj;

    /* What nxt_h1p_conn_close() ends up doing (nxt_h1p_conn_closing()). */

    nxt_runtime_idle_close_test_close_calls++;
    nxt_runtime_idle_close_test_closed_conn = c;

    c->write_state = &nxt_runtime_idle_close_test_proto_close_state;

    nxt_conn_close(task->thread->engine, c);
}


static const nxt_conn_state_t  nxt_runtime_idle_close_test_proto_read_state
    nxt_aligned(64) =
{
    .close_handler = nxt_runtime_idle_close_test_proto_close,
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
    int                     fds[2];
    nxt_mp_t                *mp;
    nxt_int_t               ret;
    nxt_conn_t              *proto, *fresh;
    nxt_task_t              *task;
    nxt_sockaddr_t          sa, *remote;
    nxt_listen_event_t      lev;
    nxt_listen_socket_t     ls;
    nxt_event_engine_t      engine, *saved_engine;
    nxt_work_queue_cache_t  cache;

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "runtime idle close test started");

    ret = NXT_ERROR;
    fds[0] = -1;
    fds[1] = -1;
    proto = NULL;
    fresh = NULL;

    task = thr->task;
    task->thread = thr;

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));
    nxt_memzero(&lev, sizeof(nxt_listen_event_t));
    nxt_memzero(&ls, sizeof(nxt_listen_socket_t));
    nxt_memzero(&sa, sizeof(nxt_sockaddr_t));

    /* Backs the conn structs and the sockaddr cache. */
    engine.mem_pool = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(engine.mem_pool == NULL)) {
        return NXT_ERROR;
    }

    engine.task = *task;
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

    /* Enough of a listen socket to size and type cached sockaddrs from. */
    sa.type = SOCK_STREAM;
    sa.u.sockaddr.sa_family = AF_INET;

    ls.socket = -1;
    ls.sockaddr = &sa;
    ls.socklen = sizeof(struct sockaddr_in);
    ls.address_length = NXT_INET_ADDR_STR_LEN;

    /*
     * The listener the conns were accepted on: its own reference plus the
     * one nxt_conn_accept() takes for the protocol-owned conn below.  Not
     * draining and without a joint, so nxt_router_listen_event_release()
     * only drops the count.
     */
    lev.listen = &ls;
    lev.count = 2;

    saved_engine = thr->engine;
    thr->engine = &engine;

    /*
     * Real, valid fds so nxt_socket_close() in the close handler closes
     * something real rather than an already-invalid descriptor.
     */
    if (nxt_slow_path(pipe(fds) != 0)) {
        nxt_log_alert(thr->log, "runtime idle close test failed to open a "
                      "pipe");
        goto done;
    }

    /*
     * A keep-alive conn owned by its protocol: as nxt_conn_accept_alloc()
     * and nxt_conn_accept() leave it, with a cached sockaddr, a listener
     * reference, and a stale write_state from its last response.
     */
    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        goto done;
    }

    proto = nxt_conn_create(mp, task);
    if (nxt_slow_path(proto == NULL)) {
        nxt_mp_destroy(mp);
        goto done;
    }

    /* From here on the per-conn pool belongs to the conn. */

    proto->socket.fd = fds[0];
    fds[0] = -1;

    proto->remote = nxt_sockaddr_cache_alloc(&engine, &ls);
    if (nxt_slow_path(proto->remote == NULL)) {
        goto done;
    }

    remote = proto->remote;

    proto->listen = &lev;

    /* Bypasses the shutdown() syscall branch in nxt_conn_close(). */
    proto->socket.error = 1;

    proto->read_state = &nxt_runtime_idle_close_test_proto_read_state;
    proto->write_state = &nxt_runtime_idle_close_test_stale_state;

    /* Idle, and eligible for the close pass (read_ready == 0). */
    proto->socket.read_ready = 0;
    nxt_conn_idle(&engine, proto);

    /*
     * A conn accepted a moment ago whose listen handler has not run yet:
     * no protocol state at all.  Before the fix the pass closed it with
     * write_state == NULL, a crash in nxt_conn_close_handler(); the pass
     * must leave it alone now.
     */
    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        goto done;
    }

    fresh = nxt_conn_create(mp, task);
    if (nxt_slow_path(fresh == NULL)) {
        nxt_mp_destroy(mp);
        goto done;
    }

    fresh->socket.fd = fds[1];
    fds[1] = -1;

    fresh->remote = nxt_sockaddr_cache_alloc(&engine, &ls);
    if (nxt_slow_path(fresh->remote == NULL)) {
        goto done;
    }

    fresh->listen = &lev;
    fresh->socket.error = 1;
    fresh->socket.read_ready = 0;
    nxt_conn_idle(&engine, fresh);

    nxt_runtime_idle_close_test_stale_calls = 0;
    nxt_runtime_idle_close_test_close_calls = 0;
    nxt_runtime_idle_close_test_free_calls = 0;
    nxt_runtime_idle_close_test_closed_conn = NULL;

    nxt_runtime_test_close_idle_connections(&engine);

    /* Runs nxt_conn_close_handler(), which dispatches to fast_work_queue. */
    nxt_runtime_idle_close_test_drain(&engine.close_work_queue);
    nxt_runtime_idle_close_test_drain(&engine.fast_work_queue);

    if (nxt_slow_path(nxt_runtime_idle_close_test_close_calls != 1
                      || nxt_runtime_idle_close_test_closed_conn != proto))
    {
        nxt_log_alert(thr->log, "runtime idle close test: the protocol "
                      "close handler did not run exactly once for the "
                      "protocol-owned conn (%ui calls)",
                      nxt_runtime_idle_close_test_close_calls);
        goto done;
    }

    if (nxt_slow_path(nxt_runtime_idle_close_test_stale_calls != 0)) {
        nxt_log_alert(thr->log, "runtime idle close test: the stale "
                      "write_state ran -- the idle-close pass bypassed the "
                      "protocol close handler");
        goto done;
    }

    if (nxt_slow_path(nxt_runtime_idle_close_test_free_calls != 1)) {
        nxt_log_alert(thr->log, "runtime idle close test: the protocol "
                      "free handler did not run (%ui calls)",
                      nxt_runtime_idle_close_test_free_calls);
        goto done;
    }

    /* The protocol free path ran: proto is gone, only fresh is left. */
    proto = NULL;

    if (nxt_slow_path(lev.count != 1)) {
        nxt_log_alert(thr->log, "runtime idle close test: the listener "
                      "reference was not released (count %uD, expected 1)",
                      lev.count);
        goto done;
    }

    /*
     * The sockaddr went back to the engine cache: the cache hands out its
     * free list first, so the next allocation for this listener must be
     * that very block (it lives in the engine pool, destroyed below).
     */
    if (nxt_slow_path(nxt_sockaddr_cache_alloc(&engine, &ls) != remote)) {
        nxt_log_alert(thr->log, "runtime idle close test: the closed "
                      "conn's sockaddr was not returned to the engine "
                      "cache");
        goto done;
    }

    if (nxt_slow_path(engine.connections != 1)) {
        nxt_log_alert(thr->log, "runtime idle close test: engine accounts "
                      "for %uD connection(s), expected 1 -- either the "
                      "protocol-owned conn was not freed or the "
                      "uninitialized one was", engine.connections);
        goto done;
    }

    if (nxt_slow_path(nxt_queue_is_empty(&engine.idle_connections)
                      || nxt_queue_first(&engine.idle_connections)
                         != &fresh->link
                      || nxt_queue_next(&fresh->link)
                         != nxt_queue_tail(&engine.idle_connections)
                      || fresh->idle != NXT_CONN_TRACK_IDLE))
    {
        nxt_log_alert(thr->log, "runtime idle close test: idle_connections "
                      "must hold exactly the not-yet-initialized conn");
        goto done;
    }

    if (nxt_slow_path(fresh->write_state != NULL
                      || fresh->socket.fd == -1))
    {
        nxt_log_alert(thr->log, "runtime idle close test: the "
                      "not-yet-initialized conn was touched by the pass");
        goto done;
    }

    ret = NXT_OK;

done:

    /* What the listen handler and the protocol would eventually do. */

    if (fresh != NULL) {
        if (fresh->socket.fd != -1) {
            (void) close(fresh->socket.fd);
        }

        nxt_conn_untrack(&engine, fresh);

        if (fresh->remote != NULL) {
            nxt_sockaddr_cache_free(&engine, fresh);
        }

        nxt_conn_free(task, fresh);
    }

    if (proto != NULL) {
        if (proto->socket.fd != -1) {
            (void) close(proto->socket.fd);
        }

        nxt_conn_untrack(&engine, proto);

        if (proto->remote != NULL) {
            nxt_sockaddr_cache_free(&engine, proto);
        }

        nxt_conn_free(task, proto);
    }

    if (fds[0] != -1) {
        (void) close(fds[0]);
    }

    if (fds[1] != -1) {
        (void) close(fds[1]);
    }

    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&cache);
    nxt_mp_destroy(engine.mem_pool);

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "runtime idle close test passed");
    }

    return ret;
}
