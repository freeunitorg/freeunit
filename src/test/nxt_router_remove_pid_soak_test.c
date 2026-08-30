/*
 * Copyright (C) F5, Inc.
 */

/*
 * Accounting soak for the route a refused worker start really travels
 * (issue #231).  When a worker never becomes usable, nothing cancels its
 * start RPC directly: the prototype reaps the child and sends REMOVE_PID
 * carrying the stream the start was registered under, and the router turns
 * that into the start failure.
 *
 * The chain this exercises, in the router's own order:
 *
 *   nxt_router_remove_pid_handler()  retypes a last-marked REMOVE_PID that
 *                                    carries a non-zero stream into
 *                                    _NXT_PORT_MSG_RPC_ERROR
 *     -> nxt_port_rpc_handler()      finds and deletes the registration
 *     -> nxt_router_app_port_error() the RPC is a worker start, so ->proto
 *                                    is 0 and the count is 1
 *     -> nxt_router_app_start_failed()  gives that one slot back
 *
 * The handler is private to src/nxt_router.c; it is reached the way the
 * port dispatcher reaches it, through the router's published handler table
 * in nxt_router_process.
 *
 * Every iteration arms a real start through
 * nxt_router_start_app_process_handler(), takes the stream from the
 * START_PROCESS message that start actually queued for the prototype --
 * the very stream a prototype would echo back -- and requires
 * pending_processes at its baseline afterwards.  A leak leaves baseline+1
 * and a double release leaves baseline-1; both are caught without
 * nxt_assert, so the check survives a non-debug build.
 *
 * The baselines alternate between 0 and a non-zero value because an
 * over-release at baseline 0 is the case that hides: nxt_router.c clamps
 * the count to pending_processes rather than letting it wrap, so the
 * counter simply stays at 0 and an equality check against 0 cannot tell
 * the difference.  At a non-zero baseline the same bug shows up plainly.
 *
 * The counter app->proto_port_requests is required to stay 0 throughout.
 * A worker start cannot touch it -- ->proto is set from the same condition
 * that increments it, and this route never arms a prototype -- so the check
 * is a guard against the fixture drifting into the prototype path, not a
 * failure this route can produce.
 *
 * A stream nothing registered is delivered too, so the release above
 * cannot be credited to anything other than the RPC registration.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_rpc.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_main_process.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


#define NXT_ROUTER_REMOVE_PID_SOAK_N  1000


/* File scope: the handler hands the application to the router's
   reference-counting helpers, so its storage must outlive the frame. */
static nxt_app_t  nxt_router_remove_pid_soak_test_app;


/*
 * The stream a start put on the wire.  nxt_port_socket_write2() queues the
 * START_PROCESS message at the tail of the destination port, so the newest
 * one is this iteration's; returns 0, which the caller rejects, if the
 * write did not queue anything.
 */

static uint32_t
nxt_router_remove_pid_soak_stream(nxt_port_t *dport)
{
    nxt_queue_link_t     *link;
    nxt_port_send_msg_t  *msg;

    if (nxt_slow_path(nxt_queue_is_empty(&dport->messages))) {
        return 0;
    }

    link = nxt_queue_last(&dport->messages);
    msg = nxt_queue_link_data(link, nxt_port_send_msg_t, link);

    return msg->port_msg.stream;
}


/*
 * A REMOVE_PID as the prototype sends it: the reaped worker's pid as the
 * whole payload, the start stream, and last set so the registration is
 * deleted rather than merely looked up.  Modelled on the message
 * nxt_port_rpc_remove_peer() synthesises for the same purpose.
 */

static void
nxt_router_remove_pid_soak_msg(nxt_port_recv_msg_t *msg, nxt_buf_t *buf,
    nxt_port_t *port, nxt_pid_t *pid, uint32_t stream)
{
    nxt_memzero(msg, sizeof(nxt_port_recv_msg_t));
    nxt_memzero(buf, sizeof(nxt_buf_t));

    buf->mem.pos = (u_char *) pid;
    buf->mem.free = (u_char *) (pid + 1);

    msg->fd[0] = -1;
    msg->fd[1] = -1;
    msg->buf = buf;
    msg->port = port;

    msg->port_msg.stream = stream;
    msg->port_msg.pid = nxt_pid;
    msg->port_msg.type = _NXT_PORT_MSG_REMOVE_PID;
    msg->port_msg.last = 1;
}


nxt_int_t
nxt_router_remove_pid_soak_test(nxt_thread_t *thr)
{
    nxt_mp_t             *mp;
    nxt_app_t            *app;
    nxt_buf_t            buf;
    nxt_pid_t            worker_pid;
    nxt_int_t            ret;
    uint32_t             baseline, stream;
    nxt_uint_t           i;
    nxt_bool_t           app_mutex;
    nxt_task_t           *task;
    nxt_port_t           *router_port, *dport;
    nxt_router_t         router, *saved_router;
    nxt_runtime_t        *rt, *saved_rt;
    nxt_app_joint_t      *joint;
    nxt_event_engine_t   engine, *saved_engine;
    nxt_port_recv_msg_t  msg;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router remove pid soak test started");

    ret = NXT_ERROR;
    router_port = NULL;
    dport = NULL;
    joint = NULL;
    app_mutex = 0;
    app = &nxt_router_remove_pid_soak_test_app;

    /*
     * No process by this pid is registered, so nxt_port_remove_pid() finds
     * nothing to tear down and the message carries the stream alone -- the
     * part of REMOVE_PID this test is about.
     */
    worker_pid = nxt_pid + 1;

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

    /*
     * nxt_router_remove_pid_handler() forwards the removal to every router
     * engine before it reaches the RPC, and dereferences the nxt_router
     * global to find them.  An empty list is the honest arrangement here:
     * this fixture runs no worker engines, and what the forwarding does --
     * dropping the dead process from each engine's app port hash -- is not
     * part of the pending_processes accounting under test.
     */
    nxt_memzero(&router, sizeof(nxt_router_t));
    nxt_queue_init(&router.engines);

    saved_router = nxt_router;
    saved_rt = thr->runtime;
    saved_engine = thr->engine;
    nxt_router = &router;
    thr->runtime = rt;
    thr->engine = &engine;
    rt->main_engine = &engine;

    /* The port the handler registers its RPC on, and REMOVE_PID arrives on. */

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
     * The prototype the start addresses.  No queue and write_ready unset,
     * so nxt_port_socket_write2() takes the nxt_port_msg_chk_insert() path
     * and the START_PROCESS message is simply queued: the write succeeds,
     * the RPC stays armed, and the queued message is where the stream is
     * read back from.
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

    nxt_str_set(&app->name, "remove-pid-soak-test");

    app->joint = joint;
    joint->app = app;
    joint->use_count = 1;

    app->max_processes = 8;
    app->max_pending_processes = 4;

    /* A prototype port is present, so every start is a worker start. */
    app->proto_port = dport;

    /* One process alive, so the failure path has no requests to fail. */
    app->processes = 1;

    /* Set once: a worker start must leave this alone.  See the file head. */
    app->proto_port_requests = 0;

    for (i = 0; i < NXT_ROUTER_REMOVE_PID_SOAK_N; i++) {
        baseline = (i % 2) ? 2 : 0;

        /* What a real initiator leaves behind: the slot and the work's use. */
        app->pending_processes = baseline + 1;
        app->use_count = 2;

        nxt_router_start_app_process_handler(task, router_port, app);

        if (app->pending_processes != baseline + 1 || joint->use_count != 2
            || app->use_count != 1)
        {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router remove pid soak test: iteration %ui armed "
                          "start left pending %uD joint %uD app use %d "
                          "(expected %uD, 2 and 1)",
                          i, app->pending_processes, joint->use_count,
                          (int) app->use_count, baseline + 1);
            goto done;
        }

        stream = nxt_router_remove_pid_soak_stream(dport);

        if (nxt_slow_path(stream == 0)) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router remove pid soak test: iteration %ui start "
                          "queued no START_PROCESS message to read a stream "
                          "from", i);
            goto done;
        }

        /*
         * A stream no start registered: the handler must find no
         * registration and change nothing.  Without this the release below
         * could be credited to any bookkeeping REMOVE_PID happens to do.
         */

        nxt_router_remove_pid_soak_msg(&msg, &buf, router_port, &worker_pid,
                                       stream + 1);

        nxt_router_process.port_handlers->remove_pid(task, &msg);

        if (app->pending_processes != baseline + 1 || joint->use_count != 2) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router remove pid soak test: iteration %ui "
                          "unregistered stream released pending %uD joint %uD "
                          "(expected %uD and 2)",
                          i, app->pending_processes, joint->use_count,
                          baseline + 1);
            goto done;
        }

        /* The real one: the worker died before it ever reported ready. */

        nxt_router_remove_pid_soak_msg(&msg, &buf, router_port, &worker_pid,
                                       stream);

        nxt_router_process.port_handlers->remove_pid(task, &msg);

        if (msg.port_msg.type != _NXT_PORT_MSG_RPC_ERROR
            || msg.u.removed_pid != worker_pid)
        {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router remove pid soak test: iteration %ui "
                          "REMOVE_PID was not turned into an RPC error for "
                          "pid %PI, type %d pid %PI",
                          i, worker_pid, (int) msg.port_msg.type,
                          msg.u.removed_pid);
            goto done;
        }

        if (app->pending_processes != baseline) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router remove pid soak test: iteration %ui "
                          "pending_processes %uD (expected %uD)",
                          i, app->pending_processes, baseline);
            goto done;
        }

        if (app->proto_port_requests != 0) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router remove pid soak test: iteration %ui worker "
                          "start touched proto_port_requests %uD (expected 0)",
                          i, app->proto_port_requests);
            goto done;
        }

        if (joint->use_count != 1 || app->use_count != 1) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router remove pid soak test: iteration %ui joint "
                          "use_count %uD app use_count %d (expected 1 and 1)",
                          i, joint->use_count, (int) app->use_count);
            goto done;
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router remove pid soak test passed: %d starts failed "
                  "through REMOVE_PID", NXT_ROUTER_REMOVE_PID_SOAK_N);

    ret = NXT_OK;

done:

    if (dport != NULL) {
        app->proto_port = NULL;

        /*
         * Every start queued its START_PROCESS message rather than writing
         * it, and nxt_port_msg_chk_insert() takes a port reference for each.
         * Left alone the port never reaches nxt_port_mp_cleanup(), so its
         * pool and messages leak and the assertions there never run -- which
         * would quietly hollow out this test under a debug build.
         */
        nxt_port_test_run_error_handler(task, dport);

        if (!nxt_queue_is_empty(&dport->messages)) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "router remove pid soak test: prototype port still "
                          "holds queued messages after teardown");
            ret = NXT_ERROR;
        }

        nxt_port_use(task, dport, -1);
    }

    if (router_port != NULL) {
        router_port->pair[0] = -1;

        nxt_port_use(task, router_port, -1);
    }

    thr->engine = saved_engine;
    thr->runtime = saved_rt;
    nxt_router = saved_router;

    if (app_mutex) {
        nxt_thread_mutex_destroy(&app->mutex);
    }

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    return ret;
}
