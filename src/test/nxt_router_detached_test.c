/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for the sender check in nxt_router_detached_handler()
 * (src/nxt_router.c).
 *
 * Every application worker holds a copy of the router port, and the pid in
 * a port message is chosen by the sender.  A detached edge that named
 * another worker's pid used to be applied to that worker: a forged start
 * pinned it busy together with a reference to its application, and a forged
 * finish let the reaper take a worker that was still running.  The edge is
 * now applied only when the kernel's credential names the same process.
 *
 * The fixture is one application with the main port of one worker.  The port
 * has no socket (pair[1] is -1) and is in no idle queue, so the edges change
 * only ->detached, the count and the application reference, and those are
 * what the test reads.  Each forged edge is followed by the genuine one, which
 * shows that the fixture reaches the accounting at all.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


#if (NXT_USE_CMSG_PID)

static nxt_int_t
nxt_router_detached_test_edge(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_t *port, nxt_app_t *app, uint8_t state, nxt_pid_t sender,
    nxt_bool_t detached, nxt_atomic_t use_count, const char *name)
{
    nxt_buf_t            b;
    nxt_port_recv_msg_t  msg;

    nxt_memzero(&b, sizeof(nxt_buf_t));
    b.mem.start = &state;
    b.mem.pos = &state;
    b.mem.free = &state + 1;
    b.mem.end = &state + 1;

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));
    msg.buf = &b;
    msg.size = 1;
    msg.fd[0] = -1;
    msg.fd[1] = -1;
    msg.port_msg.pid = port->pid;
    msg.port_msg.type = _NXT_PORT_MSG_DETACHED;
    msg.port_msg.last = 1;
    msg.cmsg_pid = sender;

    nxt_router_test_detached_handler(task, &msg);

    if (port->detached != detached
        || app->detached_processes != (detached ? 1 : 0)
        || app->use_count != use_count)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router detached test: %s: detached %d, count %uD, "
                      "use count %d; expected %d, %d, %d",
                      name, (int) port->detached, app->detached_processes,
                      (int) app->use_count, (int) detached,
                      detached ? 1 : 0, (int) use_count);
        return NXT_ERROR;
    }

    return NXT_OK;
}

#endif


nxt_int_t
nxt_router_detached_test(nxt_thread_t *thr)
{
#if !(NXT_USE_CMSG_PID)

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router detached test skipped: no sender credentials");
    return NXT_OK;

#else

    nxt_mp_t            *mp;
    nxt_app_t           *app;
    nxt_int_t           ret;
    nxt_pid_t           pid, other;
    nxt_task_t          *task;
    nxt_port_t          *port;
    nxt_bool_t          app_mutex, hashed;
    nxt_runtime_t       *rt, *saved_rt;
    nxt_event_engine_t  engine, *saved_engine;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router detached test started");

    ret = NXT_ERROR;
    port = NULL;
    app_mutex = 0;
    hashed = 0;

    task = thr->task;
    task->thread = thr;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    app = nxt_mp_zalloc(mp, sizeof(nxt_app_t));
    if (nxt_slow_path(rt == NULL || app == NULL)) {
        goto done;
    }

    rt->mem_pool = mp;

    /* The handler runs on the main engine only. */

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));

    thr->runtime = rt;
    thr->engine = &engine;
    rt->main_engine = &engine;

    if (nxt_slow_path(nxt_thread_mutex_create(&app->mutex) != NXT_OK)) {
        goto done;
    }

    app_mutex = 1;

    nxt_queue_init(&app->ports);
    nxt_queue_init(&app->spare_ports);
    nxt_queue_init(&app->idle_ports);
    nxt_queue_init(&app->ack_waiting_req);

    nxt_str_set(&app->name, "detached-test");

    /* Never reached by the edges below, so nothing frees the application. */
    app->use_count = 8;

    /* A pid that is not this process, so a sender of nxt_pid is foreign. */

    pid = nxt_pid + 1;
    other = nxt_pid;

    port = nxt_port_new(task, 0, pid, NXT_PROCESS_APP);
    if (nxt_slow_path(port == NULL)) {
        goto done;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;
    port->app = app;

    if (nxt_slow_path(nxt_port_hash_add(&rt->ports, port) != NXT_OK)) {
        goto done;
    }

    hashed = 1;

    if (nxt_router_detached_test_edge(thr, task, port, app,
                                      NXT_PORT_DETACHED_START, other, 0, 8,
                                      "a forged start")
        != NXT_OK
        || nxt_router_detached_test_edge(thr, task, port, app,
                                         NXT_PORT_DETACHED_START, pid, 1, 9,
                                         "a genuine start")
           != NXT_OK
        || nxt_router_detached_test_edge(thr, task, port, app,
                                         NXT_PORT_DETACHED_FINISH, other, 1, 9,
                                         "a forged finish")
           != NXT_OK
        || nxt_router_detached_test_edge(thr, task, port, app,
                                         NXT_PORT_DETACHED_FINISH, -1, 1, 9,
                                         "a finish with no credential")
           != NXT_OK
        || nxt_router_detached_test_edge(thr, task, port, app,
                                         NXT_PORT_DETACHED_FINISH, pid, 0, 8,
                                         "a genuine finish")
           != NXT_OK)
    {
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router detached test passed");

    ret = NXT_OK;

done:

    if (hashed) {
        (void) nxt_port_hash_remove(&rt->ports, port);
    }

    if (port != NULL) {
        port->app = NULL;
        nxt_port_use(task, port, -1);
    }

    thr->engine = saved_engine;
    thr->runtime = saved_rt;

    if (app_mutex) {
        nxt_thread_mutex_destroy(&app->mutex);
    }

    nxt_mp_destroy(mp);

    return ret;

#endif
}
