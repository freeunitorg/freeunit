/*
 * Copyright (C) FreeUnit Community
 */

/*
 * NEW_PORT and REMOVE_PID on the main port go through
 * nxt_main_gate_handler().  Main accepts them only from a registered
 * prototype, and only for a child of that prototype.  NEW_PORT must also
 * be for an application port that main has already.
 *
 * The messages go through the handler table of the main port, so the test
 * covers the table and not only the gate.  Each refused message must change
 * no record and must close the descriptors it carries.  It must be dropped
 * with no alert when main has no record of the sender or of the process it
 * names, and be refused with one alert otherwise.  Each accepted message
 * must do what it did before the gate.
 *
 * The sender is the pid that the kernel gives where the platform gives one.
 * There, cases with a header pid that differs from it check that the gate
 * reads the kernel pid.
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

#define NXT_MAIN_GATE_NEW_PORT    _NXT_PORT_MSG_NEW_PORT
#define NXT_MAIN_GATE_REMOVE_PID  _NXT_PORT_MSG_REMOVE_PID

/* What the gate does with a message. */
enum {
    NXT_MAIN_GATE_OK = 0,
    NXT_MAIN_GATE_DROP,
    NXT_MAIN_GATE_ALERT,
};

/* The processes of the fixture, by offset from the pid of this process. */
enum {
    NXT_MAIN_GATE_PROTO = 1,
    NXT_MAIN_GATE_PROTO2,
    NXT_MAIN_GATE_ROUTER,
    NXT_MAIN_GATE_CHILD,
    NXT_MAIN_GATE_OTHER,
    NXT_MAIN_GATE_RCHILD,
    NXT_MAIN_GATE_CHILD2,
    NXT_MAIN_GATE_UNKNOWN,
    NXT_MAIN_GATE_PIDS
};


typedef struct {
    nxt_uint_t          type;
    /* The pid in the header, and the kernel pid; 0 if it is the same. */
    nxt_uint_t          sender;
    nxt_uint_t          cmsg;
    nxt_uint_t          pid;
    nxt_port_id_t       id;
    nxt_process_type_t  ptype;
    size_t              size;
    nxt_uint_t          result;
    const char          *label;
} nxt_main_gate_case_t;


typedef struct {
    nxt_thread_t        *thr;
    nxt_task_t          *task;
    nxt_mp_t            *mp;
    nxt_runtime_t       *rt;
    nxt_port_t          *main_port;
    /* The record of each process, or NULL once it was removed. */
    nxt_process_t       *records[NXT_MAIN_GATE_PIDS];
} nxt_main_gate_ctx_t;


/*
 * Each case passes every check but the one it is for: the NEW_PORT cases
 * name port 0, which main has, except the one for a missing port, and the
 * router has a worker linked to it, so that only the prototype check
 * refuses the router for that worker.  The cases marked NXT_MAIN_GATE_OK
 * are accepted.
 */
static const nxt_main_gate_case_t  nxt_main_gate_cases[] = {
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_ROUTER, 0,
      NXT_MAIN_GATE_RCHILD, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_ALERT,
      "from the router for a worker linked to it" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_ROUTER, 0,
      NXT_MAIN_GATE_CHILD, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_ALERT,
      "from the router" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_CHILD, 0,
      NXT_MAIN_GATE_CHILD, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_ALERT,
      "from a worker" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_UNKNOWN, 0,
      NXT_MAIN_GATE_CHILD, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_DROP,
      "from an unknown process" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_CHILD, 0, NXT_PROCESS_ROUTER,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_ALERT,
      "for a router port" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_OTHER, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_ALERT,
      "for a worker of another prototype" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_PROTO, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_ALERT,
      "for the sender" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_UNKNOWN, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_DROP,
      "for an unknown process" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_CHILD, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP - 1, NXT_MAIN_GATE_ALERT,
      "one byte short" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_CHILD, 0, NXT_PROCESS_MAX,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_ALERT,
      "with a type out of range" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_CHILD, NXT_MAIN_GATE_PORT_ID, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_ALERT,
      "for a port of a worker that main does not have" },

    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_ROUTER, 0,
      NXT_MAIN_GATE_RCHILD, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_ALERT,
      "from the router for a worker linked to it" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_ROUTER, 0,
      NXT_MAIN_GATE_CHILD, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_ALERT,
      "from the router" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_OTHER, 0,
      NXT_MAIN_GATE_CHILD, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_ALERT,
      "from a worker" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_UNKNOWN, 0,
      NXT_MAIN_GATE_CHILD, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_DROP,
      "from an unknown process" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_OTHER, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_ALERT,
      "for a worker of another prototype" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_ROUTER, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_ALERT,
      "for the router" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_PROTO, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_ALERT,
      "for the sender" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_UNKNOWN, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_DROP,
      "for an unknown process" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_CHILD, 0, 0,
      0, NXT_MAIN_GATE_ALERT,
      "with no payload" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_CHILD, 0, 0,
      NXT_MAIN_GATE_PID - 1, NXT_MAIN_GATE_ALERT,
      "with a short payload" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_CHILD, 0, 0,
      2 * NXT_MAIN_GATE_PID, NXT_MAIN_GATE_ALERT,
      "with a long payload" },

#if (NXT_USE_CMSG_PID)

    /*
     * The header names a prototype, but the kernel names its worker: the
     * gate must read the kernel pid.  Then the other way round: the header
     * names the router, but the kernel names a prototype that reports a
     * worker of its own.
     */
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_PROTO, NXT_MAIN_GATE_CHILD,
      NXT_MAIN_GATE_CHILD, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_ALERT,
      "with the pid of a prototype in the header" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_PROTO, NXT_MAIN_GATE_CHILD,
      NXT_MAIN_GATE_CHILD, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_ALERT,
      "with the pid of a prototype in the header" },
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_ROUTER, NXT_MAIN_GATE_PROTO,
      NXT_MAIN_GATE_CHILD2, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_OK,
      "with the pid of the router in the header" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_ROUTER, NXT_MAIN_GATE_PROTO,
      NXT_MAIN_GATE_CHILD2, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_OK,
      "with the pid of the router in the header" },

#endif

    /*
     * A prototype announces the port of its worker that main created at
     * WHOAMI time.  Main keeps the port it has, maps the queue and closes
     * both descriptors.  Then the prototype reports the exit of the worker.
     */
    { NXT_MAIN_GATE_NEW_PORT, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_CHILD, 0, NXT_PROCESS_APP,
      NXT_MAIN_GATE_NP, NXT_MAIN_GATE_OK,
      "from a prototype for its worker" },
    { NXT_MAIN_GATE_REMOVE_PID, NXT_MAIN_GATE_PROTO, 0,
      NXT_MAIN_GATE_CHILD, 0, 0,
      NXT_MAIN_GATE_PID, NXT_MAIN_GATE_OK,
      "from a prototype for its worker" },
};


static nxt_uint_t         nxt_main_gate_alerts;
static nxt_log_handler_t  nxt_main_gate_next_handler;


/* Counts the alerts that the gate logs, and logs as before. */

static void nxt_cdecl
nxt_main_gate_log(nxt_uint_t level, nxt_log_t *log, const char *fmt, ...)
{
    u_char   *p;
    va_list  args;
    u_char   msg[NXT_MAX_ERROR_STR];

    if (level == NXT_LOG_ALERT) {
        nxt_main_gate_alerts++;
    }

    va_start(args, fmt);
    p = nxt_vsprintf(msg, msg + sizeof(msg), fmt, args);
    va_end(args);

    nxt_main_gate_next_handler(level, log, "%*s", (size_t) (p - msg), msg);
}


static nxt_pid_t
nxt_main_gate_pid(nxt_uint_t offset)
{
    return nxt_pid + (nxt_pid_t) offset;
}


static nxt_port_t *
nxt_main_gate_port(nxt_main_gate_ctx_t *ctx, nxt_uint_t offset,
    nxt_process_type_t type)
{
    nxt_port_t  *port;

    port = nxt_runtime_process_port_create(ctx->task, ctx->rt,
                                           nxt_main_gate_pid(offset), 0,
                                           type);
    if (port != NULL) {
        port->pair[0] = -1;
        port->pair[1] = -1;
        port->socket.fd = -1;

        ctx->records[offset] = port->process;
    }

    return port;
}


static void
nxt_main_gate_close(nxt_fd_t fd)
{
    if (fd != -1 && nxt_test_fd_is_open(fd)) {
        (void) close(fd);
    }
}


/* Every record of the fixture that should be there still is, with its port. */

static nxt_int_t
nxt_main_gate_records_kept(nxt_main_gate_ctx_t *ctx, const char *label)
{
    nxt_uint_t     i;
    nxt_process_t  *process;

    /* By pid: a record that was removed may be freed. */

    for (i = NXT_MAIN_GATE_PROTO; i < NXT_MAIN_GATE_UNKNOWN; i++) {
        if (ctx->records[i] == NULL) {
            continue;
        }

        process = nxt_runtime_process_find(ctx->rt, nxt_main_gate_pid(i));

        NXT_TEST_CHECK(ctx->thr->log,
                       process == ctx->records[i]
                       && !nxt_queue_is_empty(&process->ports),
                       "main port gate test: %s: the record of process %PI "
                       "was removed", label, nxt_main_gate_pid(i));
    }

    return NXT_OK;
}


static nxt_int_t
nxt_main_gate_run(nxt_main_gate_ctx_t *ctx, const nxt_main_gate_case_t *c)
{
    u_char                   *body;
    nxt_fd_t                 fd0, fd1;
    nxt_buf_t                *buf;
    nxt_int_t                ret;
    nxt_pid_t                pid;
    nxt_port_t               *port;
    nxt_uint_t               dropped, alerted, alerts;
    const char               *name;
    nxt_process_t            *process;
    nxt_port_recv_msg_t      msg;
    nxt_port_msg_new_port_t  new_port;

    name = (c->type == NXT_MAIN_GATE_NEW_PORT) ? "NEW_PORT" : "REMOVE_PID";

    pid = nxt_main_gate_pid(c->pid);

    /* The body is at most a whole NEW_PORT or twice a pid. */

    nxt_memzero(&new_port, sizeof(nxt_port_msg_new_port_t));

    new_port.id = c->id;
    new_port.pid = pid;
    new_port.type = c->ptype;
    new_port.max_size = 16 * 1024;
    new_port.max_share = 64 * 1024;

    body = (u_char *) &new_port;

    if (c->type == NXT_MAIN_GATE_REMOVE_PID) {
        nxt_memcpy(body, &pid, sizeof(nxt_pid_t));
    }

    buf = NULL;

    if (c->size != 0) {
        buf = nxt_buf_mem_alloc(ctx->mp, c->size, 0);

        NXT_TEST_CHECK(ctx->thr->log, buf != NULL,
                       "main port gate test: no buffer");

        buf->mem.free = nxt_cpymem(buf->mem.free, body, c->size);
    }

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.buf = buf;
    msg.port = ctx->main_port;
    msg.fd[0] = -1;
    msg.fd[1] = -1;
    msg.port_msg.type = c->type;
    msg.port_msg.pid = nxt_main_gate_pid(c->sender);

#if (NXT_USE_CMSG_PID)
    msg.cmsg_pid = nxt_main_gate_pid((c->cmsg != 0) ? c->cmsg : c->sender);
#endif

    /*
     * A NEW_PORT carries a socket and a queue; the accepted one maps it.  A
     * refused REMOVE_PID carries two descriptors too, and the gate has to
     * close them.
     */

    fd0 = -1;
    fd1 = -1;

    if (c->type == NXT_MAIN_GATE_NEW_PORT || c->result != NXT_MAIN_GATE_OK) {
        fd0 = open("/dev/null", O_RDONLY);
        fd1 = (c->result == NXT_MAIN_GATE_OK)
              ? nxt_shm_open(ctx->task, sizeof(nxt_port_queue_t))
              : open("/dev/null", O_RDONLY);

        msg.fd[0] = fd0;
        msg.fd[1] = fd1;
    }

    dropped = nxt_main_test_port_dropped();
    alerted = nxt_main_test_port_alerted();
    alerts = nxt_main_gate_alerts;

    nxt_main_test_run_port_handler(ctx->task, &msg);

    ret = NXT_ERROR;

    if (c->type == NXT_MAIN_GATE_NEW_PORT || c->result != NXT_MAIN_GATE_OK) {
        if (nxt_slow_path(fd0 == -1 || fd1 == -1
                          || nxt_test_fd_is_open(fd0)
                          || nxt_test_fd_is_open(fd1)
                          || msg.fd[0] != -1 || msg.fd[1] != -1))
        {
            nxt_log_alert(ctx->thr->log, "main port gate test: %s %s kept a "
                          "descriptor", name, c->label);
            goto done;
        }
    }

    if (c->type == NXT_MAIN_GATE_NEW_PORT) {

        /* Port 0 of each process is in the fixture; no other may appear. */

        port = nxt_runtime_port_find(ctx->rt, pid, c->id);

        if (c->id != 0 && nxt_slow_path(port != NULL)) {
            nxt_log_alert(ctx->thr->log, "main port gate test: %s %s created "
                          "port %PI:%d", name, c->label, pid, (int) c->id);

            nxt_port_close(ctx->task, port);
            nxt_runtime_port_remove(ctx->task, port);
            goto done;
        }

        if (c->result == NXT_MAIN_GATE_OK
            && nxt_slow_path(port == NULL || port->queue == NULL
                             || port->process != ctx->records[c->pid]))
        {
            nxt_log_alert(ctx->thr->log, "main port gate test: %s %s did not "
                          "map the queue on the port that main has", name,
                          c->label);
            goto done;
        }
    }

    if (c->result == NXT_MAIN_GATE_OK && c->type == NXT_MAIN_GATE_REMOVE_PID) {
        process = nxt_runtime_process_find(ctx->rt, pid);

        if (nxt_slow_path(process != NULL)) {
            nxt_log_alert(ctx->thr->log, "main port gate test: %s %s kept "
                          "the record of process %PI", name, c->label, pid);
            goto done;
        }

        ctx->records[c->pid] = NULL;
    }

    /* Exactly one of: accepted, dropped with no alert, one alert. */

    dropped = nxt_main_test_port_dropped() - dropped;
    alerted = nxt_main_test_port_alerted() - alerted;
    alerts = nxt_main_gate_alerts - alerts;

    if (nxt_slow_path(dropped != (c->result == NXT_MAIN_GATE_DROP)
                      || alerted != (c->result == NXT_MAIN_GATE_ALERT)
                      || alerts != alerted))
    {
        nxt_log_alert(ctx->thr->log, "main port gate test: %s %s was "
                      "dropped %ui times and refused %ui times with %ui "
                      "alerts; expected %s", name, c->label, dropped,
                      alerted, alerts,
                      (c->result == NXT_MAIN_GATE_OK) ? "to accept it"
                      : (c->result == NXT_MAIN_GATE_DROP) ? "one silent drop"
                      : "one refusal with one alert");
        goto done;
    }

    ret = nxt_main_gate_records_kept(ctx, c->label);

done:

    nxt_main_gate_close(fd0);
    nxt_main_gate_close(fd1);

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
    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    new_port.pid = nxt_pid + 2;
    new_port.type = NXT_PROCESS_APP;

    buf.mem.pos = (u_char *) &new_port;
    buf.mem.free = buf.mem.pos + sizeof(nxt_port_msg_new_port_t);

    msg.buf = &buf;
    msg.port_msg.type = _NXT_PORT_MSG_NEW_PORT;

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
    nxt_int_t            ret;
    nxt_port_t           *port, *proto_port, *child_port, *proto2_port;
    nxt_port_t           *other_port, *router_port, *rchild_port;
    nxt_port_t           *child2_port;
    nxt_uint_t           i;
    nxt_runtime_t        *rt, *saved_rt;
    nxt_event_engine_t   engine, *saved_engine;
    nxt_main_gate_ctx_t  ctx;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main port gate test started");

    ret = NXT_ERROR;

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
    ctx.task = thr->task;
    ctx.task->thread = thr;
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

    ctx.main_port = nxt_main_gate_port(&ctx, 0, NXT_PROCESS_MAIN);
    router_port = nxt_main_gate_port(&ctx, NXT_MAIN_GATE_ROUTER,
                                     NXT_PROCESS_ROUTER);
    proto_port = nxt_main_gate_port(&ctx, NXT_MAIN_GATE_PROTO,
                                    NXT_PROCESS_PROTOTYPE);
    child_port = nxt_main_gate_port(&ctx, NXT_MAIN_GATE_CHILD,
                                    NXT_PROCESS_APP);
    proto2_port = nxt_main_gate_port(&ctx, NXT_MAIN_GATE_PROTO2,
                                     NXT_PROCESS_PROTOTYPE);
    other_port = nxt_main_gate_port(&ctx, NXT_MAIN_GATE_OTHER,
                                    NXT_PROCESS_APP);
    rchild_port = nxt_main_gate_port(&ctx, NXT_MAIN_GATE_RCHILD,
                                     NXT_PROCESS_APP);
    child2_port = nxt_main_gate_port(&ctx, NXT_MAIN_GATE_CHILD2,
                                     NXT_PROCESS_APP);

    if (nxt_slow_path(ctx.main_port == NULL || router_port == NULL
                      || proto_port == NULL || child_port == NULL
                      || proto2_port == NULL || other_port == NULL
                      || rchild_port == NULL || child2_port == NULL))
    {
        goto done;
    }

    nxt_queue_insert_tail(&proto_port->process->children,
                          &child_port->process->link);
    nxt_queue_insert_tail(&proto_port->process->children,
                          &child2_port->process->link);
    nxt_queue_insert_tail(&proto2_port->process->children,
                          &other_port->process->link);
    nxt_queue_insert_tail(&router_port->process->children,
                          &rchild_port->process->link);

    nxt_main_gate_next_handler = ctx.task->log->handler;
    ctx.task->log->handler = nxt_main_gate_log;

    for (i = 0; i < nxt_nitems(nxt_main_gate_cases); i++) {
        if (nxt_main_gate_run(&ctx, &nxt_main_gate_cases[i]) != NXT_OK) {
            break;
        }
    }

    ctx.task->log->handler = nxt_main_gate_next_handler;

    if (i != nxt_nitems(nxt_main_gate_cases)) {
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main port gate test passed");

    ret = NXT_OK;

done:

    /*
     * By pid, because the accepted REMOVE_PID has freed the record of the
     * worker; the workers first, because a parent is released before them.
     */

    for (i = NXT_MAIN_GATE_UNKNOWN; i-- > 0; ) {
        port = nxt_runtime_port_find(rt, nxt_main_gate_pid(i), 0);

        if (port != NULL) {
            nxt_port_close(ctx.task, port);
            nxt_runtime_port_remove(ctx.task, port);
        }
    }

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);

    nxt_thread_mutex_destroy(&rt->processes_mutex);

    nxt_mp_destroy(mp);

    return ret;
}
