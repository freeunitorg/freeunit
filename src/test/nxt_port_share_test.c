/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for an inline send bigger than port->max_share
 * (nxt_port_write_msgs() in src/nxt_port_socket.c).
 *
 * An inline send stops after port->max_share bytes and queues the rest of
 * the message for the write handler.  The socket did not return EAGAIN, so
 * only an armed write event brings the handler back.  The inline exit did
 * not arm it, and the rest of the message never went out.  With the real
 * values, a START_PROCESS message over 64 KiB hung the control API.
 *
 * The test sends a message of six fragments on a port with a share of two.
 * The inline send must arm the write event.  The test then runs the write
 * handler once per armed event, as the event loop does, and counts the
 * datagrams the peer gets.
 *
 * A second message goes out from another engine while the write event of
 * the port still looks active: the write handler on port->engine has found
 * the queue empty and disables the event after the inline send looked at
 * it.  The inline send must still post a re-arm, and the re-arm, which runs
 * on port->engine after that disable, must bring the rest of the message
 * out.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/socket.h>


#define NXT_PORT_SHARE_TEST_FRAG   1024
#define NXT_PORT_SHARE_TEST_FRAGS  6


static void nxt_port_share_test_enable(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_port_share_test_disable(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_port_share_test_completion(nxt_task_t *task, void *obj,
    void *data);
static void nxt_port_share_test_signal(nxt_event_engine_t *engine,
    nxt_uint_t signo);
static void nxt_port_share_test_drain(nxt_work_queue_t *wq);
static nxt_int_t nxt_port_share_test_send(nxt_task_t *task, nxt_mp_t *mp,
    nxt_port_t *port);
static void nxt_port_share_test_recv(nxt_fd_t sock);


static nxt_uint_t  nxt_port_share_test_enables;
static nxt_uint_t  nxt_port_share_test_completions;
static nxt_uint_t  nxt_port_share_test_frags;
static nxt_uint_t  nxt_port_share_test_last;


/* The poller stand-in: an enable records that the event is armed. */

static void
nxt_port_share_test_enable(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_port_share_test_enables++;

    ev->write = NXT_EVENT_ACTIVE;
}


static void
nxt_port_share_test_disable(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    ev->write = NXT_EVENT_DISABLED;
}


static void
nxt_port_share_test_completion(nxt_task_t *task, void *obj, void *data)
{
    nxt_port_share_test_completions++;
}


/* A post to the owner engine needs no wake-up: the test runs it itself. */

static void
nxt_port_share_test_signal(nxt_event_engine_t *engine, nxt_uint_t signo)
{
}


static void
nxt_port_share_test_drain(nxt_work_queue_t *wq)
{
    void                *obj, *data;
    nxt_task_t          *t;
    nxt_work_handler_t  handler;

    while (wq->head != NULL) {
        handler = nxt_work_queue_pop(wq, &t, &obj, &data);
        handler(t, obj, data);
    }
}


/* Count the datagrams the peer has, and the ones that end a message. */

static void
nxt_port_share_test_recv(nxt_fd_t sock)
{
    ssize_t         n;
    nxt_port_msg_t  pm;
    u_char          data[NXT_PORT_SHARE_TEST_FRAG];
    struct iovec    iov[2];
    struct msghdr   mh;

    for ( ;; ) {
        iov[0].iov_base = &pm;
        iov[0].iov_len = sizeof(nxt_port_msg_t);
        iov[1].iov_base = data;
        iov[1].iov_len = sizeof(data);

        nxt_memzero(&mh, sizeof(struct msghdr));
        mh.msg_iov = iov;
        mh.msg_iovlen = 2;

        n = recvmsg(sock, &mh, MSG_DONTWAIT);

        if (n < (ssize_t) sizeof(nxt_port_msg_t)) {
            return;
        }

        nxt_port_share_test_frags++;
        nxt_port_share_test_last += !pm.mf;
    }
}


/*
 * Five full fragments and a short sixth one.  The header takes room from
 * each fragment, so five fragments carry less than five times
 * NXT_PORT_SHARE_TEST_FRAG bytes.
 */

static nxt_int_t
nxt_port_share_test_send(nxt_task_t *task, nxt_mp_t *mp, nxt_port_t *port)
{
    nxt_buf_t  *buf;

    buf = nxt_buf_mem_alloc(mp, 5 * NXT_PORT_SHARE_TEST_FRAG + 100, 0);
    if (nxt_slow_path(buf == NULL)) {
        return NXT_ERROR;
    }

    buf->mem.free = buf->mem.end;
    buf->completion_handler = nxt_port_share_test_completion;

    if (nxt_port_socket_write2(task, port, NXT_PORT_MSG_DATA_LAST, -1, -1,
                               7, 0, buf)
        != NXT_OK)
    {
        nxt_log_error(NXT_LOG_NOTICE, task->log,
                      "port share test: the inline send failed");
        return NXT_ERROR;
    }

    return NXT_OK;
}


nxt_int_t
nxt_port_share_test(nxt_thread_t *thr)
{
    nxt_mp_t               *mp;
    nxt_fd_t               pair[2];
    nxt_int_t              ret;
    nxt_uint_t             round;
    nxt_task_t             *task;
    nxt_port_t             *port;
    nxt_event_engine_t     engine, owner, *saved_engine;
    nxt_event_interface_t  stub;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port share test started");

    task = thr->task;
    task->thread = thr;

    ret = NXT_ERROR;

    nxt_port_share_test_enables = 0;
    nxt_port_share_test_completions = 0;
    nxt_port_share_test_frags = 0;
    nxt_port_share_test_last = 0;

    nxt_memzero(&engine, sizeof(engine));
    nxt_memzero(&stub, sizeof(stub));

    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    stub.enable_write = nxt_port_share_test_enable;
    stub.disable_write = nxt_port_share_test_disable;
    engine.event = stub;

    saved_engine = thr->engine;
    thr->engine = &engine;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        thr->engine = saved_engine;
        return NXT_ERROR;
    }

    port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_MAIN);
    if (nxt_slow_path(port == NULL)) {
        nxt_mp_destroy(mp);
        thr->engine = saved_engine;
        return NXT_ERROR;
    }

    /* SOCK_DGRAM: the type src/nxt_socketpair.c selects. */

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0
                      || fcntl(pair[1], F_SETFL, O_NONBLOCK) == -1))
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port share test: socketpair failed");
        port->pair[0] = -1;
        port->pair[1] = -1;
        port->socket.fd = -1;
        goto done;
    }

    port->pair[0] = pair[0];
    port->pair[1] = pair[1];

    /* A fresh port: the write event was never added to the poller. */

    nxt_port_write_enable(task, port);

    port->socket.task = task;
    port->socket.write = NXT_EVENT_INACTIVE;
    port->max_size = NXT_PORT_SHARE_TEST_FRAG;
    port->max_share = 2 * NXT_PORT_SHARE_TEST_FRAG;

    if (nxt_port_share_test_send(task, mp, port) != NXT_OK) {
        goto done;
    }

    nxt_port_share_test_drain(&engine.fast_work_queue);
    nxt_port_share_test_recv(pair[0]);

    if (nxt_port_share_test_frags != 2 || nxt_port_share_test_enables != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port share test: the inline send sent %ui fragments "
                      "and armed the write event %ui times (expected 2, 1)",
                      nxt_port_share_test_frags,
                      nxt_port_share_test_enables);
        goto done;
    }

    /* The event loop: one write handler run per armed event. */

    for (round = 0;
         round < 8 && port->socket.write == NXT_EVENT_ACTIVE;
         round++)
    {
        port->socket.write_handler(task, &port->socket, NULL);
        nxt_port_share_test_drain(&engine.fast_work_queue);
        nxt_port_share_test_recv(pair[0]);
    }

    if (nxt_port_share_test_frags != NXT_PORT_SHARE_TEST_FRAGS
        || nxt_port_share_test_last != 1
        || nxt_port_share_test_completions != 1
        || !nxt_queue_is_empty(&port->messages))
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port share test: the peer got %ui of %d fragments, "
                      "%ui last, %ui completions, queue %s",
                      nxt_port_share_test_frags, NXT_PORT_SHARE_TEST_FRAGS,
                      nxt_port_share_test_last,
                      nxt_port_share_test_completions,
                      nxt_queue_is_empty(&port->messages) ? "empty"
                                                          : "not empty");
        goto done;
    }

    /*
     * The second message, from this engine to a port that another engine
     * owns.  The event still looks active, and the write handler on the
     * owner disables it after the inline send has looked at it.
     */

    nxt_port_share_test_enables = 0;
    nxt_port_share_test_completions = 0;
    nxt_port_share_test_frags = 0;
    nxt_port_share_test_last = 0;

    nxt_memzero(&owner, sizeof(owner));

    nxt_work_queue_cache_create(&owner.work_queue_cache, 1024);
    owner.fast_work_queue.cache = &owner.work_queue_cache;
    nxt_work_queue_name(&owner.fast_work_queue, "owner fast");

    stub.signal = nxt_port_share_test_signal;
    owner.event = stub;
    owner.task = *task;

    port->engine = &owner;
    port->socket.write = NXT_EVENT_ACTIVE;

    if (nxt_port_share_test_send(task, mp, port) != NXT_OK) {
        goto owner_done;
    }

    nxt_port_share_test_drain(&engine.fast_work_queue);
    nxt_port_share_test_recv(pair[0]);

    /* The write handler on the owner disables the event now. */

    port->socket.write = NXT_EVENT_DISABLED;

    /* The owner runs what was posted to it, then its event loop. */

    thr->engine = &owner;

    nxt_locked_work_queue_move(thr, &owner.locked_work_queue,
                               &owner.fast_work_queue);
    nxt_port_share_test_drain(&owner.fast_work_queue);

    for (round = 0;
         round < 8 && port->socket.write == NXT_EVENT_ACTIVE;
         round++)
    {
        port->socket.write_handler(task, &port->socket, NULL);
        nxt_port_share_test_drain(&owner.fast_work_queue);
        nxt_port_share_test_recv(pair[0]);
    }

    thr->engine = &engine;

    if (nxt_port_share_test_frags != NXT_PORT_SHARE_TEST_FRAGS
        || nxt_port_share_test_last != 1
        || nxt_port_share_test_completions != 1
        || !nxt_queue_is_empty(&port->messages))
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port share test: with the event active at the inline "
                      "send, the peer got %ui of %d fragments, %ui last, %ui "
                      "completions, %ui re-arms, queue %s",
                      nxt_port_share_test_frags, NXT_PORT_SHARE_TEST_FRAGS,
                      nxt_port_share_test_last,
                      nxt_port_share_test_completions,
                      nxt_port_share_test_enables,
                      nxt_queue_is_empty(&port->messages) ? "empty"
                                                          : "not empty");
        goto owner_done;
    }

    ret = NXT_OK;

owner_done:

    thr->engine = &engine;
    port->engine = &engine;

    nxt_work_queue_cache_destroy(&owner.work_queue_cache);

done:

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_port_test_run_error_handler(task, port);
        nxt_port_share_test_drain(&engine.fast_work_queue);
    }

    if (port->pair[0] != -1) {
        nxt_fd_close(port->pair[0]);
        nxt_fd_close(port->pair[1]);
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    nxt_port_use(task, port, -1);
    nxt_mp_destroy(mp);

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    thr->engine = saved_engine;

    if (ret == NXT_OK) {
        nxt_thread_time_update(thr);
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port share test passed");
    }

    return ret;
}
