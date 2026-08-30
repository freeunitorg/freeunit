/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for the prototype-start wedge: a prototype START_PROCESS
 * RPC that resolves through nxt_router_app_port_error() instead of
 * nxt_router_app_port_ready() used to leave app->proto_port_requests set.
 *
 * Only port_ready() cleared that counter, so once it was stranded every
 * later nxt_router_start_app_process_handler() call for the application
 * took the "wait for prototype process" branch at src/nxt_router.c:430 and
 * parked without sending anything.  Nothing was left to send a prototype
 * request, so no port_ready or port_error could fire to clear it: the
 * application could never start another process for the router's lifetime,
 * and a reload that left its config block unchanged reused the wedged
 * nxt_app_t rather than replacing it.
 *
 * The stranded counter also stranded pending_processes.  Each parked caller
 * holds one increment its initiator took, but port_error() released exactly
 * one -- so a cohort of N parked callers leaked N-1 start slots on top of
 * the wedge.
 *
 * The arrangement here is the one the router really uses: no prototype port
 * yet, so the handler builds a START_PROCESS payload and arms an RPC with
 * ->proto set; a second call then joins the wait.  Tearing the port's
 * registrations down with nxt_port_rpc_close() drives the router's own
 * error handler for the armed RPC, which is how a start that never answers
 * is reported.  The test then requires what the wedge denied: the counter
 * cleared, every slot in the cohort given back, and a later call actually
 * sending a fresh prototype request instead of parking.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_rpc.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


/* File scope: the handler hands the application to the router's
   reference-counting helpers, so its storage must outlive the frame. */
static nxt_app_t  nxt_router_proto_wedge_test_app;


nxt_int_t
nxt_router_proto_wedge_test(nxt_thread_t *thr)
{
    nxt_mp_t            *mp;
    nxt_app_t           *app;
    nxt_int_t           ret;
    nxt_bool_t          app_mutex;
    nxt_task_t          *task;
    nxt_port_t          *router_port, *main_port, *shared_port;
    nxt_runtime_t       *rt, *saved_rt;
    nxt_app_joint_t     *joint;
    nxt_event_engine_t  engine, *saved_engine;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router proto wedge test started");

    ret = NXT_ERROR;
    router_port = NULL;
    main_port = NULL;
    shared_port = NULL;
    joint = NULL;
    app_mutex = 0;
    app = &nxt_router_proto_wedge_test_app;

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
     * The prototype branch allocates its payload from
     * task->thread->engine->mem_pool.  Only the members the reached code
     * touches are set; see src/test/nxt_router_new_port_test.c.
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
     * A prototype start addresses the main process.  No queue and
     * write_ready unset, so nxt_port_socket_write2() takes the
     * nxt_port_msg_chk_insert() path and the message is simply queued:
     * the write succeeds and the RPC stays armed, which is what this test
     * needs.
     */

    main_port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_MAIN);
    if (nxt_slow_path(main_port == NULL)) {
        goto done;
    }

    main_port->pair[0] = -1;
    main_port->pair[1] = -1;
    main_port->socket.fd = -1;

    rt->port_by_type[NXT_PROCESS_MAIN] = main_port;

    /* Only its two descriptors are read, and both travel as -1. */

    shared_port = nxt_port_new(task, 2, nxt_pid, NXT_PROCESS_APP);
    if (nxt_slow_path(shared_port == NULL)) {
        goto done;
    }

    shared_port->pair[0] = -1;
    shared_port->pair[1] = -1;
    shared_port->socket.fd = -1;
    shared_port->queue_fd = -1;

    nxt_memzero(app, sizeof(nxt_app_t));

    if (nxt_slow_path(nxt_thread_mutex_create(&app->mutex) != NXT_OK)) {
        goto done;
    }

    app_mutex = 1;

    nxt_queue_init(&app->ports);
    nxt_queue_init(&app->spare_ports);
    nxt_queue_init(&app->idle_ports);
    nxt_queue_init(&app->ack_waiting_req);

    nxt_str_set(&app->name, "proto-wedge-test");

    app->joint = joint;
    joint->app = app;
    joint->use_count = 1;

    app->shared_port = shared_port;
    app->max_processes = 8;
    app->max_pending_processes = 4;

    /* No prototype port yet: the next call starts one. */
    app->proto_port = NULL;

    /* One process alive, so the failure path has no requests to fail. */
    app->processes = 1;

    /* What a real initiator leaves behind: the slot and the work's use. */
    app->pending_processes = 1;
    app->use_count = 2;

    nxt_router_start_app_process_handler(task, router_port, app);

    if (app->proto_port_requests != 1 || joint->use_count != 2
        || app->use_count != 1)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router proto wedge test: prototype start left "
                      "proto_port_requests %uD joint %uD app use %d "
                      "(expected 1, 2 and 1)",
                      app->proto_port_requests, joint->use_count,
                      (int) app->use_count);
        goto done;
    }

    /*
     * A second caller finds the request already in flight and joins the
     * wait, adding its own initiator increment to both counters.
     */

    app->pending_processes = 2;
    app->use_count = 2;

    nxt_router_start_app_process_handler(task, router_port, app);

    if (app->proto_port_requests != 2 || joint->use_count != 2
        || app->use_count != 1)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router proto wedge test: parked caller left "
                      "proto_port_requests %uD joint %uD app use %d "
                      "(expected 2, 2 and 1)",
                      app->proto_port_requests, joint->use_count,
                      (int) app->use_count);
        goto done;
    }

    /*
     * The prototype never answers.  Closing the port's registrations drives
     * nxt_router_app_port_error() for the armed RPC -- the wedge is what it
     * used to leave behind.
     */

    nxt_port_rpc_close(task, router_port);

    if (app->proto_port_requests != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router proto wedge test: failed prototype left "
                      "proto_port_requests %uD (expected 0)",
                      app->proto_port_requests);
        goto done;
    }

    if (app->pending_processes != 0 || joint->use_count != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router proto wedge test: failed prototype left "
                      "pending %uD joint %uD (expected 0 and 1)",
                      app->pending_processes, joint->use_count);
        goto done;
    }

    /*
     * The behaviour the wedge denied: with no prototype port and the
     * counter clear, a later call must send a fresh prototype request --
     * arming an RPC and taking a joint reference -- rather than parking in
     * the wait branch, which takes neither.
     */

    app->pending_processes = 1;
    app->use_count = 2;

    nxt_router_start_app_process_handler(task, router_port, app);

    if (app->proto_port_requests != 1 || joint->use_count != 2
        || app->use_count != 1)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router proto wedge test: app wedged after a failed "
                      "prototype, proto_port_requests %uD joint %uD "
                      "app use %d (expected 1, 2 and 1)",
                      app->proto_port_requests, joint->use_count,
                      (int) app->use_count);
        goto done;
    }

    /* Release the second armed RPC the same way. */

    nxt_port_rpc_close(task, router_port);

    if (app->pending_processes != 0 || joint->use_count != 1
        || app->proto_port_requests != 0)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router proto wedge test: second release left pending "
                      "%uD joint %uD proto_port_requests %uD "
                      "(expected 0, 1 and 0)",
                      app->pending_processes, joint->use_count,
                      app->proto_port_requests);
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router proto wedge test passed");

    ret = NXT_OK;

done:

    if (shared_port != NULL) {
        nxt_port_use(task, shared_port, -1);
    }

    if (main_port != NULL) {
        rt->port_by_type[NXT_PROCESS_MAIN] = NULL;

        /*
         * Both prototype requests were queued rather than written, and
         * nxt_port_msg_chk_insert() takes a port reference for each.  Left
         * alone the port never reaches nxt_port_mp_cleanup(), so its pool
         * and messages leak and the assertions there never run -- which
         * would quietly hollow out this test under a debug build.
         */
        nxt_port_test_run_error_handler(task, main_port);

        if (!nxt_queue_is_empty(&main_port->messages)) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router proto wedge test: main port still holds "
                          "queued messages after teardown");
            ret = NXT_ERROR;
        }

        nxt_port_use(task, main_port, -1);
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
