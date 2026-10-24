/*
 * Copyright (C) FreeUnit Community
 */

/*
 * NEW_PORT and REMOVE_PID on the main port go through nxt_main_gates[].
 * Main accepts them only from a registered prototype, and only for a child
 * of that prototype.  NEW_PORT must also be for an application port that
 * main has already.
 *
 * The messages go through the handler table of the main port, so the test
 * covers the table and not only the gate.  Each refused message must change
 * no record, must close the descriptors it carries and must count as
 * refused.  Each accepted message must do what it did before the gate.
 *
 * nxt_port_new_port_msg() must refuse a process type out of range, because
 * the receiver uses the type as an index.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_queue.h>
#include <nxt_port_memory.h>
#include <nxt_runtime.h>
#include <nxt_main_process.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


/* A port id that main does not have for any process of the fixture. */
#define NXT_MAIN_GATE_PORT_ID  5

/* The sizes of a whole NEW_PORT body and of a whole REMOVE_PID body. */
#define NXT_MAIN_GATE_NP       sizeof(nxt_port_msg_new_port_t)
#define NXT_MAIN_GATE_PID      sizeof(nxt_pid_t)


typedef struct {
    nxt_pid_t           sender;
    nxt_pid_t           pid;
    nxt_port_id_t       id;
    nxt_process_type_t  type;
    size_t              size;
    const char          *label;
} nxt_main_gate_case_t;


typedef struct {
    nxt_thread_t        *thr;
    nxt_task_t          *task;
    nxt_mp_t            *mp;
    nxt_runtime_t       *rt;
    nxt_port_t          *main_port;
    nxt_pid_t           pids[6];
    nxt_process_t       *records[6];
} nxt_main_gate_ctx_t;


static nxt_port_t *
nxt_main_gate_port(nxt_task_t *task, nxt_runtime_t *rt, nxt_pid_t pid,
    nxt_process_type_t type)
{
    nxt_port_t  *port;

    port = nxt_runtime_process_port_create(task, rt, pid, 0, type);
    if (port != NULL) {
        port->pair[0] = -1;
        port->pair[1] = -1;
        port->socket.fd = -1;
    }

    return port;
}


static void
nxt_main_gate_port_release(nxt_task_t *task, nxt_port_t *port)
{
    if (port != NULL) {
        nxt_port_close(task, port);
        nxt_runtime_port_remove(task, port);
    }
}


static void
nxt_main_gate_close(nxt_fd_t fd)
{
    if (fd != -1 && nxt_test_fd_is_open(fd)) {
        (void) close(fd);
    }
}


static void
nxt_main_gate_msg_init(nxt_port_recv_msg_t *msg, nxt_port_t *port,
    nxt_uint_t type, nxt_pid_t sender, nxt_buf_t *buf)
{
    nxt_memzero(msg, sizeof(nxt_port_recv_msg_t));

    msg->buf = buf;
    msg->port = port;
    msg->fd[0] = -1;
    msg->fd[1] = -1;
    msg->port_msg.type = type;
    msg->port_msg.pid = sender;

#if (NXT_USE_CMSG_PID)
    msg->cmsg_pid = sender;
#endif
}


/* Every record of the fixture is still there and still has its port. */

static nxt_int_t
nxt_main_gate_records_kept(nxt_main_gate_ctx_t *ctx, const char *label)
{
    nxt_uint_t     i;
    nxt_process_t  *process;

    /* By pid: a record that was removed may be freed. */

    for (i = 0; i < nxt_nitems(ctx->pids); i++) {
        process = nxt_runtime_process_find(ctx->rt, ctx->pids[i]);

        NXT_TEST_CHECK(ctx->thr->log,
                       process == ctx->records[i]
                       && !nxt_queue_is_empty(&process->ports),
                       "main port gate test: %s: the record of process %PI "
                       "was removed", label, ctx->pids[i]);
    }

    return NXT_OK;
}


static nxt_int_t
nxt_main_gate_new_port_refused(nxt_main_gate_ctx_t *ctx,
    const nxt_main_gate_case_t *c)
{
    nxt_fd_t                 fd0, fd1;
    nxt_buf_t                *buf;
    nxt_int_t                ret;
    nxt_port_t               *port;
    nxt_uint_t               refused;
    nxt_port_recv_msg_t      msg;
    nxt_port_msg_new_port_t  new_port;

    buf = nxt_buf_mem_alloc(ctx->mp, sizeof(nxt_port_msg_new_port_t), 0);

    NXT_TEST_CHECK(ctx->thr->log, buf != NULL,
                   "main port gate test: no buffer");

    nxt_memzero(&new_port, sizeof(nxt_port_msg_new_port_t));

    new_port.id = c->id;
    new_port.pid = c->pid;
    new_port.type = c->type;
    new_port.max_size = 16 * 1024;
    new_port.max_share = 64 * 1024;

    buf->mem.free = nxt_cpymem(buf->mem.free, &new_port, c->size);

    nxt_main_gate_msg_init(&msg, ctx->main_port, _NXT_PORT_MSG_NEW_PORT,
                           c->sender, buf);

    fd0 = open("/dev/null", O_RDONLY);
    fd1 = open("/dev/null", O_RDONLY);

    msg.fd[0] = fd0;
    msg.fd[1] = fd1;

    refused = nxt_main_test_port_refused();

    nxt_main_test_run_port_handler(ctx->task, &msg);

    ret = NXT_ERROR;

    /* Port 0 of each process is in the fixture; no other port may appear. */

    port = (c->id != 0) ? nxt_runtime_port_find(ctx->rt, c->pid, c->id)
                        : NULL;

    if (nxt_slow_path(port != NULL)) {
        nxt_log_alert(ctx->thr->log, "main port gate test: NEW_PORT %s "
                      "created port %PI:%d", c->label, c->pid, (int) c->id);

        nxt_main_gate_port_release(ctx->task, port);
        goto done;
    }

    if (nxt_slow_path(fd0 == -1 || fd1 == -1
                      || nxt_test_fd_is_open(fd0)
                      || nxt_test_fd_is_open(fd1)
                      || msg.fd[0] != -1 || msg.fd[1] != -1))
    {
        nxt_log_alert(ctx->thr->log, "main port gate test: NEW_PORT %s "
                      "kept a descriptor", c->label);
        goto done;
    }

    if (nxt_slow_path(nxt_main_test_port_refused() != refused + 1)) {
        nxt_log_alert(ctx->thr->log, "main port gate test: NEW_PORT %s was "
                      "not refused", c->label);
        goto done;
    }

    ret = nxt_main_gate_records_kept(ctx, c->label);

done:

    nxt_main_gate_close(fd0);
    nxt_main_gate_close(fd1);

    nxt_mp_free(ctx->mp, buf);

    return ret;
}


static nxt_int_t
nxt_main_gate_remove_pid_refused(nxt_main_gate_ctx_t *ctx,
    const nxt_main_gate_case_t *c)
{
    nxt_buf_t            *buf;
    nxt_int_t            ret;
    nxt_uint_t           refused;
    nxt_port_recv_msg_t  msg;
    u_char               payload[2 * sizeof(nxt_pid_t)];

    buf = NULL;

    if (c->size != 0) {
        buf = nxt_buf_mem_alloc(ctx->mp, sizeof(payload), 0);

        NXT_TEST_CHECK(ctx->thr->log, buf != NULL,
                       "main port gate test: no buffer");

        nxt_memzero(payload, sizeof(payload));
        nxt_memcpy(payload, &c->pid, sizeof(nxt_pid_t));

        buf->mem.free = nxt_cpymem(buf->mem.free, payload, c->size);
    }

    nxt_main_gate_msg_init(&msg, ctx->main_port, _NXT_PORT_MSG_REMOVE_PID,
                           c->sender, buf);

    refused = nxt_main_test_port_refused();

    nxt_main_test_run_port_handler(ctx->task, &msg);

    ret = nxt_main_gate_records_kept(ctx, c->label);

    if (ret == NXT_OK
        && nxt_slow_path(nxt_main_test_port_refused() != refused + 1))
    {
        nxt_log_alert(ctx->thr->log, "main port gate test: REMOVE_PID %s was "
                      "not refused", c->label);
        ret = NXT_ERROR;
    }

    if (buf != NULL) {
        nxt_mp_free(ctx->mp, buf);
    }

    return ret;
}


static nxt_int_t
nxt_main_gate_new_port_msg(nxt_main_gate_ctx_t *ctx)
{
    nxt_int_t                ret;
    nxt_buf_t                buf;
    nxt_port_recv_msg_t      msg;
    nxt_port_msg_new_port_t  new_port, out;

    nxt_memzero(&new_port, sizeof(nxt_port_msg_new_port_t));
    nxt_memzero(&out, sizeof(nxt_port_msg_new_port_t));
    nxt_memzero(&buf, sizeof(nxt_buf_t));

    new_port.pid = nxt_pid + 2;
    new_port.type = NXT_PROCESS_APP;

    buf.mem.pos = (u_char *) &new_port;
    buf.mem.free = buf.mem.pos + sizeof(nxt_port_msg_new_port_t);

    nxt_main_gate_msg_init(&msg, NULL, _NXT_PORT_MSG_NEW_PORT, nxt_pid, &buf);

    ret = nxt_port_new_port_msg(&msg, &out);

    NXT_TEST_CHECK(ctx->thr->log, ret == NXT_OK && out.pid == new_port.pid,
                   "main port gate test: an application port message was "
                   "refused");

    new_port.type = NXT_PROCESS_MAX;

    ret = nxt_port_new_port_msg(&msg, &out);

    NXT_TEST_CHECK(ctx->thr->log, ret != NXT_OK,
                   "main port gate test: a new port message with type %d "
                   "was accepted", (int) out.type);

    return NXT_OK;
}


nxt_int_t
nxt_main_port_gate_test(nxt_thread_t *thr)
{
    nxt_mp_t             *mp;
    nxt_fd_t             fd0, fd1;
    nxt_int_t            ret;
    nxt_pid_t            proto_pid, child_pid, other_pid, router_pid;
    nxt_pid_t            proto2_pid, rchild_pid, unknown_pid;
    nxt_buf_t            *buf;
    nxt_task_t           *task;
    nxt_uint_t           i, refused;
    nxt_port_t           *main_port, *router_port, *proto_port, *child_port;
    nxt_port_t           *proto2_port, *other_port, *rchild_port, *port;
    nxt_runtime_t        *rt, *saved_rt;
    nxt_event_engine_t   engine, *saved_engine;
    nxt_main_gate_ctx_t  ctx;
    nxt_port_recv_msg_t  msg;

    nxt_port_msg_new_port_t  new_port;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main port gate test started");

    ret = NXT_ERROR;
    main_port = NULL;
    router_port = NULL;
    proto_port = NULL;
    child_port = NULL;
    proto2_port = NULL;
    other_port = NULL;
    rchild_port = NULL;

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

    nxt_memzero(&ctx, sizeof(ctx));

    ctx.thr = thr;
    ctx.task = task;
    ctx.mp = mp;
    ctx.rt = rt;

    if (nxt_slow_path(nxt_main_gate_new_port_msg(&ctx) != NXT_OK)) {
        goto done;
    }

    /*
     * This process stands in for main.  Two prototypes have one worker
     * each, as nxt_main_process_whoami_handler() links them.  The router
     * has a worker linked to it too, which main never does, so that only
     * the prototype check refuses the router for that worker.
     */

    proto_pid = nxt_pid + 1;
    child_pid = nxt_pid + 2;
    other_pid = nxt_pid + 3;
    router_pid = nxt_pid + 4;
    proto2_pid = nxt_pid + 6;
    rchild_pid = nxt_pid + 7;
    unknown_pid = nxt_pid + 9;

    main_port = nxt_main_gate_port(task, rt, nxt_pid, NXT_PROCESS_MAIN);
    router_port = nxt_main_gate_port(task, rt, router_pid, NXT_PROCESS_ROUTER);
    proto_port = nxt_main_gate_port(task, rt, proto_pid,
                                    NXT_PROCESS_PROTOTYPE);
    child_port = nxt_main_gate_port(task, rt, child_pid, NXT_PROCESS_APP);
    proto2_port = nxt_main_gate_port(task, rt, proto2_pid,
                                     NXT_PROCESS_PROTOTYPE);
    other_port = nxt_main_gate_port(task, rt, other_pid, NXT_PROCESS_APP);
    rchild_port = nxt_main_gate_port(task, rt, rchild_pid, NXT_PROCESS_APP);

    if (nxt_slow_path(main_port == NULL || router_port == NULL
                      || proto_port == NULL || child_port == NULL
                      || proto2_port == NULL || other_port == NULL
                      || rchild_port == NULL))
    {
        goto done;
    }

    nxt_queue_insert_tail(&proto_port->process->children,
                          &child_port->process->link);
    nxt_queue_insert_tail(&proto2_port->process->children,
                          &other_port->process->link);
    nxt_queue_insert_tail(&router_port->process->children,
                          &rchild_port->process->link);

    ctx.main_port = main_port;
    ctx.records[0] = router_port->process;
    ctx.records[1] = proto_port->process;
    ctx.records[2] = child_port->process;
    ctx.records[3] = proto2_port->process;
    ctx.records[4] = other_port->process;
    ctx.records[5] = rchild_port->process;

    ctx.pids[0] = router_pid;
    ctx.pids[1] = proto_pid;
    ctx.pids[2] = child_pid;
    ctx.pids[3] = proto2_pid;
    ctx.pids[4] = other_pid;
    ctx.pids[5] = rchild_pid;

    {
        /*
         * Each case names port 0, which main has, except the last one.  So
         * each case passes every check but the one it is for.
         */
        const nxt_main_gate_case_t  refused_new_port[] = {
            { router_pid, rchild_pid, 0, NXT_PROCESS_APP, NXT_MAIN_GATE_NP,
              "from the router for a worker linked to it" },
            { router_pid, child_pid, 0, NXT_PROCESS_APP, NXT_MAIN_GATE_NP,
              "from the router" },
            { child_pid, child_pid, 0, NXT_PROCESS_APP, NXT_MAIN_GATE_NP,
              "from a worker" },
            { unknown_pid, child_pid, 0, NXT_PROCESS_APP, NXT_MAIN_GATE_NP,
              "from an unknown process" },
            { proto_pid, child_pid, 0, NXT_PROCESS_ROUTER, NXT_MAIN_GATE_NP,
              "for a router port" },
            { proto_pid, other_pid, 0, NXT_PROCESS_APP, NXT_MAIN_GATE_NP,
              "for a worker of another prototype" },
            { proto_pid, proto_pid, 0, NXT_PROCESS_APP, NXT_MAIN_GATE_NP,
              "for the sender" },
            { proto_pid, unknown_pid, 0, NXT_PROCESS_APP, NXT_MAIN_GATE_NP,
              "for an unknown process" },
            { proto_pid, child_pid, 0, NXT_PROCESS_APP, NXT_MAIN_GATE_NP - 1,
              "one byte short" },
            { proto_pid, child_pid, 0, NXT_PROCESS_MAX, NXT_MAIN_GATE_NP,
              "with a type out of range" },
            { proto_pid, child_pid, NXT_MAIN_GATE_PORT_ID, NXT_PROCESS_APP,
              NXT_MAIN_GATE_NP,
              "for a port of a worker that main does not have" },
        };

        for (i = 0; i < nxt_nitems(refused_new_port); i++) {
            if (nxt_main_gate_new_port_refused(&ctx, &refused_new_port[i])
                != NXT_OK)
            {
                goto done;
            }
        }
    }

    {
        const nxt_main_gate_case_t  refused_remove_pid[] = {
            { router_pid, rchild_pid, 0, 0, NXT_MAIN_GATE_PID,
              "from the router for a worker linked to it" },
            { router_pid, child_pid, 0, 0, NXT_MAIN_GATE_PID,
              "from the router" },
            { other_pid, child_pid, 0, 0, NXT_MAIN_GATE_PID, "from a worker" },
            { unknown_pid, child_pid, 0, 0, NXT_MAIN_GATE_PID,
              "from an unknown process" },
            { proto_pid, other_pid, 0, 0, NXT_MAIN_GATE_PID,
              "for a worker of another prototype" },
            { proto_pid, router_pid, 0, 0, NXT_MAIN_GATE_PID,
              "for the router" },
            { proto_pid, proto_pid, 0, 0, NXT_MAIN_GATE_PID,
              "for the sender" },
            { proto_pid, unknown_pid, 0, 0, NXT_MAIN_GATE_PID,
              "for an unknown process" },
            { proto_pid, child_pid, 0, 0, 0, "with no payload" },
            { proto_pid, child_pid, 0, 0, NXT_MAIN_GATE_PID - 1,
              "with a short payload" },
            { proto_pid, child_pid, 0, 0, 2 * NXT_MAIN_GATE_PID,
              "with a long payload" },
        };

        for (i = 0; i < nxt_nitems(refused_remove_pid); i++) {
            if (nxt_main_gate_remove_pid_refused(&ctx, &refused_remove_pid[i])
                != NXT_OK)
            {
                goto done;
            }
        }
    }

    /*
     * Accepted: a prototype announces the port of its worker that main
     * created at WHOAMI time.  Main keeps the port it has, maps the queue
     * and closes both descriptors.
     */

    buf = nxt_buf_mem_alloc(mp, sizeof(nxt_port_msg_new_port_t), 0);
    if (nxt_slow_path(buf == NULL)) {
        goto done;
    }

    nxt_memzero(&new_port, sizeof(nxt_port_msg_new_port_t));

    new_port.id = 0;
    new_port.pid = child_pid;
    new_port.type = NXT_PROCESS_APP;
    new_port.max_size = 16 * 1024;
    new_port.max_share = 64 * 1024;

    buf->mem.free = nxt_cpymem(buf->mem.free, &new_port,
                               sizeof(nxt_port_msg_new_port_t));

    nxt_main_gate_msg_init(&msg, main_port, _NXT_PORT_MSG_NEW_PORT,
                           proto_pid, buf);

    fd0 = open("/dev/null", O_RDONLY);
    fd1 = nxt_shm_open(task, sizeof(nxt_port_queue_t));

    msg.fd[0] = fd0;
    msg.fd[1] = fd1;

    refused = nxt_main_test_port_refused();

    nxt_main_test_run_port_handler(task, &msg);

    nxt_mp_free(mp, buf);

    port = nxt_runtime_port_find(rt, child_pid, 0);

    if (nxt_slow_path(fd0 == -1 || fd1 == -1
                      || port != child_port || port->queue == NULL
                      || nxt_test_fd_is_open(fd0)
                      || nxt_test_fd_is_open(fd1)
                      || msg.fd[0] != -1 || msg.fd[1] != -1
                      || nxt_main_test_port_refused() != refused))
    {
        nxt_log_alert(thr->log, "main port gate test: a NEW_PORT of a "
                      "prototype for its worker was refused or did not map "
                      "the queue");

        nxt_main_gate_close(fd0);
        nxt_main_gate_close(fd1);

        goto done;
    }

    /* Accepted: a prototype reports the exit of its worker. */

    buf = nxt_buf_mem_alloc(mp, sizeof(nxt_pid_t), 0);
    if (nxt_slow_path(buf == NULL)) {
        goto done;
    }

    buf->mem.free = nxt_cpymem(buf->mem.free, &child_pid, sizeof(nxt_pid_t));

    nxt_main_gate_msg_init(&msg, main_port, _NXT_PORT_MSG_REMOVE_PID,
                           proto_pid, buf);

    nxt_main_test_run_port_handler(task, &msg);

    nxt_mp_free(mp, buf);

    if (nxt_slow_path(nxt_runtime_process_find(rt, child_pid) != NULL
                      || nxt_main_test_port_refused() != refused))
    {
        nxt_log_alert(thr->log, "main port gate test: a REMOVE_PID of a "
                      "prototype for its worker was refused");
        goto done;
    }

    child_port = NULL;

    if (nxt_slow_path(nxt_runtime_process_find(rt, other_pid)
                      != other_port->process
                      || nxt_runtime_process_find(rt, router_pid)
                         != router_port->process))
    {
        nxt_log_alert(thr->log, "main port gate test: a REMOVE_PID removed "
                      "another record");
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main port gate test passed");

    ret = NXT_OK;

done:

    nxt_main_gate_port_release(task, rchild_port);
    nxt_main_gate_port_release(task, other_port);
    nxt_main_gate_port_release(task, proto2_port);
    nxt_main_gate_port_release(task, child_port);
    nxt_main_gate_port_release(task, proto_port);
    nxt_main_gate_port_release(task, router_port);
    nxt_main_gate_port_release(task, main_port);

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);

    nxt_thread_mutex_destroy(&rt->processes_mutex);

    nxt_mp_destroy(mp);

    return ret;
}
