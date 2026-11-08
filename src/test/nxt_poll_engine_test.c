/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


/*
 * A write enable on an event whose write side is already active.  The port
 * layer does this on purpose: nxt_port_rearm_now() enables the write event
 * without testing its state, because with an edge-triggered engine a
 * registration left active on a socket that stayed writable reports
 * nothing more, and only a new enable brings the write handler back.  epoll
 * adds a descriptor only when both of its sides are inactive.  The poll
 * engine added it again whenever the read side was inactive, the hash
 * refused the duplicate descriptor, and the commit queued the event's error
 * handler: a port was ended instead of re-armed.
 *
 * A full disable is the other half of the same state machine.  It has to
 * remove the descriptor whenever either side is registered, and to say so
 * to the caller of nxt_poll_close(), which defers the socket close until
 * the removal is committed.  It did neither with one side already
 * inactive, which is the state a blocked write leaves behind.
 */

static void nxt_poll_engine_test_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_poll_engine_test_error(nxt_task_t *task, void *obj,
    void *data);
static void nxt_poll_engine_test_drain(nxt_task_t *task,
    nxt_event_engine_t *engine);


static nxt_uint_t  nxt_poll_engine_test_errors;


nxt_int_t
nxt_poll_engine_test(nxt_thread_t *thr)
{
    nxt_fd_t            pair[2];
    nxt_int_t           ret;
    nxt_task_t          *task;
    nxt_fd_event_t      ev;
    nxt_event_engine_t  *engine, *saved_engine;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "poll engine test started");

    task = thr->task;
    task->thread = thr;

    ret = NXT_ERROR;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "poll engine test: socketpair failed");
        return NXT_ERROR;
    }

    engine = nxt_event_engine_create(task, &nxt_poll_engine, NULL, 0, 0);
    if (nxt_slow_path(engine == NULL)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "poll engine test: no poll engine");
        goto close;
    }

    saved_engine = thr->engine;
    thr->engine = engine;

    /* The engine's own changes, such as its signal pipe, go first. */

    engine->event.poll(engine, 0);
    nxt_poll_engine_test_drain(task, engine);

    nxt_memzero(&ev, sizeof(nxt_fd_event_t));

    ev.fd = pair[1];
    ev.task = task;
    ev.log = thr->log;
    ev.read_work_queue = &engine->fast_work_queue;
    ev.write_work_queue = &engine->fast_work_queue;
    ev.read_handler = nxt_poll_engine_test_handler;
    ev.write_handler = nxt_poll_engine_test_handler;
    ev.error_handler = nxt_poll_engine_test_error;

    nxt_poll_engine_test_errors = 0;

    /* The first enable registers the descriptor; the second finds it active. */

    nxt_fd_event_enable_write(engine, &ev);

    engine->event.poll(engine, 0);
    nxt_poll_engine_test_drain(task, engine);

    nxt_fd_event_enable_write(engine, &ev);

    engine->event.poll(engine, 0);
    nxt_poll_engine_test_drain(task, engine);

    if (nxt_poll_engine_test_errors != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "poll engine test: a write enable on an active event "
                      "raised the error handler %ui times",
                      nxt_poll_engine_test_errors);
        goto done;
    }

    /* The same with the read side: enabled twice, never an error. */

    nxt_fd_event_disable_write(engine, &ev);

    engine->event.poll(engine, 0);
    nxt_poll_engine_test_drain(task, engine);

    nxt_fd_event_enable_read(engine, &ev);
    nxt_fd_event_enable_read(engine, &ev);

    engine->event.poll(engine, 0);
    nxt_poll_engine_test_drain(task, engine);

    if (nxt_poll_engine_test_errors != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "poll engine test: a read enable on an active event "
                      "raised the error handler %ui times",
                      nxt_poll_engine_test_errors);
        goto done;
    }

    /*
     * The other half: a full disable with the write side already inactive,
     * which is the state a blocked write leaves behind and the one
     * nxt_poll_close() reaches.  It must queue the removal and report it,
     * so that the caller of the close defers the socket close.  Without
     * that, the descriptor stays in the set and the closed socket is left
     * for poll() to report as POLLNVAL.
     */

    nxt_poll_engine_test_errors = 0;

    if (nxt_fd_event_close(engine, &ev) == 0
        || ev.read != NXT_EVENT_INACTIVE
        || ev.write != NXT_EVENT_INACTIVE)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "poll engine test: a full disable with the write side "
                      "inactive queued no removal, read %d write %d",
                      ev.read, ev.write);
        goto done;
    }

    engine->event.poll(engine, 0);
    nxt_poll_engine_test_drain(task, engine);

    if (nxt_poll_engine_test_errors != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "poll engine test: removing the descriptor raised the "
                      "error handler %ui times",
                      nxt_poll_engine_test_errors);
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_fd_event_disable(engine, &ev);

    engine->event.poll(engine, 0);
    nxt_poll_engine_test_drain(task, engine);

    thr->engine = saved_engine;

    nxt_event_engine_free(engine);

close:

    nxt_socket_close(task, pair[0]);
    nxt_socket_close(task, pair[1]);

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "poll engine test passed");
    }

    return ret;
}


static void
nxt_poll_engine_test_handler(nxt_task_t *task, void *obj, void *data)
{
}


static void
nxt_poll_engine_test_error(nxt_task_t *task, void *obj, void *data)
{
    nxt_poll_engine_test_errors++;
}


static void
nxt_poll_engine_test_drain(nxt_task_t *task, nxt_event_engine_t *engine)
{
    void                *obj, *data;
    nxt_task_t          *wq_task;
    nxt_work_handler_t  handler;

    /* nxt_work_queue_pop() dereferences the head unconditionally. */

    while (engine->fast_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine->fast_work_queue, &wq_task, &obj,
                                     &data);

        if (handler != NULL) {
            handler(wq_task != NULL ? wq_task : task, obj, data);
        }
    }
}
