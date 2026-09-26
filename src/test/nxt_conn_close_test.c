/*
 * Copyright (C) F5, Inc.
 */

/*
 * An I/O handler queued before nxt_conn_close() in the same engine cycle
 * cannot be taken back, so it must be harmless.  Each leg queues handlers
 * on a real conn over a socketpair, closes, and drains the work queues in
 * engine order.  The fd is never registered, so the close is synchronous.
 */

#include <nxt_main.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/socket.h>


#if (NXT_HAVE_EPOLL_EDGE)

typedef struct {
    nxt_event_engine_t  *engine;
    nxt_event_engine_t  *saved_engine;
    nxt_conn_t          *c;
    nxt_fd_t            peer;

    nxt_uint_t          released;
    nxt_uint_t          read_handlers;
    nxt_uint_t          null_handlers;
    nxt_fd_t            fd_at_release;
} nxt_conn_close_test_t;


static nxt_int_t nxt_conn_close_test_write(nxt_thread_t *thr);
static nxt_int_t nxt_conn_close_test_read(nxt_thread_t *thr);
static nxt_int_t nxt_conn_close_test_twice(nxt_thread_t *thr);
static nxt_int_t nxt_conn_close_test_race(nxt_thread_t *thr);
static nxt_int_t nxt_conn_close_test_setup(nxt_thread_t *thr,
    nxt_conn_close_test_t *t);
static nxt_int_t nxt_conn_close_test_finish(nxt_thread_t *thr,
    nxt_conn_close_test_t *t, nxt_int_t ret);
static void nxt_conn_close_test_drain(nxt_conn_close_test_t *t);
static nxt_bool_t nxt_conn_close_test_drain_wq(nxt_conn_close_test_t *t,
    nxt_work_queue_t *wq);
static void nxt_conn_close_test_release(nxt_task_t *task, void *obj,
    void *data);
static void nxt_conn_close_test_read_handler(nxt_task_t *task, void *obj,
    void *data);


static nxt_conn_close_test_t  nxt_conn_close_test_fixture;


/* Like every close state in the tree: no error_handler. */

static const nxt_conn_state_t  nxt_conn_close_test_close_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_conn_close_test_release,
};


static const nxt_conn_state_t  nxt_conn_close_test_read_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_conn_close_test_read_handler,
    .close_handler = nxt_conn_close_test_read_handler,
    .error_handler = nxt_conn_close_test_read_handler,
};

#endif


nxt_int_t
nxt_conn_close_test(nxt_thread_t *thr)
{
#if (NXT_HAVE_EPOLL_EDGE)

    nxt_int_t  ret;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "conn close test started");

    ret = nxt_conn_close_test_write(thr);

    if (nxt_conn_close_test_read(thr) != NXT_OK) {
        ret = NXT_ERROR;
    }

    if (nxt_conn_close_test_twice(thr) != NXT_OK) {
        ret = NXT_ERROR;
    }

    if (nxt_conn_close_test_race(thr) != NXT_OK) {
        ret = NXT_ERROR;
    }

    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "conn close test passed");

#endif

    return NXT_OK;
}


#if (NXT_HAVE_EPOLL_EDGE)

/* Queues as nxt_h1p_conn_init() and the fd dispatch set them. */

static nxt_int_t
nxt_conn_close_test_setup(nxt_thread_t *thr, nxt_conn_close_test_t *t)
{
    nxt_mp_t            *mp;
    nxt_fd_t            pair[2];
    nxt_conn_t          *c;
    nxt_task_t          *task;
    nxt_event_engine_t  *engine;

    task = thr->task;
    task->thread = thr;

    nxt_memzero(t, sizeof(nxt_conn_close_test_t));
    t->peer = -1;
    t->fd_at_release = -2;

    t->saved_engine = thr->engine;

    engine = nxt_event_engine_create(task, &nxt_epoll_edge_engine, NULL, 0, 0);
    if (nxt_slow_path(engine == NULL)) {
        return NXT_ERROR;
    }

    t->engine = engine;
    thr->engine = engine;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    c = nxt_conn_create(mp, task);
    if (nxt_slow_path(c == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    t->c = c;

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: socketpair failed");
        return NXT_ERROR;
    }

    if (nxt_slow_path(fcntl(pair[0], F_SETFL, O_NONBLOCK) == -1
                      || fcntl(pair[1], F_SETFL, O_NONBLOCK) == -1))
    {
        nxt_fd_close(pair[0]);
        nxt_fd_close(pair[1]);
        return NXT_ERROR;
    }

    c->socket.fd = pair[0];
    t->peer = pair[1];

    c->socket.read_ready = 1;
    c->socket.write_ready = 1;

    c->read_work_queue = &engine->fast_work_queue;
    c->write_work_queue = &engine->fast_work_queue;
    c->socket.read_work_queue = &engine->read_work_queue;
    c->socket.write_work_queue = &engine->write_work_queue;

    c->read = nxt_buf_mem_alloc(mp, 64, 0);
    if (nxt_slow_path(c->read == NULL)) {
        return NXT_ERROR;
    }

    c->read_state = &nxt_conn_close_test_read_state;
    c->write_state = &nxt_conn_close_test_close_state;

    return NXT_OK;
}


static nxt_int_t
nxt_conn_close_test_finish(nxt_thread_t *thr, nxt_conn_close_test_t *t,
    nxt_int_t ret)
{
    nxt_conn_t  *c;

    c = t->c;

    if (c != NULL) {
        if (c->socket.fd != -1) {
            nxt_fd_close(c->socket.fd);
            c->socket.fd = -1;
        }

        nxt_conn_free(thr->task, c);
    }

    if (t->peer != -1) {
        nxt_fd_close(t->peer);
    }

    if (t->engine != NULL) {
        nxt_event_engine_free(t->engine);
    }

    thr->engine = t->saved_engine;

    return ret;
}


/*
 * Drain in nxt_event_engine_start() order, firing close timers between
 * passes.  A NULL handler is counted instead of called.
 */

static void
nxt_conn_close_test_drain(nxt_conn_close_test_t *t)
{
    nxt_bool_t          ran;
    nxt_event_engine_t  *engine;

    engine = t->engine;

    for ( ;; ) {
        do {
            ran = nxt_conn_close_test_drain_wq(t, &engine->fast_work_queue)
                  || nxt_conn_close_test_drain_wq(t, &engine->accept_work_queue)
                  || nxt_conn_close_test_drain_wq(t, &engine->read_work_queue)
                  || nxt_conn_close_test_drain_wq(t, &engine->socket_work_queue)
                  || nxt_conn_close_test_drain_wq(t,
                                                  &engine->connect_work_queue)
                  || nxt_conn_close_test_drain_wq(t, &engine->write_work_queue)
                  || nxt_conn_close_test_drain_wq(t,
                                                  &engine->shutdown_work_queue)
                  || nxt_conn_close_test_drain_wq(t, &engine->close_work_queue);
        } while (ran);

        nxt_timer_find(engine);

        if (nxt_rbtree_is_empty(&engine->timers.tree)) {
            return;
        }

        /*
         * nxt_conn_close_handler() adds the close timer with 0 ms, so a
         * 100 ms step fires it without advancing the clock.
         */
        nxt_timer_expire(engine, engine->timers.now + 100);

        if (engine->fast_work_queue.head == NULL) {
            return;
        }
    }
}


static nxt_bool_t
nxt_conn_close_test_drain_wq(nxt_conn_close_test_t *t, nxt_work_queue_t *wq)
{
    void                *obj, *data;
    nxt_task_t          *task;
    nxt_work_handler_t  handler;

    if (wq->head == NULL) {
        return 0;
    }

    handler = nxt_work_queue_pop(wq, &task, &obj, &data);

    if (handler == NULL) {
        t->null_handlers++;
        return 1;
    }

    handler(task, obj, data);

    return 1;
}


static void
nxt_conn_close_test_release(nxt_task_t *task, void *obj, void *data)
{
    nxt_conn_t             *c;
    nxt_conn_close_test_t  *t;

    c = obj;
    t = &nxt_conn_close_test_fixture;

    t->released++;
    t->fd_at_release = c->socket.fd;
}


/* Like the h1proto read handlers, which end in nxt_h1p_shutdown(). */

static void
nxt_conn_close_test_read_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_conn_t             *c;
    nxt_conn_close_test_t  *t;

    c = obj;
    t = &nxt_conn_close_test_fixture;

    t->read_handlers++;

    nxt_conn_close(task->thread->engine, c);
}


/*
 * A queued write, then block_write and close, as nxt_h1p_peer_close() and
 * the h1p timeouts do: the close state's NULL error_handler must not run.
 */

static nxt_int_t
nxt_conn_close_test_write(nxt_thread_t *thr)
{
    ssize_t                n;
    u_char                 byte;
    nxt_conn_t             *c;
    nxt_conn_close_test_t  *t;

    t = &nxt_conn_close_test_fixture;

    if (nxt_conn_close_test_setup(thr, t) != NXT_OK) {
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    c = t->c;

    c->write = c->read;

    nxt_work_queue_add(c->socket.write_work_queue, nxt_conn_io_write,
                       c->socket.task, c, c->socket.data);

    c->block_write = 1;

    nxt_conn_close(t->engine, c);

    nxt_conn_close_test_drain(t);

    if (t->null_handlers != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: write: a queued nxt_conn_io_write() "
                      "on a closed conn queued the NULL error_handler of the "
                      "close state (%ui times); the engine would call it",
                      t->null_handlers);
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    if (t->released != 1 || t->fd_at_release != -1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: write: release handler ran %ui "
                      "times (expected 1), fd at release %d (expected -1)",
                      t->released, (int) t->fd_at_release);
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    /* Nothing was written; the peer sees only the FIN. */

    n = recv(t->peer, &byte, 1, 0);

    if (n != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: write: peer recv() %z, expected 0",
                      n);
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    return nxt_conn_close_test_finish(thr, t, NXT_OK);
}


/*
 * A queued read, then a close without block_read, as nxt_h1p_conn_close()
 * does: nothing may be read and no read state handler may run.
 */

static nxt_int_t
nxt_conn_close_test_read(nxt_thread_t *thr)
{
    nxt_conn_t             *c;
    nxt_conn_close_test_t  *t;

    t = &nxt_conn_close_test_fixture;

    if (nxt_conn_close_test_setup(thr, t) != NXT_OK) {
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    c = t->c;

    if (send(t->peer, "ping", 4, 0) != 4) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: read: send() failed");
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    nxt_work_queue_add(c->socket.read_work_queue, nxt_conn_io_read,
                       c->socket.task, c, c->socket.data);

    nxt_conn_close(t->engine, c);

    nxt_conn_close_test_drain(t);

    if (t->read_handlers != 0 || c->nbytes != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: read: a queued nxt_conn_io_read() "
                      "on a closed conn read %uD bytes and ran %ui read "
                      "state handlers (expected 0 and 0)",
                      c->nbytes, t->read_handlers);
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    if (t->null_handlers != 0 || t->released != 1 || t->fd_at_release != -1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: read: %ui NULL handlers, release "
                      "handler ran %ui times (expected 1), fd at release %d "
                      "(expected -1)", t->null_handlers, t->released,
                      (int) t->fd_at_release);
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    return nxt_conn_close_test_finish(thr, t, NXT_OK);
}


/* Two closes in one cycle must release once. */

static nxt_int_t
nxt_conn_close_test_twice(nxt_thread_t *thr)
{
    nxt_conn_t             *c;
    nxt_conn_close_test_t  *t;

    t = &nxt_conn_close_test_fixture;

    if (nxt_conn_close_test_setup(thr, t) != NXT_OK) {
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    c = t->c;

    nxt_conn_close(t->engine, c);
    nxt_conn_close(t->engine, c);

    nxt_conn_close_test_drain(t);

    if (t->released != 1 || t->fd_at_release != -1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: twice: release handler ran %ui "
                      "times (expected 1), fd at release %d (expected -1)",
                      t->released, (int) t->fd_at_release);
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    if (c->socket.fd != -1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: twice: fd %d still open after the "
                      "close", (int) c->socket.fd);
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    return nxt_conn_close_test_finish(thr, t, NXT_OK);
}


/*
 * The full race: a read ran and queued its ready handler, a new read event
 * is queued, then the conn is closed.  The ready handler closes again (the
 * second close must be a no-op) and the new read must not reach the FIN
 * and run the close handler, which would close once more.
 */

static nxt_int_t
nxt_conn_close_test_race(nxt_thread_t *thr)
{
    nxt_conn_t             *c;
    nxt_conn_close_test_t  *t;

    t = &nxt_conn_close_test_fixture;

    if (nxt_conn_close_test_setup(thr, t) != NXT_OK) {
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    c = t->c;

    if (send(t->peer, "ping", 4, 0) != 4 || shutdown(t->peer, SHUT_WR) != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: race: send() or shutdown() failed");
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    /* The first read runs and queues the ready handler. */

    nxt_work_queue_add(c->socket.read_work_queue, nxt_conn_io_read,
                       c->socket.task, c, c->socket.data);

    (void) nxt_conn_close_test_drain_wq(t, c->socket.read_work_queue);

    if (c->nbytes != 4 || c->read_work_queue->head == NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: race: the first read got %uD bytes "
                      "(expected 4) or queued no handler", c->nbytes);
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    /* A new edge event: the FIN is readable. */

    c->socket.read_ready = 1;

    nxt_work_queue_add(c->socket.read_work_queue, nxt_conn_io_read,
                       c->socket.task, c, c->socket.data);

    nxt_conn_close(t->engine, c);

    nxt_conn_close_test_drain(t);

    if (t->read_handlers != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: race: %ui read state handlers ran "
                      "(expected 1); the queued read after the close reached "
                      "the socket", t->read_handlers);
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    if (t->null_handlers != 0 || t->released != 1 || t->fd_at_release != -1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close test: race: %ui NULL handlers, release "
                      "handler ran %ui times (expected 1), fd at release %d "
                      "(expected -1)", t->null_handlers, t->released,
                      (int) t->fd_at_release);
        return nxt_conn_close_test_finish(thr, t, NXT_ERROR);
    }

    return nxt_conn_close_test_finish(thr, t, NXT_OK);
}

#endif
