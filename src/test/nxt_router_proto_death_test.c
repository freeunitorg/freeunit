/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for issue #269: a prototype that dies between forking a
 * worker and that worker's PROCESS_READY strands the router's on-demand
 * START_PROCESS registration.
 *
 * The router hears about the death as a REMOVE_PID for the prototype.  That
 * message carries no stream once the prototype itself has reached READY
 * (src/nxt_main_process.c:1200), so the only thing left that can retire the
 * armed RPC is nxt_port_rpc_remove_peer() (src/nxt_port.c:1067) -- and it
 * retires registrations by peer.  nxt_router_start_app_process_handler()
 * used to arm its registration with no peer at all, so the sweep found
 * nothing, the attempt's pending_processes slot was never given back, and
 * with "processes: {spare: 0}" (max_pending_processes 1)
 * nxt_router_app_can_start() stayed false for the router's lifetime: every
 * later request parked in ack_waiting_req and no replacement prototype was
 * ever asked for, because only the start handler asks for one and can_start
 * is what gates it.
 *
 * This is the sibling of src/test/nxt_router_proto_wedge_test.c, which
 * covers the prototype-start branch retired through nxt_port_rpc_close().
 * Here the application already has a prototype, so the handler takes the
 * plain app-start branch, and the RPC is retired the way the real
 * REMOVE_PID does it: by peer.  Before the fix the sweep is a no-op and the
 * checks below fail; after it the sweep runs nxt_router_app_port_error()
 * exactly once, the slot comes back, and the application is startable again.
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
static nxt_app_t  nxt_router_proto_death_test_app;


/*
 * Mirrors the nxt_inline nxt_router_app_can_start() predicate in
 * src/nxt_router.c.  Being nxt_inline, the gate #269 wedges is private to
 * the router's translation unit and cannot be called from here, so this copy
 * must be kept in sync with it.
 */

static nxt_bool_t
nxt_router_proto_death_can_start(nxt_app_t *app)
{
    return app->processes + app->pending_processes < app->max_processes
           && app->pending_processes < app->max_pending_processes;
}


nxt_int_t
nxt_router_proto_death_test(nxt_thread_t *thr)
{
    nxt_mp_t            *mp;
    nxt_app_t           *app;
    nxt_int_t           ret;
    nxt_bool_t          app_mutex;
    nxt_task_t          *task;
    nxt_port_t          *router_port, *proto_port;
    nxt_runtime_t       *rt, *saved_rt;
    nxt_app_joint_t     *joint;
    nxt_event_engine_t  engine, *saved_engine;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router proto death test started");

    ret = NXT_ERROR;
    router_port = NULL;
    proto_port = NULL;
    joint = NULL;
    app_mutex = 0;
    app = &nxt_router_proto_death_test_app;

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
     * Only the members the reached code touches are set; see
     * src/test/nxt_router_new_port_test.c.
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
     * The prototype the START_PROCESS is written to, and whose pid the
     * REMOVE_PID sweep is driven with.  No queue and write_ready unset, so
     * nxt_port_socket_write2() takes the nxt_port_msg_chk_insert() path and
     * the message is simply queued: the write succeeds and the RPC stays
     * armed, which is what this test needs.
     *
     * The port's pid is the running process, as everywhere in these
     * fixtures; the sweep only ever compares it with the value the handler
     * recorded as the registration's peer.
     */

    proto_port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_PROTOTYPE);
    if (nxt_slow_path(proto_port == NULL)) {
        goto done;
    }

    proto_port->pair[0] = -1;
    proto_port->pair[1] = -1;
    proto_port->socket.fd = -1;

    nxt_memzero(app, sizeof(nxt_app_t));

    if (nxt_slow_path(nxt_thread_mutex_create(&app->mutex) != NXT_OK)) {
        goto done;
    }

    app_mutex = 1;

    nxt_queue_init(&app->ports);
    nxt_queue_init(&app->spare_ports);
    nxt_queue_init(&app->idle_ports);
    nxt_queue_init(&app->ack_waiting_req);

    nxt_str_set(&app->name, "proto-death-test");

    app->joint = joint;
    joint->app = app;
    joint->use_count = 1;

    /*
     * "processes: {spare: 0}" -- max_pending_processes is 1, so a single
     * stranded slot is enough to wedge the application for good.
     */
    app->max_processes = 8;
    app->max_pending_processes = 1;

    /* A live prototype, so the handler takes the plain app-start branch. */
    app->proto_port = proto_port;
    app->conf.length = 0;

    /* One process alive, so the failure path has no requests to fail. */
    app->processes = 1;

    /* What a real initiator leaves behind: the slot and the work's use. */
    app->pending_processes = 1;
    app->use_count = 2;

    nxt_router_start_app_process_handler(task, router_port, app);

    if (app->pending_processes != 1 || joint->use_count != 2
        || app->use_count != 1)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router proto death test: successful start left "
                      "pending %uD joint %uD app use %d (expected 1, 2 and 1)",
                      app->pending_processes, joint->use_count,
                      (int) app->use_count);
        goto done;
    }

    /*
     * The prototype dies with the worker it forked not yet ready.  This is
     * exactly what nxt_router_remove_pid_handler() does with the streamless
     * REMOVE_PID that reports it.
     */

    nxt_port_rpc_remove_peer(task, router_port, proto_port->pid);

    if (app->pending_processes != 0 || joint->use_count != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router proto death test: prototype death left pending "
                      "%uD joint %uD (expected 0 and 1) -- the start RPC was "
                      "not retired", app->pending_processes,
                      joint->use_count);
        goto done;
    }

    /*
     * The gate #269 wedges: with the slot back the application can ask for
     * a replacement prototype on the next request.
     */

    app->processes = 0;

    if (!nxt_router_proto_death_can_start(app)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router proto death test: app cannot start after the "
                      "prototype's death, processes %uD pending %uD",
                      app->processes, app->pending_processes);
        goto done;
    }

    /*
     * And the death is reported exactly once: a second sweep for the same
     * pid, which a REMOVE_PID for an already-forgotten process would drive,
     * must find nothing left to fail rather than release a second slot.
     */

    app->pending_processes = 1;

    nxt_port_rpc_remove_peer(task, router_port, proto_port->pid);

    if (app->pending_processes != 1 || joint->use_count != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router proto death test: repeated sweep left pending "
                      "%uD joint %uD (expected 1 and 1)",
                      app->pending_processes, joint->use_count);
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router proto death test passed");

    ret = NXT_OK;

done:

    if (proto_port != NULL) {
        /*
         * The start above was queued rather than written, and
         * nxt_port_msg_chk_insert() takes a port reference for every queued
         * message.  Left alone the port never reaches nxt_port_mp_cleanup(),
         * so its pool and messages leak and the assertions there never run
         * -- which would quietly hollow out this test under a debug build.
         */
        nxt_port_test_run_error_handler(task, proto_port);

        if (!nxt_queue_is_empty(&proto_port->messages)) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router proto death test: prototype port still "
                          "holds queued messages after teardown");
            ret = NXT_ERROR;
        }

        nxt_port_use(task, proto_port, -1);
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
