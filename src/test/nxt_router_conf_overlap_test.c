/*
 * Copyright (C) F5, Inc.
 */

/*
 * The router applies one configuration at a time, because all applies share
 * the socket queues.  Two applies free one listener twice:
 * https://github.com/freeunitorg/freeunit/issues/687
 *
 * The test checks that:
 *
 *   - a configuration that comes during an apply waits and gets no answer;
 *     an error would make the controller discard its stored configuration;
 *   - a newer configuration replaces the waiting one, which gets an error;
 *   - when the apply ends, the waiting configuration starts from the work
 *     queue.  It has no descriptor, so its start answers RPC_ERROR;
 *   - a configuration that comes after the apply ends, but before the
 *     waiting one starts, replaces it rather than starting first.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_hash.h>
#include <nxt_port_rpc.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


nxt_int_t
nxt_router_conf_overlap_test(nxt_thread_t *thr)
{
    nxt_mp_t                *mp;
    nxt_int_t               ret;
    nxt_buf_t               *b;
    nxt_uint_t              deferred, queued;
    nxt_task_t              *task;
    nxt_port_t              *port;
    nxt_router_t            test_router;
    nxt_runtime_t           *rt, *saved_rt;
    nxt_work_handler_t      handler;
    nxt_port_recv_msg_t     msg;
    nxt_event_engine_t      engine, *saved_engine;
    nxt_router_temp_conf_t  *tmcf;
    void                    *obj, *data;
    nxt_task_t              *wq_task;
    nxt_queue_link_t        *lnk;
    nxt_port_send_msg_t     *sm;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router conf overlap test started");

    ret = NXT_ERROR;
    port = NULL;
    saved_rt = thr->runtime;
    saved_engine = thr->engine;

    task = thr->task;
    task->thread = thr;

    if (nxt_slow_path(nxt_port_rpc_init() != NXT_OK)) {
        return NXT_ERROR;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    if (nxt_slow_path(rt == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    rt->mem_pool = mp;
    thr->runtime = rt;

    /* An engine: the port error handler queues its completions on it. */

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");
    engine.mem_pool = mp;
    engine.task.thread = thr;
    engine.task.log = thr->log;

    thr->engine = &engine;

    nxt_memzero(&test_router, sizeof(nxt_router_t));
    nxt_queue_init(&test_router.sockets);
    nxt_queue_init(&test_router.apps);

    /* The reply port.  It has no fd, so the answers stay in its queue. */

    port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_CONTROLLER);
    if (nxt_slow_path(port == NULL)) {
        goto done;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    if (nxt_slow_path(nxt_port_hash_add(&rt->ports, port) != NXT_OK)) {
        goto done;
    }

    /* An apply is in flight.  A new configuration arrives. */

    nxt_router_test_conf_applying_set(1);

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    b = nxt_buf_mem_alloc(mp, sizeof(size_t), 0);
    if (nxt_slow_path(b == NULL)) {
        goto done;
    }

    nxt_memzero(b->mem.free, sizeof(size_t));
    b->mem.free += sizeof(size_t);

    msg.port_msg.pid = nxt_pid;
    msg.port_msg.reply_port = port->id;
    msg.port_msg.stream = 1;
    msg.buf = b;
    msg.fd[0] = -1;
    msg.fd[1] = -1;

    deferred = nxt_router_test_confs_deferred;

    nxt_router_test_conf_data_handler(task, &msg);

    if (nxt_router_test_confs_deferred != deferred + 1
        || !nxt_router_test_conf_deferred())
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router conf overlap test: a configuration that came "
                      "during an apply did not wait for it");
        goto done;
    }

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router conf overlap test: a configuration that came "
                      "during an apply was answered instead of waiting");
        goto done;
    }

    if (!nxt_router_test_conf_applying()) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router conf overlap test: the waiting configuration "
                      "ended the apply that was in flight");
        goto done;
    }

    /* A newer configuration replaces the waiting one, which gets an error. */

    b->mem.pos = b->mem.start;
    msg.port_msg.stream = 2;

    nxt_router_test_conf_data_handler(task, &msg);

    if (nxt_router_test_confs_deferred != deferred + 2
        || !nxt_router_test_conf_deferred())
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router conf overlap test: a newer configuration did "
                      "not take the place of the waiting one");
        goto done;
    }

    queued = 0;

    for (lnk = nxt_queue_first(&port->messages);
         lnk != nxt_queue_tail(&port->messages);
         lnk = nxt_queue_next(lnk))
    {
        queued++;
    }

    if (queued != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router conf overlap test: %ui answers after a newer "
                      "configuration replaced the waiting one, expected 1",
                      queued);
        goto done;
    }

    /* The apply in flight fails.  That ends it. */

    tmcf = nxt_router_test_temp_conf(task);
    if (nxt_slow_path(tmcf == NULL)) {
        goto done;
    }

    tmcf->router_conf->router = &test_router;
    tmcf->port = port;

    /* nxt_router_conf_send() answers on it and drops a reference for it. */
    nxt_port_use(task, port, 1);

    nxt_router_conf_error(task, tmcf);

    if (nxt_router_test_conf_applying()) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router conf overlap test: a failed apply did not end, "
                      "every later configuration would wait for ever");
        goto done;
    }

    /*
     * The waiting configuration starts from the work queue.  One that
     * arrives before that runs must not start first, or the older one would
     * be applied after it: it replaces the waiting one instead.
     */

    b->mem.pos = b->mem.start;
    msg.port_msg.stream = 3;

    nxt_router_test_conf_data_handler(task, &msg);

    if (nxt_router_test_confs_deferred != deferred + 3
        || !nxt_router_test_conf_deferred()
        || nxt_router_test_conf_applying())
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router conf overlap test: a configuration that came "
                      "before the waiting one started did not replace it");
        goto done;
    }


    while (engine.fast_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine.fast_work_queue, &wq_task, &obj,
                                     &data);

        if (handler != NULL) {
            handler(wq_task != NULL ? wq_task : task, obj, data);
        }
    }

    if (nxt_router_test_conf_deferred()) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router conf overlap test: the waiting configuration "
                      "did not start when the apply ended");
        goto done;
    }

    /*
     * Answers: two replaced configurations, the failed apply, and the newest
     * configuration, which started last.
     */

    queued = 0;

    for (lnk = nxt_queue_first(&port->messages);
         lnk != nxt_queue_tail(&port->messages);
         lnk = nxt_queue_next(lnk))
    {
        queued++;
    }

    if (queued != 4) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router conf overlap test: %ui answers on the reply "
                      "port, expected 4", queued);
        goto done;
    }

    sm = nxt_queue_link_data(nxt_queue_last(&port->messages),
                             nxt_port_send_msg_t, link);

    if (sm->port_msg.stream != 3) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router conf overlap test: stream %uD started last, "
                      "expected the newest, 3", sm->port_msg.stream);
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router conf overlap test passed");

    ret = NXT_OK;

done:

    nxt_router_test_conf_applying_set(0);

    if (port != NULL) {
        (void) nxt_port_hash_remove(&rt->ports, port);

        /* Drop the queued answers: each holds a port reference. */
        nxt_port_test_run_error_handler(task, port);

        nxt_port_use(task, port, -1);
    }

    /* nxt_work_queue_pop() dereferences the head unconditionally. */

    while (engine.fast_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine.fast_work_queue, &wq_task, &obj,
                                     &data);

        if (handler != NULL) {
            handler(wq_task != NULL ? wq_task : task, obj, data);
        }
    }

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);

    thr->engine = saved_engine;
    thr->runtime = saved_rt;

    nxt_mp_destroy(mp);

    return ret;
}
