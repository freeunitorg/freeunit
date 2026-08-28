/*
 * Copyright (C) F5, Inc.
 */

/*
 * Failure-injection soak for issue #214.  The single-shot test proves that
 * one early failure gives the pending_processes slot back; this one proves
 * the accounting is exact under repetition and reaches all three "goto
 * failed" sites in nxt_router_start_app_process_handler():
 *
 *   site 1  the prototype buffer allocation, forced by an app conf length
 *           no allocator can satisfy;
 *   site 2  nxt_port_rpc_register_handler_ex(), forced by the NXT_TESTS
 *           rpc allocation hook;
 *   site 3  nxt_port_socket_write2(), forced by the NXT_TESTS port message
 *           allocation hook (the site the single-shot test uses).
 *
 * Every iteration seeds the counter at a baseline, adds the one increment
 * the initiator takes, and requires it back at the baseline afterwards --
 * a leak leaves baseline+1, a double release leaves baseline-1 (or wraps
 * to UINT32_MAX at baseline 0).  Both are caught without nxt_assert, so
 * the check survives a non-debug build.  Baselines alternate between 0 and
 * a non-zero value so an underflow is caught as an ordinary miscount and
 * not only as a wrap.
 *
 * After the loop the application must still be startable -- that is the
 * damage #214 does -- and one successful start must still park exactly one
 * increment on the RPC for nxt_router_app_port_error() to release.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_rpc.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


#define NXT_ROUTER_START_FAIL_SOAK_N  1000


/* File scope: the handler hands the application to the router's
   reference-counting helpers, so its storage must outlive the frame. */
static nxt_app_t  nxt_router_start_fail_soak_test_app;


/*
 * Mirrors the nxt_inline nxt_router_app_can_start() predicate in
 * src/nxt_router.c:1218.  Being nxt_inline, the gate #214 wedges is private
 * to the router's translation unit and cannot be called from here, so this
 * copy must be kept in sync with it.
 */

static nxt_bool_t
nxt_router_start_fail_soak_can_start(nxt_app_t *app)
{
    return app->processes + app->pending_processes < app->max_processes
           && app->pending_processes < app->max_pending_processes;
}


nxt_int_t
nxt_router_start_fail_soak_test(nxt_thread_t *thr)
{
    uint32_t             baseline;
    nxt_mp_t             *mp;
    nxt_app_t            *app;
    nxt_int_t            ret;
    nxt_bool_t           app_mutex;
    nxt_uint_t           i, site;
    nxt_task_t           *task;
    nxt_port_t           *router_port, *dport;
    nxt_runtime_t        *rt, *saved_rt;
    nxt_app_joint_t      *joint;
    nxt_event_engine_t   engine, *saved_engine;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router start fail soak test started");

    ret = NXT_ERROR;
    router_port = NULL;
    dport = NULL;
    joint = NULL;
    app_mutex = 0;
    app = &nxt_router_start_fail_soak_test_app;

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
    joint = nxt_mp_zalloc(mp, sizeof(nxt_app_joint_t));
    if (nxt_slow_path(rt == NULL || joint == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    rt->mem_pool = mp;

    if (nxt_slow_path(nxt_thread_mutex_create(&rt->processes_mutex) != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    /*
     * Site 1 allocates from task->thread->engine->mem_pool, so unlike the
     * single-shot fixture this one needs an engine.  Only the members the
     * reached code touches are set; see src/test/nxt_router_new_port_test.c.
     */
    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");
    engine.mem_pool = mp;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;
    thr->runtime = rt;
    thr->engine = &engine;
    rt->main_engine = &engine;

    /* The port the handler registers its RPC on. */

    router_port = nxt_port_new(task, 0, nxt_pid, NXT_PROCESS_ROUTER);
    if (nxt_slow_path(router_port == NULL)) {
        goto done;
    }

    /*
     * nxt_port_rpc_register_handler_ex() asserts on a live pair[0]; the
     * descriptor is never touched here, so borrow the nxt_port_fail_test()
     * convention of a placeholder that is reset before the port is freed.
     */
    router_port->pair[0] = 0;
    router_port->pair[1] = -1;
    router_port->socket.fd = -1;

    /*
     * The destination port.  No queue and write_ready unset, so
     * nxt_port_socket_write2() takes the nxt_port_msg_chk_insert() path:
     * the test hook fails its allocation for site 3, and without the hook
     * the message is simply queued and the write succeeds.
     */

    dport = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_APP);
    if (nxt_slow_path(dport == NULL)) {
        goto done;
    }

    dport->pair[0] = -1;
    dport->pair[1] = -1;
    dport->socket.fd = -1;

    nxt_memzero(app, sizeof(nxt_app_t));

    if (nxt_slow_path(nxt_thread_mutex_create(&app->mutex) != NXT_OK)) {
        goto done;
    }

    app_mutex = 1;

    nxt_queue_init(&app->ports);
    nxt_queue_init(&app->spare_ports);
    nxt_queue_init(&app->idle_ports);
    nxt_queue_init(&app->ack_waiting_req);

    nxt_str_set(&app->name, "start-fail-soak-test");

    app->joint = joint;
    joint->app = app;
    joint->use_count = 1;

    app->max_processes = 8;
    app->max_pending_processes = 1;

    for (i = 0; i < NXT_ROUTER_START_FAIL_SOAK_N; i++) {
        site = i % 3;
        baseline = (i % 2) ? 2 : 0;

        /* What a real initiator leaves behind: the slot and the work's use. */
        app->pending_processes = baseline + 1;
        app->use_count = 2;

        /* One process alive, so the failure path has no requests to fail. */
        app->processes = 1;
        app->proto_port_requests = 0;

        if (site == 0) {
            /*
             * Site 1: no prototype, so the handler builds the START_PROCESS
             * payload first.  A length no allocator can serve makes
             * nxt_buf_mem_alloc() return NULL before the buffer is written
             * to, so app->conf may stay empty.
             */
            app->proto_port = NULL;
            app->conf.length = (size_t) 1 << 46;

        } else {
            app->proto_port = dport;
            app->conf.length = 0;

            if (site == 1) {
                nxt_port_rpc_test_alloc_failures(1);

            } else {
                nxt_port_test_msg_alloc_failures(1);
            }
        }

        nxt_router_start_app_process_handler(task, router_port, app);

        nxt_port_rpc_test_alloc_failures(0);
        nxt_port_test_msg_alloc_failures(0);

        if (app->pending_processes != baseline) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router start fail soak test: iteration %ui site %ui "
                          "pending_processes %uD (expected %uD)",
                          i, site + 1, app->pending_processes, baseline);
            goto done;
        }

        if (app->use_count != 1) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router start fail soak test: iteration %ui site %ui "
                          "app use_count %d (expected 1)",
                          i, site + 1, (int) app->use_count);
            goto done;
        }

        if (joint->use_count != 1) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router start fail soak test: iteration %ui site %ui "
                          "joint use_count %uD (expected 1)",
                          i, site + 1, joint->use_count);
            goto done;
        }
    }

    /* The gate #214 wedges: after the failures the app must still start. */

    app->processes = 0;
    app->pending_processes = 0;
    app->conf.length = 0;
    app->proto_port = dport;

    if (!nxt_router_start_fail_soak_can_start(app)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start fail soak test: app cannot start after "
                      "%d failures, processes %uD pending %uD",
                      NXT_ROUTER_START_FAIL_SOAK_N, app->processes,
                      app->pending_processes);
        goto done;
    }

    /*
     * A start that succeeds parks the initiator's increment on the armed
     * RPC and takes a joint reference; nothing is released here.
     */

    app->pending_processes = 1;
    app->use_count = 2;

    nxt_router_start_app_process_handler(task, router_port, app);

    if (app->pending_processes != 1 || joint->use_count != 2) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start fail soak test: successful start left "
                      "pending %uD joint %uD (expected 1 and 2)",
                      app->pending_processes, joint->use_count);
        goto done;
    }

    /*
     * Tearing the port's registrations down drives the router's own error
     * handler for the armed RPC, which is how a start that never answers is
     * reported; it must give the parked increment back.
     */

    nxt_port_rpc_close(task, router_port);

    if (app->pending_processes != 0 || joint->use_count != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start fail soak test: release left pending %uD "
                      "joint %uD (expected 0 and 1)",
                      app->pending_processes, joint->use_count);
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router start fail soak test passed: %d iterations over "
                  "failure sites 1-3",
                  NXT_ROUTER_START_FAIL_SOAK_N);

    ret = NXT_OK;

done:

    if (dport != NULL) {
        nxt_port_use(task, dport, -1);
    }

    if (router_port != NULL) {
        router_port->pair[0] = -1;

        nxt_port_use(task, router_port, -1);
    }

    thr->engine = saved_engine;
    thr->runtime = saved_rt;

    if (app_mutex) {
        nxt_thread_mutex_destroy(&app->mutex);
    }

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    return ret;
}
