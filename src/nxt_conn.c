
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>


#if (NXT_DEBUG)
static nxt_bool_t nxt_conn_is_parked(nxt_event_engine_t *engine,
    nxt_conn_t *c);
#endif
static nxt_bool_t nxt_conn_timer_quiescent(nxt_timer_t *timer);


nxt_conn_io_t  nxt_unix_conn_io = {
    .connect = nxt_conn_io_connect,
    .accept = nxt_conn_io_accept,

    .read = nxt_conn_io_read,
    .recvbuf = nxt_conn_io_recvbuf,
    .recv = nxt_conn_io_recv,

    .write = nxt_conn_io_write,
    .sendbuf = nxt_conn_io_sendbuf,

#if (NXT_HAVE_LINUX_SENDFILE)
    .old_sendbuf = nxt_linux_event_conn_io_sendfile,
#elif (NXT_HAVE_FREEBSD_SENDFILE)
    .old_sendbuf = nxt_freebsd_event_conn_io_sendfile,
#elif (NXT_HAVE_MACOSX_SENDFILE)
    .old_sendbuf = nxt_macosx_event_conn_io_sendfile,
#elif (NXT_HAVE_SOLARIS_SENDFILEV)
    .old_sendbuf = nxt_solaris_event_conn_io_sendfilev,
#elif (NXT_HAVE_AIX_SEND_FILE)
    .old_sendbuf = nxt_aix_event_conn_io_send_file,
#elif (NXT_HAVE_HPUX_SENDFILE)
    .old_sendbuf = nxt_hpux_event_conn_io_sendfile,
#else
    .old_sendbuf = nxt_event_conn_io_sendbuf,
#endif

    .writev = nxt_event_conn_io_writev,
    .send = nxt_event_conn_io_send,
};


nxt_conn_t *
nxt_conn_create(nxt_mp_t *mp, nxt_task_t *task)
{
    nxt_conn_t    *c;
    nxt_thread_t  *thr;

    thr = nxt_thread();

    /*
     * The freelist only recycles connection structs backed by the persistent
     * engine->mem_pool, so a recycled struct outlives every per-connection
     * pool.  When engine->mem_pool is NULL (e.g. an OOM at engine setup) the
     * struct is allocated from the per-connection pool mp and is therefore
     * NOT recyclable; nxt_conn_free() must not push it onto the freelist.
     */

    if (thr->engine->mem_pool != NULL) {

        if (thr->engine->free_connections != NULL) {
            c = thr->engine->free_connections;
            thr->engine->free_connections = c->next;
            nxt_memzero(c, sizeof(nxt_conn_t));

        } else {
            c = nxt_mp_zget(thr->engine->mem_pool, sizeof(nxt_conn_t));
            if (nxt_slow_path(c == NULL)) {
                return NULL;
            }
        }

    } else {
        c = nxt_mp_zget(mp, sizeof(nxt_conn_t));
        if (nxt_slow_path(c == NULL)) {
            return NULL;
        }
    }

    c->mem_pool = mp;

    c->socket.fd = -1;

    c->socket.log = &c->log;
    c->log = *task->log;

    /* The while loop skips possible uint32_t overflow. */

    while (c->log.ident == 0) {
        c->log.ident = nxt_task_next_ident();
    }

    thr->engine->connections++;

    c->task.thread = thr;
    c->task.log = &c->log;
    c->task.ident = c->log.ident;

    /*
     * The socket and timer tasks are captured by value into deferred work items
     * (nxt_conn_read / nxt_conn_write / nxt_conn_connect) and into expiring
     * timer work, so they must not be rebound to a task living in a
     * shorter-lived pool than the connection (e.g. the request-embedded
     * &r->task): an item still queued when that pool is released would
     * dereference freed memory -- freeunit#156.  The ownership is asserted at
     * the sites that used to rebind these fields: nxt_h1p_conn_request_init()
     * and nxt_h1p_request_close() for accepted connections, nxt_conn_socket()
     * and nxt_h1p_peer_close() for outgoing ones.
     */
    c->socket.task = &c->task;
    c->read_timer.task = &c->task;
    c->write_timer.task = &c->task;

    c->io = thr->engine->event.io;
    c->max_chunk = NXT_INT32_T_MAX;
    c->sendfile = NXT_CONN_SENDFILE_UNSET;

    c->socket.read_work_queue = &thr->engine->fast_work_queue;
    c->socket.write_work_queue = &thr->engine->fast_work_queue;

    nxt_conn_timer_init(&c->read_timer, c, c->socket.read_work_queue);
    nxt_conn_timer_init(&c->write_timer, c, c->socket.write_work_queue);

    nxt_log_debug(&c->log, "connections: %uD", thr->engine->connections);

    return c;
}


void
nxt_conn_free(nxt_task_t *task, nxt_conn_t *c)
{
    nxt_mp_t            *mp;
    nxt_event_engine_t  *engine;

    engine = task->thread->engine;
    engine->connections--;

    mp = c->mem_pool;

    /*
     * Only recycle structs backed by the persistent engine->mem_pool.  A
     * freelist entry must outlive every per-connection pool, so when
     * engine->mem_pool is NULL the struct lives in mp (== c->mem_pool) and
     * must die with it -- pushing it would leave a dangling pointer on the
     * freelist once mp is released below.  In the recycled case the struct
     * does not live in mp, so releasing mp does not free the pushed struct.
     *
     * A conn must be freed on the engine that created it: recycling a struct
     * backed by another engine's mem_pool would dangle once that engine is
     * torn down.  The pre-existing connections-- accounting already relies on
     * this; assert it so the freelist can too.
     */

    nxt_assert(c->task.thread == task->thread);

    /*
     * Take both timers out of the engine's timer machinery, and take the conn
     * off whichever tracking queue it is on.  Callers only ever
     * nxt_timer_disable() a timer, which clears the enabled bit but leaves the
     * node in the engine's rbtree, and they rely on nxt_conn_close_handler()
     * having untracked the conn -- both leave a live node inside a struct that
     * nxt_conn_create() re-zeroes on reuse, silently corrupting the rbtree or
     * the idle/active queue and drifting their counters.  nxt_conn_untrack() is
     * idempotent and nxt_timer_delete()'s removal can be deferred as a queued
     * change, so quiescence is rechecked in nxt_conn_recycle_pending().
     *
     * Both run outside the recycling branch below: on the non-recycling path
     * the struct dies with mp, so a node left in the rbtree or on a tracking
     * queue would dangle into released memory rather than be re-zeroed -- the
     * same defect with a worse failure mode.
     */

    nxt_assert(c->idle == NXT_CONN_TRACK_NONE);
    nxt_conn_untrack(engine, c);

    nxt_timer_delete(engine, &c->read_timer);
    nxt_timer_delete(engine, &c->write_timer);

    if (engine->mem_pool != NULL) {
        nxt_assert(!nxt_conn_is_parked(engine, c));

        /*
         * Park the struct rather than publishing it to the freelist directly.
         * A conn can be freed while work items that reference it are still
         * queued -- nxt_h1p_peer_close() sets block_read/block_write for
         * exactly that reason, then reaches nxt_conn_free() synchronously on
         * its fd == -1 path.  Handing the struct out now would let
         * nxt_conn_create()'s re-zero clear those guards from under the queued
         * item, which would then act on an unrelated live connection.
         * nxt_conn_recycle_pending() promotes the struct only once the engine
         * has drained every work queue, so no queued item can still reach it.
         */
        c->next = engine->pending_connections;
        engine->pending_connections = c;
    }

    if (mp != NULL) {
        nxt_mp_release(mp);
    }
}


static nxt_bool_t
nxt_conn_timer_quiescent(nxt_timer_t *timer)
{
    return !nxt_timer_is_in_tree(timer)
           && !timer->queued
           && timer->change == NXT_TIMER_NO_CHANGE;
}


#if (NXT_DEBUG)

/*
 * A conn freed twice would appear on pending_connections twice; the promotion
 * walk then splices it onto free_connections twice and the two lists end up
 * aliasing, so distinct live connections can be handed the same struct.  Catch
 * it at the source in debug builds rather than at the far-away symptom.
 */

static nxt_bool_t
nxt_conn_is_parked(nxt_event_engine_t *engine, nxt_conn_t *c)
{
    nxt_conn_t  *p;

    for (p = engine->pending_connections; p != NULL; p = p->next) {
        if (p == c) {
            return 1;
        }
    }

    return 0;
}

#endif


/*
 * Publish parked connection structs to the reusable freelist.  Called from the
 * event engine once nxt_event_engine_queue_pop() reports every engine-local
 * work queue empty: at that point no queued work item can reference any parked
 * struct, so reuse cannot race a handler that is still to run.  A struct whose
 * timers have not settled yet stays parked and is retried on a later cycle.
 *
 * See nxt_event_engine_start() for why the cross-thread locked_work_queue is
 * outside this barrier and why that is currently safe.
 */

void
nxt_conn_recycle_pending(nxt_event_engine_t *engine)
{
    nxt_conn_t  *c, *next, **link;

    link = &engine->pending_connections;

    for (c = *link; c != NULL; c = next) {
        next = c->next;

        if (nxt_conn_timer_quiescent(&c->read_timer)
            && nxt_conn_timer_quiescent(&c->write_timer))
        {
            *link = next;

            c->next = engine->free_connections;
            engine->free_connections = c;

        } else {
            link = &c->next;
        }
    }
}


void
nxt_conn_timer(nxt_event_engine_t *engine, nxt_conn_t *c,
    const nxt_conn_state_t *state, nxt_timer_t *timer)
{
    nxt_msec_t  value;

    if (state->timer_value != NULL) {
        value = state->timer_value(c, state->timer_data);

        if (value != 0) {
            timer->handler = state->timer_handler;
            nxt_timer_add(engine, timer, value);
        }
    }
}


void
nxt_conn_work_queue_set(nxt_conn_t *c, nxt_work_queue_t *wq)
{
    c->read_work_queue = wq;
    c->write_work_queue = wq;
    c->read_timer.work_queue = wq;
    c->write_timer.work_queue = wq;
}


nxt_sockaddr_t *
nxt_conn_local_addr(nxt_task_t *task, nxt_conn_t *c)
{
    int             ret;
    size_t          size, length;
    socklen_t       socklen;
    nxt_sockaddr_t  *sa;

    if (c->local != NULL) {
        return c->local;
    }

    /* AF_UNIX should not get in here. */

    switch (c->remote->u.sockaddr.sa_family) {
#if (NXT_INET6)
    case AF_INET6:
        socklen = sizeof(struct sockaddr_in6);
        length = NXT_INET6_ADDR_STR_LEN;
        size = offsetof(nxt_sockaddr_t, u) + socklen + length;
        break;
#endif
    case AF_INET:
    default:
        socklen = sizeof(struct sockaddr_in);
        length = NXT_INET_ADDR_STR_LEN;
        size = offsetof(nxt_sockaddr_t, u) + socklen + length;
        break;
    }

    sa = nxt_mp_get(c->mem_pool, size);
    if (nxt_slow_path(sa == NULL)) {
        return NULL;
    }

    sa->socklen = socklen;
    sa->length = length;

    ret = getsockname(c->socket.fd, &sa->u.sockaddr, &socklen);
    if (nxt_slow_path(ret != 0)) {
        nxt_alert(task, "getsockname(%d) failed", c->socket.fd);
        return NULL;
    }

    c->local = sa;

    nxt_sockaddr_text(sa);

    /*
     * TODO: here we can adjust the end of non-freeable block
     * in c->mem_pool to the end of actual sockaddr length.
     */

    return sa;
}
