/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for issue #214: nxt_router_start_app_process_handler()
 * is entered owning one app->pending_processes increment (its initiator
 * took it before posting the work), and its failure paths used to return
 * without giving it back.  The counter gates every future start
 * (nxt_router_app_can_start()), so each leak permanently costs the
 * application one start slot; with the default max_pending_processes of 1
 * a single failure wedges the application for the router's lifetime.
 *
 * The deterministic failure used here is the nxt_port_socket_write2()
 * site: the NXT_TESTS allocation-failure hook makes the port layer's
 * message allocation fail, the write returns NXT_ERROR, and the handler
 * takes "goto failed" after cancelling the RPC registration it had just
 * made.  On entry the app is set up the way a real start attempt finds
 * it: pending_processes already counted, one extra app use for the posted
 * work item.
 *
 * This single-shot test reaches that third site only.  All three failure
 * sites, and the accounting across repeated failures, are covered by
 * src/test/nxt_router_start_fail_soak_test.c.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_rpc.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


/*
 * File scope rather than a stack object: the handler hands the application to
 * the router's reference-counting helpers, so its storage must outlive the
 * frame even though this test's arrangement never lets it be posted anywhere.
 */
static nxt_app_t  nxt_router_start_fail_test_app;


nxt_int_t
nxt_router_start_fail_test(nxt_thread_t *thr)
{
    nxt_app_t   *app;
    nxt_int_t   ret;
    nxt_bool_t  app_mutex;
    nxt_task_t  *task;
    nxt_port_t  *router_port, *dport;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router start fail test started");

    ret = NXT_ERROR;
    router_port = NULL;
    dport = NULL;
    app_mutex = 0;
    app = &nxt_router_start_fail_test_app;

    task = thr->task;
    task->thread = thr;

    if (nxt_slow_path(nxt_port_rpc_init() != NXT_OK)) {
        return NXT_ERROR;
    }

    /* The port the handler registers its RPC on. */

    router_port = nxt_port_new(task, 0, nxt_pid, NXT_PROCESS_ROUTER);
    if (nxt_slow_path(router_port == NULL)) {
        return NXT_ERROR;
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
     * The destination port the START_PROCESS message is written to.  No
     * queue and write_ready unset, so nxt_port_socket_write2() takes the
     * nxt_port_msg_chk_insert() path, whose allocation the test hook
     * fails.
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

    nxt_str_set(&app->name, "start-fail-test");

    /*
     * A prototype port makes the handler take the plain app-start branch:
     * no buffer allocation, straight to the RPC registration and the
     * write.
     */
    app->proto_port = dport;

    /* What a real initiator leaves behind: the slot and the work's use. */
    app->pending_processes = 1;
    app->use_count = 2;

    /* One process alive, so the failure path has no requests to fail. */
    app->processes = 1;

    nxt_port_test_msg_alloc_failures(1);

    nxt_router_start_app_process_handler(task, router_port, app);

    nxt_port_test_msg_alloc_failures(0);

    if (app->pending_processes != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start fail test: pending_processes %uD leaked "
                      "(expected 0)", app->pending_processes);
        goto done;
    }

    if (app->use_count != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start fail test: app use_count %d (expected 1)",
                      (int) app->use_count);
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router start fail test passed");

    ret = NXT_OK;

done:

    if (dport != NULL) {
        nxt_port_use(task, dport, -1);
    }

    if (router_port != NULL) {
        router_port->pair[0] = -1;

        nxt_port_use(task, router_port, -1);
    }

    if (app_mutex) {
        nxt_thread_mutex_destroy(&app->mutex);
    }

    return ret;
}
