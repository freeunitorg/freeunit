/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test: a worker start sent to a prototype that has already
 * exited must not be logged as an alert.
 *
 * On shutdown main sends QUIT to every process directly.  The prototype
 * reports the exit of its workers to the router, then exits itself.  The
 * router can handle such a REMOVE_PID before its own QUIT, start a
 * replacement worker from nxt_router_app_port_close(), and write
 * START_PROCESS to the prototype's socket after the prototype closed it.
 * sendmsg() then fails with EPIPE (ECONNREFUSED on a SOCK_DGRAM pair), and
 * nxt_router_start_app_process_handler() logged
 * "app '...' failed to start a process" as an alert.  The test suite fails
 * on that alert (test_app_lifecycle_process_churn in the sanitize job).
 *
 * The first leg sends to a real socket pair whose reader end is closed, and
 * checks that the slot is released and that the alert is not logged.  The
 * second leg makes the send fail with an error that does not mean "peer is
 * gone", and checks that the alert is still logged there.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_rpc.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


static void nxt_cdecl nxt_router_start_proto_gone_test_log_handler(
    nxt_uint_t level, nxt_log_t *log, const char *fmt, ...);
static nxt_int_t nxt_router_start_proto_gone_test_run(nxt_thread_t *thr,
    nxt_err_t inject, nxt_uint_t expected_alerts);
static void nxt_router_start_proto_gone_test_drain_wq(nxt_work_queue_t *wq);


/* File scope: the handler hands the application to the router's
   reference-counting helpers, so its storage must outlive the frame. */
static nxt_app_t   nxt_router_start_proto_gone_test_app;

static nxt_uint_t  nxt_router_start_proto_gone_test_alerts;


nxt_int_t
nxt_router_start_proto_gone_test(nxt_thread_t *thr)
{
    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router start proto gone test started");

    /* The prototype has closed its socket: no alert. */

    if (nxt_router_start_proto_gone_test_run(thr, 0, 0) != NXT_OK) {
        return NXT_ERROR;
    }

    /* Any other send failure is still an alert. */

    if (nxt_router_start_proto_gone_test_run(thr, NXT_EINVAL, 1) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router start proto gone test passed");

    return NXT_OK;
}


/*
 * Counts the "failed to start a process" alerts, and passes every line on to
 * the main log so the test output stays readable.
 */

static void nxt_cdecl
nxt_router_start_proto_gone_test_log_handler(nxt_uint_t level, nxt_log_t *log,
    const char *fmt, ...)
{
    u_char   *p;
    va_list  args;
    u_char   msg[NXT_MAX_ERROR_STR];

    va_start(args, fmt);
    p = nxt_vsprintf(msg, msg + NXT_MAX_ERROR_STR - 1, fmt, args);
    va_end(args);

    *p = '\0';

    if (level == NXT_LOG_ALERT
        && nxt_memstrn(msg, p, "failed to start a process",
                       nxt_length("failed to start a process")) != NULL)
    {
        nxt_router_start_proto_gone_test_alerts++;
    }

    nxt_log_error(level, &nxt_main_log, "%s", msg);
}


static nxt_int_t
nxt_router_start_proto_gone_test_run(nxt_thread_t *thr, nxt_err_t inject,
    nxt_uint_t expected_alerts)
{
    nxt_fd_t            pair[2];
    nxt_app_t           *app;
    nxt_int_t           ret;
    nxt_log_t           *saved_log;
    nxt_bool_t          app_mutex;
    nxt_task_t          *task;
    nxt_port_t          *router_port, *proto_port;
    nxt_event_engine_t  engine, *saved_engine;

    nxt_log_t  log = {
        NXT_LOG_INFO,
        0,
        nxt_router_start_proto_gone_test_log_handler,
        NULL,
        NULL
    };

    ret = NXT_ERROR;
    router_port = NULL;
    proto_port = NULL;
    app_mutex = 0;
    pair[0] = -1;
    pair[1] = -1;
    app = &nxt_router_start_proto_gone_test_app;

    task = thr->task;
    task->thread = thr;

    if (nxt_slow_path(nxt_port_rpc_init() != NXT_OK)) {
        return NXT_ERROR;
    }

    /*
     * The failed send queues nxt_port_error_handler() on the engine's fast
     * work queue; it drops the reference nxt_port_write_msgs() takes for it.
     */
    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    saved_engine = thr->engine;
    thr->engine = &engine;

    /* The port the handler registers its RPC on. */

    router_port = nxt_port_new(task, 0, nxt_pid, NXT_PROCESS_ROUTER);
    if (nxt_slow_path(router_port == NULL)) {
        goto done;
    }

    /* See src/test/nxt_router_start_fail_test.c. */
    router_port->pair[0] = 0;
    router_port->pair[1] = -1;
    router_port->socket.fd = -1;

    /*
     * The prototype port.  It is write-ready with an empty queue, so
     * nxt_port_socket_write2() sends inline and reports the failure to the
     * handler.
     */

    proto_port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_PROTOTYPE);
    if (nxt_slow_path(proto_port == NULL)) {
        goto done;
    }

    proto_port->pair[0] = -1;
    proto_port->pair[1] = -1;
    proto_port->socket.fd = -1;

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start proto gone test: socketpair failed");
        goto done;
    }

    if (inject == 0) {
        /* The prototype has exited: its reader end is closed. */
        nxt_fd_close(pair[0]);
        pair[0] = -1;
    }

    proto_port->socket.fd = pair[1];
    proto_port->socket.task = task;
    proto_port->socket.log = thr->log;
    proto_port->socket.write_ready = 1;
    proto_port->socket.write = NXT_EVENT_INACTIVE;
    proto_port->max_size = 1024;
    proto_port->max_share = 1024;

    nxt_memzero(app, sizeof(nxt_app_t));

    if (nxt_slow_path(nxt_thread_mutex_create(&app->mutex) != NXT_OK)) {
        goto done;
    }

    app_mutex = 1;

    nxt_queue_init(&app->ports);
    nxt_queue_init(&app->spare_ports);
    nxt_queue_init(&app->idle_ports);
    nxt_queue_init(&app->ack_waiting_req);

    nxt_str_set(&app->name, "start-proto-gone-test");

    /* A prototype port makes the handler take the plain app-start branch. */
    app->proto_port = proto_port;

    /* What a real initiator leaves behind: the slot and the work's use. */
    app->pending_processes = 1;
    app->use_count = 2;

    /* One process alive, so the failure path has no requests to fail. */
    app->processes = 1;

    nxt_router_start_proto_gone_test_alerts = 0;

    saved_log = task->log;
    task->log = &log;

    if (inject != 0) {
        nxt_socketpair_test_send_fail(inject, 1);
    }

    nxt_router_start_app_process_handler(task, router_port, app);

    nxt_socketpair_test_send_fail(0, 0);

    task->log = saved_log;

    nxt_router_start_proto_gone_test_drain_wq(&engine.fast_work_queue);

    if (proto_port->socket.error == 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start proto gone test: the START_PROCESS send "
                      "did not fail (inject %d)", (int) inject);
        goto done;
    }

    if (app->pending_processes != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start proto gone test: pending_processes %uD "
                      "leaked (expected 0, inject %d)",
                      app->pending_processes, (int) inject);
        goto done;
    }

    if (app->use_count != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start proto gone test: app use_count %d "
                      "(expected 1, inject %d)",
                      (int) app->use_count, (int) inject);
        goto done;
    }

    if (nxt_router_start_proto_gone_test_alerts != expected_alerts) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start proto gone test: %ui \"failed to start a "
                      "process\" alerts (expected %ui, send error %d)",
                      nxt_router_start_proto_gone_test_alerts,
                      expected_alerts, (int) proto_port->socket.error);
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_router_start_proto_gone_test_drain_wq(&engine.fast_work_queue);

    if (proto_port != NULL) {
        proto_port->socket.fd = -1;

        nxt_port_use(task, proto_port, -1);
    }

    if (pair[0] != -1) {
        nxt_fd_close(pair[0]);
    }

    if (pair[1] != -1) {
        nxt_fd_close(pair[1]);
    }

    if (router_port != NULL) {
        router_port->pair[0] = -1;

        nxt_port_use(task, router_port, -1);
    }

    thr->engine = saved_engine;

    if (app_mutex) {
        nxt_thread_mutex_destroy(&app->mutex);
    }

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);

    return ret;
}


static void
nxt_router_start_proto_gone_test_drain_wq(nxt_work_queue_t *wq)
{
    void                *obj, *data;
    nxt_task_t          *t;
    nxt_work_handler_t  handler;

    while (wq->head != NULL) {
        handler = nxt_work_queue_pop(wq, &t, &obj, &data);
        handler(t, obj, data);
    }
}
