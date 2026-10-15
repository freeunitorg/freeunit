/*
 * Copyright (C) F5, Inc.
 */

/*
 * Outstanding READ_QUEUE doorbells on the shared application port stay
 * bounded (freeunitorg/freeunit#708, Codex "Cap outstanding
 * application-queue doorbells").
 *
 * A worker kept busy dequeues the next item before it reads the doorbell
 * of the previous one, so the queue depth seen by the router stays at zero
 * and every enqueue asks for a doorbell.  Without a gate the doorbells
 * outrun the socket buffer and pile up in port->messages without bound.
 *
 * The leg replays the router's send sequence from
 * nxt_router_app_prepare_request() against a real non-blocking datagram
 * socketpair whose reader never reads: enqueue, decide, skip the doorbell
 * when nxt_port_socket_backlogged(), otherwise write it; then the "worker"
 * dequeues the item.  The heap queue must hold at most the one doorbell
 * that met EAGAIN, and the sequence must still have asked for a doorbell
 * on every item (the condition the gate exists for).
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_queue.h>
#include <nxt_app_queue.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/socket.h>


#define NXT_PORT_DOORBELL_TEST_ITEMS  100000


static void
nxt_port_doorbell_test_stub(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
}


static nxt_uint_t
nxt_port_doorbell_test_queued(nxt_port_t *port)
{
    nxt_uint_t        n;
    nxt_queue_link_t  *lnk;

    n = 0;

    for (lnk = nxt_queue_first(&port->messages);
         lnk != nxt_queue_tail(&port->messages);
         lnk = nxt_queue_next(lnk))
    {
        n++;
    }

    return n;
}


static void
nxt_port_doorbell_test_drain_wq(nxt_work_queue_t *wq)
{
    void                *obj, *data;
    nxt_task_t          *t;
    nxt_work_handler_t  handler;

    while (wq->head != NULL) {
        handler = nxt_work_queue_pop(wq, &t, &obj, &data);
        handler(t, obj, data);
    }
}


nxt_int_t
nxt_port_doorbell_test(nxt_thread_t *thr)
{
    int                    notify;
    uint8_t                msg[NXT_APP_QUEUE_MSG_SIZE];
    uint8_t                buf[NXT_APP_QUEUE_MSG_SIZE];
    uint32_t               cookie;
    nxt_fd_t               pair[2];
    nxt_int_t              ret;
    nxt_uint_t             i, asked, written, queued;
    nxt_task_t             *task;
    nxt_port_t             *port;
    nxt_app_queue_t        *q;
    nxt_event_engine_t     engine, *saved_engine;
    nxt_event_interface_t  stub;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port doorbell test started");

    task = thr->task;
    task->thread = thr;

    ret = NXT_ERROR;
    pair[0] = -1;
    pair[1] = -1;
    asked = 0;
    written = 0;
    queued = 0;

    q = nxt_zalloc(sizeof(nxt_app_queue_t));
    if (nxt_slow_path(q == NULL)) {
        return NXT_ERROR;
    }

    nxt_app_queue_init(q);
    nxt_memzero(msg, sizeof(msg));

    nxt_memzero(&engine, sizeof(engine));
    nxt_memzero(&stub, sizeof(stub));

    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    stub.enable_write = nxt_port_doorbell_test_stub;
    stub.block_write = nxt_port_doorbell_test_stub;
    engine.event = stub;

    saved_engine = thr->engine;
    thr->engine = &engine;

    port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_APP);
    if (nxt_slow_path(port == NULL)) {
        thr->engine = saved_engine;
        nxt_free(q);
        return NXT_ERROR;
    }

    port->socket.task = task;
    port->socket.log = task->log;
    port->engine = &engine;
    port->max_size = 1024;
    port->max_share = 1024;
    port->socket.write_work_queue = &engine.fast_work_queue;

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0)) {
        goto done;
    }

    if (nxt_slow_path(fcntl(pair[1], F_SETFL, O_NONBLOCK) == -1)) {
        goto done;
    }

    port->pair[0] = pair[0];
    port->pair[1] = pair[1];

    nxt_port_write_enable(task, port);

    port->socket.write_ready = 1;
    port->socket.write = NXT_EVENT_INACTIVE;

    for (i = 0; i < NXT_PORT_DOORBELL_TEST_ITEMS; i++) {

        if (nxt_app_queue_send_bound(q, msg, sizeof(msg), i, &notify,
                                     &cookie, 4)
            != NXT_OK)
        {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "port doorbell test: enqueue %ui failed", i);
            goto done;
        }

        if (notify != 0) {
            asked++;

            /* The gate in nxt_router_app_prepare_request(). */
            if (!nxt_port_socket_backlogged(port)) {
                if (nxt_port_socket_write(task, port,
                                          NXT_PORT_MSG_READ_QUEUE, -1, i, 0,
                                          NULL)
                    != NXT_OK)
                {
                    goto done;
                }

                written++;
            }
        }

        /* The busy worker takes the item; it never reads the socket. */
        if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "port doorbell test: dequeue %ui failed", i);
            goto done;
        }
    }

    queued = nxt_port_doorbell_test_queued(port);

    if (asked != NXT_PORT_DOORBELL_TEST_ITEMS) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port doorbell test: %ui of %d items asked for a "
                      "doorbell; the scenario no longer reproduces",
                      asked, NXT_PORT_DOORBELL_TEST_ITEMS);
        goto done;
    }

    if (queued > 1 || written >= asked) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port doorbell test: unbounded doorbells: %ui "
                      "written, %ui queued on the heap", written, queued);
        goto done;
    }

    ret = NXT_OK;

done:

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_port_test_run_error_handler(task, port);
        nxt_port_doorbell_test_drain_wq(&engine.fast_work_queue);
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    if (pair[0] != -1) {
        nxt_fd_close(pair[0]);
    }

    if (pair[1] != -1) {
        nxt_fd_close(pair[1]);
    }

    nxt_port_use(task, port, -1);
    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    thr->engine = saved_engine;
    nxt_free(q);

    if (ret == NXT_OK) {
        nxt_thread_time_update(thr);
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port doorbell test passed "
                      "(%ui doorbells asked, %ui written, %ui queued)",
                      asked, written, queued);
    }

    return ret;
}
