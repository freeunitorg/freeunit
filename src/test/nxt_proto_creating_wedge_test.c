/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for the CREATING-state start wedge: an application worker
 * that died between fork() and PROCESS_CREATED was never reported to anyone,
 * so the START_PROCESS RPC it was forked for stayed armed forever and the
 * record the main process had already made for it was never retired.
 *
 * Every child of the prototype exists to satisfy a START_PROCESS request, and
 * the router arms an RPC handler for each one.  A child that got as far as
 * PROCESS_CREATED is reported by REMOVE_PID, which carries ->stream, and
 * nxt_router_remove_pid_handler() turns a stream-bearing REMOVE_PID into an
 * RPC error.  A child still in the CREATING state took neither path:
 * nxt_proto_sigchld_handler() skipped the notification for it -- rightly,
 * since REMOVE_PID says nothing but a pid and under pid isolation the
 * prototype does not yet know a globally valid one -- and nothing else
 * answered instead.
 *
 * On the on-demand start path the router registers that RPC with no peer
 * (nxt_router_start_app_process_handler() never calls
 * nxt_port_rpc_ex_set_peer()), so nxt_port_rpc_remove_peer() could not recover
 * it either.  The armed handler owned an app->pending_processes slot, and a
 * leaked slot makes nxt_router_app_can_start() false for good once the
 * application's max is reached: the application never starts another process,
 * and a reload recovers only if its own config text changed.
 *
 * Answering the initiator is not the whole job.  A worker that got as far as
 * WHOAMI made main create a process record and a port holding main's end of
 * the worker's port socket (nxt_main_process_whoami_handler()), and REMOVE_PID
 * is the only thing that retires it -- nxt_proc_remove_notify_matrix pairs a
 * dying APP with MAIN.  Once the start RPC is retired and requests retry, a
 * worker that keeps dying in that window costs main one record and one
 * descriptor per attempt.  So the CREATING case must both answer the initiator
 * and send a streamless REMOVE_PID: streamless because the initiator has
 * already been answered, and a stream-bearing one would fail the same RPC
 * twice.
 *
 * That REMOVE_PID may only be sent when the pid in it is a usable global key,
 * which is what rt->is_pid_isolated says -- the same test nxt_process_create()
 * uses to decide whether a forked child may enter the runtime hash at all.
 * The pid-isolated case is exercised here too, and it asserts the opposite:
 * nothing at all must reach main, because sending a namespace-local pid would
 * ask main to remove whatever unrelated process holds that number.
 *
 * All three states are driven against one router port and one main port,
 * because the point is the split between them.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_application.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


#define NXT_PROTO_WEDGE_CREATING_STREAM  0x0BADF00D
#define NXT_PROTO_WEDGE_ISOLATED_STREAM  0x0BADF00E
#define NXT_PROTO_WEDGE_CREATED_STREAM   0x0C0FFEE1


typedef struct {
    nxt_uint_t  type;
    uint32_t    stream;
    nxt_pid_t   pid;      /* REMOVE_PID payload; 0 for a message without one */
} nxt_proto_wedge_msg_t;


/*
 * The ports are never write_ready here, so nxt_port_msg_chk_insert() queues
 * whatever the handler writes instead of sending it -- which is exactly what
 * this test wants to read back.
 */

static nxt_int_t
nxt_proto_wedge_expect(nxt_thread_t *thr, nxt_port_t *port,
    const nxt_proto_wedge_msg_t *expect, nxt_uint_t n, const char *label,
    const char *whom)
{
    nxt_pid_t                    pid;
    nxt_uint_t                   i;
    nxt_queue_link_t             *link;
    nxt_port_send_msg_t          *msg;
    const nxt_proto_wedge_msg_t  *e;

    link = nxt_queue_first(&port->messages);

    for (i = 0; i < n; i++) {
        e = &expect[i];

        if (link == nxt_queue_tail(&port->messages)) {
            nxt_log_alert(thr->log, "proto creating wedge test: %s: nothing "
                          "more was written to the %s, expected message %d of "
                          "%d (type %d stream %uxD)", label, whom, (int) i + 1,
                          (int) n, (int) e->type, e->stream);
            return NXT_ERROR;
        }

        msg = nxt_queue_link_data(link, nxt_port_send_msg_t, link);

        if (msg->port_msg.type != e->type || msg->port_msg.stream != e->stream)
        {
            nxt_log_alert(thr->log, "proto creating wedge test: %s: the %s got "
                          "message %d of type %d stream %uxD (expected type %d "
                          "stream %uxD)", label, whom, (int) i + 1,
                          (int) msg->port_msg.type, msg->port_msg.stream,
                          (int) e->type, e->stream);
            return NXT_ERROR;
        }

        if (e->pid != 0) {
            if (msg->buf == NULL
                || nxt_buf_used_size(msg->buf) != sizeof(nxt_pid_t))
            {
                nxt_log_alert(thr->log, "proto creating wedge test: %s: the %s "
                              "got a REMOVE_PID carrying no pid", label, whom);
                return NXT_ERROR;
            }

            nxt_memcpy(&pid, msg->buf->mem.pos, sizeof(nxt_pid_t));

            if (pid != e->pid) {
                nxt_log_alert(thr->log, "proto creating wedge test: %s: the %s "
                              "was told to remove pid %PI (expected %PI)",
                              label, whom, pid, e->pid);
                return NXT_ERROR;
            }
        }

        link = nxt_queue_next(link);
    }

    if (link != nxt_queue_tail(&port->messages)) {
        msg = nxt_queue_link_data(link, nxt_port_send_msg_t, link);

        nxt_log_alert(thr->log, "proto creating wedge test: %s: the %s got an "
                      "extra message of type %d stream %uxD after the %d "
                      "expected", label, whom, (int) msg->port_msg.type,
                      msg->port_msg.stream, (int) n);
        return NXT_ERROR;
    }

    return NXT_OK;
}


static void
nxt_proto_wedge_drain(nxt_task_t *task, nxt_port_t *port)
{
    /*
     * Queued messages hold a port reference each; nxt_port_test_run_error
     * _handler() is how the other port tests give them back, and it is what
     * lets the pool cleanup assertions run under a debug build.
     */
    nxt_port_test_run_error_handler(task, port);
}


nxt_int_t
nxt_proto_creating_wedge_test(nxt_thread_t *thr)
{
    nxt_mp_t               *mp;
    nxt_int_t              ret;
    nxt_task_t             *task;
    nxt_port_t             *router_port, *main_port, *app_port;
    nxt_runtime_t          *rt, *saved_rt;
    nxt_process_t          *process;
    nxt_event_engine_t     engine, *saved_engine;
    nxt_proto_wedge_msg_t  expect[2];

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "proto creating wedge test started");

    ret = NXT_ERROR;
    app_port = NULL;
    main_port = NULL;
    router_port = NULL;

    task = thr->task;
    task->thread = thr;

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

    if (nxt_slow_path(nxt_thread_mutex_create(&rt->processes_mutex) != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    /*
     * nxt_port_remove_notify_others() allocates its payload from
     * task->thread->engine->mem_pool; only the members the reached code
     * touches are set, as in src/test/nxt_router_new_port_test.c.
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

    /*
     * The port the start request came from.  Its pid must differ from
     * nxt_pid: nxt_port_remove_notify_others() skips the sender's own
     * process, so a router sharing this process's pid would make the
     * CREATED case pass for the wrong reason.
     */

    router_port = nxt_runtime_process_port_create(task, rt, nxt_pid + 1, 0,
                                                  NXT_PROCESS_ROUTER);
    if (nxt_slow_path(router_port == NULL)) {
        goto done;
    }

    router_port->pair[0] = -1;
    router_port->pair[1] = -1;
    router_port->socket.fd = -1;

    /*
     * The process that answered the worker's WHOAMI and is holding a record
     * and a descriptor for it.  nxt_proc_remove_notify_matrix pairs a dying
     * APP with MAIN, so this is the port the streamless REMOVE_PID has to
     * reach.
     */

    main_port = nxt_runtime_process_port_create(task, rt, nxt_pid + 3, 0,
                                                NXT_PROCESS_MAIN);
    if (nxt_slow_path(main_port == NULL)) {
        goto done;
    }

    main_port->pair[0] = -1;
    main_port->pair[1] = -1;
    main_port->socket.fd = -1;

    /* The child being reaped.  Deliberately not in rt->processes: a
       CREATING child under pid isolation is kept out of the global pid hash,
       and the CREATED case does not need to find itself there either. */

    process = nxt_mp_zalloc(mp, sizeof(nxt_process_t));
    if (nxt_slow_path(process == NULL)) {
        goto done;
    }

    process->pid = nxt_pid + 2;
    process->isolated_pid = nxt_pid + 2;
    nxt_queue_init(&process->ports);
    nxt_queue_init(&process->children);

    /* nxt_process_type() reads the first port, and the notify matrix pairs
       NXT_PROCESS_APP with NXT_PROCESS_ROUTER and NXT_PROCESS_MAIN. */

    app_port = nxt_port_new(task, 0, process->pid, NXT_PROCESS_APP);
    if (nxt_slow_path(app_port == NULL)) {
        goto done;
    }

    app_port->pair[0] = -1;
    app_port->pair[1] = -1;
    app_port->socket.fd = -1;

    nxt_queue_insert_tail(&process->ports, &app_port->link);

    /*
     * A child that died before PROCESS_CREATED, with the prototype in main's
     * pid namespace.  The old code left this silent, so the initiator's RPC
     * stayed armed for the router's lifetime and main kept the record and the
     * descriptor it made at WHOAMI time until the prototype exited.
     */

    process->state = NXT_PROCESS_STATE_CREATING;
    process->stream = NXT_PROTO_WEDGE_CREATING_STREAM;
    process->stream_pid = router_port->pid;
    process->stream_port = router_port->id;

    nxt_proto_child_exited(task, process);

    /*
     * The initiator is answered directly, and only then told to drop the pid
     * -- streamless, or nxt_router_remove_pid_handler() would fail the same
     * RPC a second time.
     */

    expect[0].type = _NXT_PORT_MSG_RPC_ERROR;
    expect[0].stream = NXT_PROTO_WEDGE_CREATING_STREAM;
    expect[0].pid = 0;
    expect[1].type = _NXT_PORT_MSG_REMOVE_PID;
    expect[1].stream = 0;
    expect[1].pid = process->pid;

    ret = nxt_proto_wedge_expect(thr, router_port, expect, 2,
                                 "a child that died while creating",
                                 "start initiator");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_proto_wedge_expect(thr, main_port, &expect[1], 1,
                                 "a child that died while creating",
                                 "main process");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * One answer per start request.  Stream identifiers come from a shared
     * counter and are reused, so a second error for a stream already retired
     * could land on an unrelated RPC.
     */

    if (nxt_slow_path(process->stream != 0)) {
        nxt_log_alert(thr->log, "proto creating wedge test: the answered "
                      "start request is still armed on the process");
        ret = NXT_ERROR;
        goto done;
    }

    nxt_proto_wedge_drain(task, router_port);
    nxt_proto_wedge_drain(task, main_port);

    /*
     * The same child under pid isolation.  ->pid is then the namespace-local
     * pid fork() returned, and main keyed its record on the global pid it
     * read from SCM_CREDENTIALS, so there is no pid to send: the initiator is
     * still answered, and nothing at all may reach main.  Sending the
     * namespace-local pid would ask main to remove whatever unrelated process
     * happens to hold that number, which is worse than the leak this closes.
     */

    rt->is_pid_isolated = 1;

    process->state = NXT_PROCESS_STATE_CREATING;
    process->stream = NXT_PROTO_WEDGE_ISOLATED_STREAM;

    nxt_proto_child_exited(task, process);

    expect[0].type = _NXT_PORT_MSG_RPC_ERROR;
    expect[0].stream = NXT_PROTO_WEDGE_ISOLATED_STREAM;
    expect[0].pid = 0;

    ret = nxt_proto_wedge_expect(thr, router_port, expect, 1,
                                 "a pid-isolated child that died while "
                                 "creating", "start initiator");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_proto_wedge_expect(thr, main_port, expect, 0,
                                 "a pid-isolated child that died while "
                                 "creating", "main process");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    rt->is_pid_isolated = 0;

    nxt_proto_wedge_drain(task, router_port);
    nxt_proto_wedge_drain(task, main_port);

    /*
     * A child that reached PROCESS_CREATED keeps the path it always had:
     * REMOVE_PID carrying the stream, which the router turns into an RPC
     * error of its own, and which retires main's record as it always did.
     */

    process->state = NXT_PROCESS_STATE_CREATED;
    process->stream = NXT_PROTO_WEDGE_CREATED_STREAM;

    nxt_proto_child_exited(task, process);

    expect[0].type = _NXT_PORT_MSG_REMOVE_PID;
    expect[0].stream = NXT_PROTO_WEDGE_CREATED_STREAM;
    expect[0].pid = process->pid;

    ret = nxt_proto_wedge_expect(thr, router_port, expect, 1,
                                 "a child that died after it was created",
                                 "start initiator");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_proto_wedge_expect(thr, main_port, expect, 1,
                                 "a child that died after it was created",
                                 "main process");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    nxt_proto_wedge_drain(task, router_port);
    nxt_proto_wedge_drain(task, main_port);

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "proto creating wedge test passed");

    ret = NXT_OK;

done:

    if (router_port != NULL) {
        nxt_proto_wedge_drain(task, router_port);

        if (!nxt_queue_is_empty(&router_port->messages)) {
            nxt_log_alert(thr->log, "proto creating wedge test: the router "
                          "port still holds queued messages after teardown");
            ret = NXT_ERROR;
        }
    }

    if (main_port != NULL) {
        nxt_proto_wedge_drain(task, main_port);

        if (!nxt_queue_is_empty(&main_port->messages)) {
            nxt_log_alert(thr->log, "proto creating wedge test: the main "
                          "port still holds queued messages after teardown");
            ret = NXT_ERROR;
        }
    }

    if (app_port != NULL) {
        nxt_queue_remove(&app_port->link);

        /*
         * nxt_queue_remove() nulls the link only in a debug build
         * (src/nxt_queue.h:130-148).  nxt_port_release() reads a non-NULL
         * link.next as "this port belongs to a process" and dereferences
         * port->process, which nxt_port_new() never set -- guarded by an
         * nxt_assert() that compiles out of a release build, so the test
         * would segfault in teardown there and nowhere else.
         */
        app_port->link.next = NULL;
        app_port->link.prev = NULL;

        nxt_port_use(task, app_port, -1);
    }

    thr->engine = saved_engine;
    thr->runtime = saved_rt;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    return ret;
}
