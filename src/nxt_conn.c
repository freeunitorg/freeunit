
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>


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

    if (engine->mem_pool != NULL) {
        c->next = engine->free_connections;
        engine->free_connections = c;
    }

    if (mp != NULL) {
        nxt_mp_release(mp);
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
