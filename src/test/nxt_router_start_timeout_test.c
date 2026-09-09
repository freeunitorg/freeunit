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
 * Commit the pending timer changes and move what is due onto the work queue,
 * the way nxt_event_engine_start() does, without running any of it:
 * nxt_timer_find() folds the change list into the rbtree and
 * nxt_timer_expire() queues the handlers that are due.  Split out from the
 * tick below so a leg can queue work *behind* an already-queued timer handler,
 * which is the ordering nxt_port_write_handler() creates for real.
 */

static void
nxt_router_start_timeout_test_expire(nxt_event_engine_t *engine,
    nxt_msec_t advance)
{
    (void) nxt_timer_find(engine);

    nxt_timer_expire(engine, engine->timers.now + advance);
}


/* Drain the work queue, returning the number of handlers that ran. */

static nxt_uint_t
nxt_router_start_timeout_test_drain(nxt_task_t *task,
    nxt_event_engine_t *engine)
{
    nxt_uint_t          ran;
    nxt_task_t          *wq_task;
    nxt_work_handler_t  handler;
    void                *obj, *data;

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


static nxt_uint_t
nxt_router_start_timeout_test_tick(nxt_task_t *task, nxt_event_engine_t *engine,
    nxt_msec_t advance)
{
    nxt_router_start_timeout_test_expire(engine, advance);

    return nxt_router_start_timeout_test_drain(task, engine);
}


/* The message a start left in a port's send queue, if it is still there. */

static nxt_port_send_msg_t *
nxt_router_start_timeout_test_msg(nxt_port_t *port)
{
    nxt_queue_link_t  *lnk;

    lnk = nxt_queue_first(&port->messages);

    if (lnk == nxt_queue_tail(&port->messages)) {
        return NULL;
    }

    return nxt_queue_link_data(lnk, nxt_port_send_msg_t, link);
}


nxt_int_t
nxt_router_start_timeout_test(nxt_thread_t *thr)
{
    nxt_mp_t                *mp;
    nxt_app_t               *app;
    nxt_int_t               ret;
    nxt_uint_t              ran;
    uint32_t                stream, joint_use;
    nxt_bool_t              app_mutex, timers;
    nxt_task_t              *task;
    nxt_port_t              *router_port, *main_port, *shared_port;
    nxt_router_t            test_router;
    nxt_runtime_t           *rt, *saved_rt;
    nxt_atomic_t            main_port_use;
    nxt_app_joint_t         *joint;
    nxt_event_engine_t      engine, *saved_engine;
    nxt_port_send_msg_t     *sent;
    nxt_router_temp_conf_t  *tmcf;

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

    /* Baseline for the reference-balance check after the deadline fires. */
    main_port_use = main_port->use_count;

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
     * And the start it gave up on is out of the send queue, with the reference
     * nxt_port_msg_chk_insert() took for it discharged.  This is the whole
     * point of resolving through nxt_port_socket_cancel(): failing the RPC
     * releases the payload and the application's shared-port descriptors, and
     * a message left behind here would be dereferenced -- and its descriptors
     * sent -- when the destination finally drained.
     */

    if (!nxt_queue_is_empty(&main_port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: an expired start left its "
                      "START_PROCESS in the destination's send queue");
        goto done;
    }

    if (main_port->use_count != main_port_use) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: destination port use count "
                      "%uD after an expired start, expected %uD",
                      (uint32_t) main_port->use_count,
                      (uint32_t) main_port_use);
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

    /*
     * The write/timer ordering inside one poll cycle.
     *
     * nxt_port_write_handler() removes a fully sent message from
     * port->messages and only then queues its buffer completion, so the
     * completion can land behind a timer handler that the same poll cycle
     * queued ahead of it.  The deadline then finds nothing in the send queue
     * while the payload is still referenced by work that has not run:
     * "not found" is not "safe to fail", and failing there would release the
     * payload under the pending completion.
     *
     * Built here by expiring the timer first -- which only queues its handler
     * -- and taking the message out afterwards, so its completion is queued
     * behind that handler exactly as a real write would leave it.
     */

    app->pending_processes = 1;
    app->use_count = 2;

    nxt_router_start_app_process_handler(task, router_port, app);

    sent = nxt_router_start_timeout_test_msg(main_port);

    if (sent == NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: no message queued for the "
                      "same-poll ordering leg");
        goto done;
    }

    stream = sent->port_msg.stream;

    nxt_router_start_timeout_test_expire(&engine, 2000);

    if (nxt_port_socket_cancel(task, main_port, NXT_PORT_MSG_START_PROCESS,
                               stream, router_port->id, NULL)
        != NXT_PORT_MSG_CANCELLED)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: could not take the queued "
                      "start back for the same-poll ordering leg");
        goto done;
    }

    if (app->proto_port_requests != 1 || app->pending_processes != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: taking the message back "
                      "failed the start by itself, proto_port_requests %uD "
                      "pending %uD (expected 1 and 1)",
                      app->proto_port_requests, app->pending_processes);
        goto done;
    }

    ran = nxt_router_start_timeout_test_drain(task, &engine);

    if (ran == 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the deadline did not fire "
                      "for a sent-but-uncompleted start");
        goto done;
    }

    if (app->proto_port_requests != 0 || app->pending_processes != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: a sent-but-uncompleted "
                      "start was not failed once its completion ran, "
                      "proto_port_requests %uD pending %uD (expected 0 and 0)",
                      app->proto_port_requests, app->pending_processes);
        goto done;
    }

    /*
     * A start whose message is only partially transmitted.
     *
     * nxt_port_write_handler() sets port_msg.nf once a fragment has gone out,
     * and clears msg->fd[] in the same breath: the peer is mid-stream and the
     * descriptors are already with it, so nxt_port_socket_cancel() must refuse
     * to take the message back.  The deadline must then leave the RPC armed
     * -- failing it would release the payload the remaining fragments point at
     * -- and fail it from the completion of the last fragment instead.  This
     * is the documented limitation of a protocol without cancellation.
     */

    app->pending_processes = 1;
    app->use_count = 2;

    nxt_router_start_app_process_handler(task, router_port, app);

    sent = nxt_router_start_timeout_test_msg(main_port);

    if (sent == NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: no message queued for the "
                      "partial-send leg");
        goto done;
    }

    sent->port_msg.nf = 1;

    ran = nxt_router_start_timeout_test_tick(task, &engine, 2000);

    if (ran == 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the deadline did not fire "
                      "for a partially sent start");
        goto done;
    }

    if (nxt_queue_is_empty(&main_port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: a partially sent message was "
                      "taken back out of the send queue");
        goto done;
    }

    if (app->proto_port_requests != 1 || app->pending_processes != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: a partially sent start was "
                      "failed while its payload was still queued, "
                      "proto_port_requests %uD pending %uD (expected 1 and 1)",
                      app->proto_port_requests, app->pending_processes);
        goto done;
    }

    /*
     * The remainder completes -- here by the destination dying, which is what
     * nxt_port_error_handler() does to every message it still holds.  The
     * failure must arrive with it and not before.
     */

    nxt_port_test_run_error_handler(task, main_port);

    (void) nxt_router_start_timeout_test_drain(task, &engine);

    if (app->proto_port_requests != 0 || app->pending_processes != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: a partially sent start was "
                      "not failed by its final completion, "
                      "proto_port_requests %uD pending %uD (expected 0 and 0)",
                      app->proto_port_requests, app->pending_processes);
        goto done;
    }

    /*
     * The other shape of START_PROCESS: a worker start, sent to a prototype
     * that is already up.  It carries no payload and no descriptors, so the
     * deadline has nothing to wait on and fails the RPC itself -- but the
     * message can still be sitting in the destination's send queue, and
     * delivered after the stream is retired it starts a worker whose
     * PROCESS_READY answers nobody, leaving a process and a port the router is
     * not tracking.  Recall it while it is still there.
     *
     * main_port stands in for the prototype: what matters is a destination
     * that is not write-ready and has no shared-memory queue, so the message
     * queues rather than going straight out.
     */

    app->proto_port = main_port;

    app->pending_processes = 1;
    app->use_count = 2;

    /* Baseline for the reaper's inherited reference, checked below. */

    joint_use = joint->use_count;

    nxt_router_start_app_process_handler(task, router_port, app);

    sent = nxt_router_start_timeout_test_msg(main_port);

    if (sent == NULL || sent->buf != NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the worker start queued no "
                      "header-only message");
        goto done;
    }

    /* Read before the recall below frees the message. */

    stream = sent->port_msg.stream;

    ran = nxt_router_start_timeout_test_tick(task, &engine, 2000);

    if (ran == 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: nothing bounded a worker "
                      "start");
        goto done;
    }

    if (!nxt_queue_is_empty(&main_port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: an expired worker start left "
                      "its START_PROCESS queued, to start a worker for a "
                      "stream that is already retired");
        goto done;
    }

    /*
     * A recalled start forked nothing, so it is accounted for as any other
     * failed attempt: the slot goes straight back.  Moving it to
     * unaccounted_processes instead would strand it -- no PROCESS_READY and
     * no REMOVE_PID are coming for a process that was never created -- and
     * that would make "processes": {"max"} a ratchet rather than a bound.
     */

    if (app->pending_processes != 0 || app->unaccounted_processes != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: a recalled worker start left "
                      "pending %uD unaccounted %uD (expected 0 and 0)",
                      app->pending_processes, app->unaccounted_processes);
        goto done;
    }

    if (joint->use_count != joint_use) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: a recalled worker start left "
                      "joint %uD (expected %uD)", joint->use_count, joint_use);
        goto done;
    }

    /*
     * The start that does leave a process behind: one whose START_PROCESS
     * reached the prototype, which forked a worker that never announced
     * itself.  Taking the message out of the send queue here is what the
     * prototype's read does in the field, so the deadline below finds nothing
     * to recall and knows the fork happened.
     *
     * The router has no pid for that worker -- a pid is what PROCESS_READY
     * carries -- so it can neither kill it nor count it in ->processes.  The
     * slot moves to unaccounted_processes, where nxt_router_app_can_start()
     * still sees it, and "processes": {"max"} keeps bounding OS children.
     */

    app->pending_processes = 1;
    app->use_count = 2;

    joint_use = joint->use_count;

    nxt_router_start_app_process_handler(task, router_port, app);

    sent = nxt_router_start_timeout_test_msg(main_port);

    if (sent == NULL || sent->buf != NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the delivered worker start "
                      "queued no header-only message");
        goto done;
    }

    stream = sent->port_msg.stream;

    if (nxt_port_socket_cancel(task, main_port, NXT_PORT_MSG_START_PROCESS,
                               stream, router_port->id, NULL)
        != NXT_PORT_MSG_CANCELLED)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: could not stand in for the "
                      "prototype's read of the worker start");
        goto done;
    }

    ran = nxt_router_start_timeout_test_tick(task, &engine, 2000);

    if (ran == 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: nothing bounded a delivered "
                      "worker start");
        goto done;
    }

    if (app->pending_processes != 0 || app->unaccounted_processes != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: an expired worker start left "
                      "pending %uD unaccounted %uD (expected 0 and 1)",
                      app->pending_processes, app->unaccounted_processes);
        goto done;
    }

    /*
     * And it is a bound rather than a leak because the expiry put a handler
     * back on the stream.  This is the REMOVE_PID the router gets when that
     * worker finally dies: the prototype which forked it reaps it and
     * notifies with the start's own stream still attached, and
     * nxt_router_remove_pid_handler() retypes that into an RPC error.  The
     * slot comes back, and so does the joint reference the reaper inherited.
     */

    nxt_port_rpc_error(task, router_port, stream);

    if (app->unaccounted_processes != 0 || joint->use_count != joint_use) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the death of the process an "
                      "expired start left behind gave back unaccounted %uD "
                      "joint %uD (expected 0 and %uD)",
                      app->unaccounted_processes, joint->use_count, joint_use);
        goto done;
    }

    app->proto_port = NULL;

    /*
     * The other shape that leaves a process behind: a prototype start whose
     * START_PROCESS reached main, which forked the prototype for it.  A
     * prototype that never announces itself forks no worker of its own, so
     * the attempt leaves exactly one process however many requests parked on
     * it -- and one slot has to account for that process, exactly as a
     * worker's does.
     *
     * Two callers park here: the initiator, and one that joined the wait.
     * The cohort stands for requests, so the joiner's slot goes straight
     * back and the initiator's is the one that stays.  Treating the whole
     * cohort as starts that never happened, which is what the router did
     * before, returned both: every later request then forked another
     * prototype that would never answer either, and "processes": {"max"}
     * bounded nothing at all.
     */

    app->pending_processes = 2;
    app->unaccounted_processes = 0;
    app->use_count = 3;

    joint_use = joint->use_count;

    nxt_router_start_app_process_handler(task, router_port, app);
    nxt_router_start_app_process_handler(task, router_port, app);

    if (app->proto_port_requests != 2) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the second caller did not "
                      "join the prototype wait, proto_port_requests %uD "
                      "(expected 2)", app->proto_port_requests);
        goto done;
    }

    sent = nxt_router_start_timeout_test_msg(main_port);

    if (sent == NULL || sent->buf == NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the prototype start queued "
                      "no payload message");
        goto done;
    }

    stream = sent->port_msg.stream;

    /*
     * Standing in for main's read.  Consuming the payload is the half that
     * matters: nxt_port_write_handler() advances the buffer as it sends, and
     * that is how the completion below tells a message the destination has
     * seen whole from one recalled or dropped with bytes still in it.
     */

    sent->buf->mem.pos = sent->buf->mem.free;

    if (nxt_port_socket_cancel(task, main_port, NXT_PORT_MSG_START_PROCESS,
                               stream, router_port->id, NULL)
        != NXT_PORT_MSG_CANCELLED)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: could not stand in for "
                      "main's read of the prototype start");
        goto done;
    }

    ran = nxt_router_start_timeout_test_tick(task, &engine, 2000);

    if (ran == 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: nothing bounded a delivered "
                      "prototype start");
        goto done;
    }

    if (app->pending_processes != 0 || app->unaccounted_processes != 1
        || app->proto_port_requests != 0)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: an expired prototype start "
                      "left pending %uD unaccounted %uD proto_port_requests "
                      "%uD (expected 0, 1 and 0)",
                      app->pending_processes, app->unaccounted_processes,
                      app->proto_port_requests);
        goto done;
    }

    /*
     * And the prototype's death gives the slot back, the same way a worker's
     * does: main leaves the start's stream on a process that never reached
     * READY, so the REMOVE_PID for it is retyped onto the reaper.
     */

    nxt_port_rpc_error(task, router_port, stream);

    if (app->unaccounted_processes != 0 || joint->use_count != joint_use) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the death of the prototype "
                      "an expired start left behind gave back unaccounted %uD "
                      "joint %uD (expected 0 and %uD)",
                      app->unaccounted_processes, joint->use_count, joint_use);
        goto done;
    }

    /*
     * A start whose write is accepted and then dropped inside the port layer.
     *
     * With the port write-ready and its queue empty, nxt_port_msg_chk_insert()
     * declines to queue and nxt_port_socket_write2() sends inline through
     * nxt_port_write_handler().  A send that fails there -- here because the
     * descriptor is -1, in the field because the peer is gone, or because
     * nxt_port_msg_insert_tail() cannot allocate after an NXT_AGAIN, which
     * happens against a perfectly live port -- drops a message that was never
     * in port->messages.  write2() still answers NXT_OK, because the handler
     * returns void.
     *
     * So the deadline finds nothing to cancel and, waiting for the payload to
     * be released, would wait for a completion nobody was going to run: the
     * RPC and its pending_processes slot armed for good, which is the very
     * wedge the option exists to bound.  nxt_port_msg_drop() completes the
     * buffers of these dropped messages, so the wait ends the way every other
     * one does.
     */

    app->pending_processes = 1;
    app->use_count = 2;

    /*
     * The inline path really does call into the socket layer, which logs
     * through the event's own task and sizes its iovec against the port's send
     * limit.  A port from nxt_port_new() has neither -- nxt_port_write_enable()
     * is what fills them in for a real one -- and the descriptor stays -1, so
     * the sendmsg() fails and the drop path is taken.
     */

    main_port->socket.task = task;
    main_port->socket.log = thr->log;
    main_port->max_size = 1024;
    main_port->socket.write_ready = 1;

    nxt_router_start_app_process_handler(task, router_port, app);

    main_port->socket.write_ready = 0;

    if (!nxt_queue_is_empty(&main_port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the dropped-write leg queued "
                      "its message instead of sending it inline");
        goto done;
    }

    /* The completion the drop queued, and the error handler it raised. */

    (void) nxt_router_start_timeout_test_drain(task, &engine);

    ran = nxt_router_start_timeout_test_tick(task, &engine, 2000);

    if (ran == 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: nothing bounded a start "
                      "whose write was dropped inside the port layer");
        goto done;
    }

    if (app->proto_port_requests != 0 || app->pending_processes != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: a start whose write was "
                      "dropped stayed armed, proto_port_requests %uD pending "
                      "%uD (expected 0 and 0)",
                      app->proto_port_requests, app->pending_processes);
        goto done;
    }

    /*
     * And the same expiry on the config-apply path, which is where the payload
     * has a lifetime of its own: nxt_router_app_rpc_create() allocates it from
     * tmcf->mem_pool, and the failure it drives ends in
     * nxt_router_conf_error(), which releases that pool.  A deadline that
     * failed the RPC with the message still queued would leave the send queue
     * pointing into freed memory -- which is what this leg would report under
     * ASan, from the teardown that drains the port below.
     */

    nxt_memzero(&test_router, sizeof(nxt_router_t));
    nxt_queue_init(&test_router.sockets);
    nxt_queue_init(&test_router.apps);

    rt->port_by_type[NXT_PROCESS_ROUTER] = router_port;

    tmcf = nxt_router_test_temp_conf(task);
    if (nxt_slow_path(tmcf == NULL)) {
        goto done;
    }

    tmcf->router_conf->router = &test_router;

    /* nxt_router_conf_send() answers on it and drops a reference for it. */
    tmcf->port = main_port;
    nxt_port_inc_use(main_port);

    nxt_router_test_app_rpc_create(task, tmcf, app);

    if (nxt_router_start_timeout_test_msg(main_port) == NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the config-apply start "
                      "queued nothing");
        goto done;
    }

    ran = nxt_router_start_timeout_test_tick(task, &engine, 2000);

    if (ran == 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: nothing bounded the "
                      "config-apply start");
        goto done;
    }

    /*
     * tmcf and its pool are gone from here on.  What must not be gone with
     * them is a queued reference to the payload they held: only the RPC_ERROR
     * nxt_router_conf_send() wrote may remain.
     */

    sent = nxt_router_start_timeout_test_msg(main_port);

    if (sent != NULL
        && sent->port_msg.type == (NXT_PORT_MSG_START_PROCESS
                                   & NXT_PORT_MSG_MASK))
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router start timeout test: the config-apply start left "
                      "its START_PROCESS queued against a released pool");
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
