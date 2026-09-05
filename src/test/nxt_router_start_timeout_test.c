/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for the silent-worker start wedge: an application process
 * that is forked successfully but never calls nxt_unit_init() sends no
 * PROCESS_READY, so the START_PROCESS RPC the router armed for it was never
 * retired.  Nothing else could retire it either -- the on-demand start
 * registers that RPC with no peer (nxt_router_start_app_process_handler()),
 * and REMOVE_PID only arrives if the process dies, which a `/bin/sleep` does
 * not.  The armed handler owned an app->pending_processes slot and, on the
 * config-apply path, was the sole continuation of nxt_router_conf_apply(), so
 * the configuration PUT never returned and the controller queued every later
 * request behind it -- GET /status included.
 *
 * The fix is a deadline armed with the START_PROCESS write and cancelled by
 * whichever handler answers first.  What this test pins down is that the
 * deadline resolves the RPC *through the existing failure path* rather than
 * inventing accounting of its own: on expiry the application must get its
 * pending_processes slot back and its proto_port_requests cohort cleared,
 * exactly as nxt_router_app_port_error() does for a start that fails for any
 * other reason, and the application must be able to start a process again
 * afterwards.
 *
 * The other half is the race the deadline creates: a reply that lands after
 * the timer has been queued must not free the timer struct out from under the
 * expiry handler, and a cancelled timer must not leave a node in the engine's
 * rbtree.  Both are driven here against a real nxt_timers_t, because a
 * memzero'd engine would let a broken cancel look correct.
 *
 * The arrangement is the router's own: no prototype port yet, so the handler
 * builds a START_PROCESS payload and arms an RPC with ->proto set; the message
 * only queues (main's port has no queue and write_ready unset), so the RPC
 * stays armed and nothing ever answers it.
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
static nxt_app_t  nxt_router_start_timeout_test_app;


/*
 * Commit the pending timer changes and run every timer that is due, the way
 * nxt_event_engine_start() does: nxt_timer_find() folds the change list into
 * the rbtree, nxt_timer_expire() moves what is due onto the work queue, and
 * the work queue is then drained.  Returns the number of timer handlers that
 * actually ran.
 */

static nxt_uint_t
nxt_router_start_timeout_test_tick(nxt_task_t *task, nxt_event_engine_t *engine,
    nxt_msec_t advance)
{
    nxt_uint_t          ran;
    nxt_task_t          *wq_task;
    nxt_work_handler_t  handler;
    void                *obj, *data;

    (void) nxt_timer_find(engine);

    nxt_timer_expire(engine, engine->timers.now + advance);

    ran = 0;

    /* nxt_work_queue_pop() dereferences the head unconditionally. */

    while (engine->fast_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine->fast_work_queue, &wq_task, &obj,
                                     &data);

        if (handler == NULL) {
            continue;
        }

        ran++;

        handler(wq_task != NULL ? wq_task : task, obj, data);
    }

    return ran;
}


nxt_int_t
nxt_router_start_timeout_test(nxt_thread_t *thr)
{
    nxt_mp_t            *mp;
    nxt_app_t           *app;
    nxt_int_t           ret;
    nxt_uint_t          ran;
    nxt_bool_t          app_mutex, timers;
    nxt_task_t          *task;
    nxt_port_t          *router_port, *main_port, *shared_port;
    nxt_runtime_t       *rt, *saved_rt;
    nxt_app_joint_t     *joint;
    nxt_event_engine_t  engine, *saved_engine;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router start timeout test started");

    ret = NXT_ERROR;
    router_port = NULL;
    main_port = NULL;
    shared_port = NULL;
    joint = NULL;
    app_mutex = 0;
    timers = 0;
    app = &nxt_router_start_timeout_test_app;

    task = thr->task;
    task->thread = thr;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;

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

    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");
    engine.mem_pool = mp;

    /*
     * The deadline runs on the engine's own task, as it does in the router,
     * so that task must be usable: a memzero'd one has a NULL log and the
     * first nxt_debug() from nxt_timer_add() dereferences it.
     */

    engine.task.thread = thr;
    engine.task.log = thr->log;

    /*
     * A real timer machinery, not the memzero'd one the other router tests
     * get away with: nxt_timer_add() writes into timers->changes, and a
     * cancel that leaves a node in this rbtree is precisely the defect the
     * teardown check below looks for.
     */

    if (nxt_slow_path(nxt_timers_init(&engine.timers, 64) != NXT_OK)) {
        goto done;
    }

    timers = 1;

    thr->runtime = rt;
    thr->engine = &engine;
    rt->main_engine = &engine;

    /* The port the handler registers its RPC on. */

    router_port = nxt_port_new(task, 0, nxt_pid, NXT_PROCESS_ROUTER);
    if (nxt_slow_path(router_port == NULL)) {
        goto done;
    }

    router_port->pair[0] = 0;
    router_port->pair[1] = -1;
    router_port->socket.fd = -1;

    /*
     * A prototype start addresses the main process.  No queue and write_ready
     * unset, so nxt_port_socket_write2() takes the nxt_port_msg_chk_insert()
     * path: the write succeeds, the message is queued and the RPC stays armed
     * -- a start that nobody will ever answer, which is the case under test.
     */

    main_port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_MAIN);
    if (nxt_slow_path(main_port == NULL)) {
        goto done;
    }

    main_port->pair[0] = -1;
    main_port->pair[1] = -1;
    main_port->socket.fd = -1;

    rt->port_by_type[NXT_PROCESS_MAIN] = main_port;

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

    nxt_str_set(&app->name, "start-timeout-test");

    app->joint = joint;
    joint->app = app;
    joint->use_count = 1;

    app->shared_port = shared_port;
    app->max_processes = 8;
    app->max_pending_processes = 4;
    app->start_timeout = 1000;

    app->proto_port = NULL;

    /* One process alive, so the failure path has no requests to fail. */
    app->processes = 1;

    /* What a real initiator leaves behind: the slot and the work's use. */
    app->pending_processes = 1;
    app->use_count = 2;

    nxt_router_start_app_process_handler(task, router_port, app);

    if (app->proto_port_requests != 1 || joint->use_count != 2) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: start left "
                      "proto_port_requests %uD joint %uD (expected 1 and 2)",
                      app->proto_port_requests, joint->use_count);
        goto done;
    }

    /*
     * Well before the deadline nothing may fire: a deadline that expires
     * early would fail starts that are merely slow.
     */

    ran = nxt_router_start_timeout_test_tick(task, &engine, 100);

    if (ran != 0 || app->pending_processes != 1
        || app->proto_port_requests != 1)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: deadline fired early, "
                      "%ui handlers, pending %uD proto_port_requests %uD "
                      "(expected 0, 1 and 1)",
                      ran, app->pending_processes, app->proto_port_requests);
        goto done;
    }

    /*
     * Past the deadline the RPC must resolve through the router's own error
     * handler: the cohort cleared and every slot it owned given back.  Before
     * the fix nothing was armed at all, so this tick ran no handler and left
     * both counters exactly as they were.
     */

    ran = nxt_router_start_timeout_test_tick(task, &engine, 2000);

    if (ran == 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: nothing bounded the start, "
                      "the RPC is armed forever");
        goto done;
    }

    if (app->proto_port_requests != 0 || app->pending_processes != 0
        || joint->use_count != 1)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: expired start left "
                      "proto_port_requests %uD pending %uD joint %uD "
                      "(expected 0, 0 and 1)",
                      app->proto_port_requests, app->pending_processes,
                      joint->use_count);
        goto done;
    }

    /*
     * And the application is not wedged by its own deadline: with the cohort
     * clear a later call must send a fresh prototype request -- arming an RPC
     * and taking a joint reference -- rather than parking in the wait branch.
     */

    app->pending_processes = 1;
    app->use_count = 2;

    nxt_router_start_app_process_handler(task, router_port, app);

    if (app->proto_port_requests != 1 || joint->use_count != 2) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: app wedged after an expired "
                      "start, proto_port_requests %uD joint %uD "
                      "(expected 1 and 2)",
                      app->proto_port_requests, joint->use_count);
        goto done;
    }

    /*
     * The other order: an answer arrives first.  Closing the port's
     * registrations drives nxt_router_app_port_error(), which must cancel the
     * armed deadline.  A cancel that only nxt_timer_disable()d it would leave
     * a node in the rbtree pointing at freed memory; the tick below walks
     * that tree, and the teardown check requires it empty.
     */

    nxt_port_rpc_close(task, router_port);

    if (app->proto_port_requests != 0 || app->pending_processes != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: answered start left "
                      "proto_port_requests %uD pending %uD (expected 0 and 0)",
                      app->proto_port_requests, app->pending_processes);
        goto done;
    }

    /*
     * Two ticks: the first lets a deferred release run, the second must find
     * nothing left.  A cancelled deadline that still fired would report the
     * application as failing a start it never made.
     */

    (void) nxt_router_start_timeout_test_tick(task, &engine, 2000);

    ran = nxt_router_start_timeout_test_tick(task, &engine, 2000);

    if (ran != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: %ui handlers still pending "
                      "after a cancelled deadline (expected 0)", ran);
        goto done;
    }

    if (!nxt_rbtree_is_empty(&engine.timers.tree)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: a cancelled deadline left a "
                      "node in the engine's timer tree");
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router start timeout test passed");

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
         * alone the port never reaches nxt_port_mp_cleanup(), so its pool and
         * messages leak and the assertions there never run.
         */
        nxt_port_test_run_error_handler(task, main_port);

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

    if (timers) {
        nxt_free(engine.timers.changes);
    }

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    return ret;
}
