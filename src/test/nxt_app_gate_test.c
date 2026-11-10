/*
 * Copyright (C) FreeUnit Community
 */

/*
 * NEW_PORT and REMOVE_PID on the prototype and discovery ports go through
 * nxt_app_gate_handler().  Both ports are a copy of the main port socket, so
 * any process that holds one -- an application worker among them -- can write
 * to them.  The gate accepts the two types only from the main process and
 * drops them from anyone else, closing the descriptors they carry.
 *
 * The test drives each message through the real prototype and discovery
 * handler tables, as nxt_port_handler() does, so it covers the wiring and not
 * only the gate: a message from main is accepted and dispatched to the handler
 * that ran before the gate; a message from any other sender is refused with
 * one alert and its descriptors closed.
 *
 * The sender is the pid the kernel gives where the platform gives one.  There,
 * a case whose header pid is main's but whose kernel pid is not checks that the
 * gate reads the kernel pid.
 *
 * Under "isolation": {"namespaces": {"pid": true}} the prototype is the init of
 * its own namespace and main lives outside it, so the kernel translates main's
 * credential to 0.  A case with rt->is_pid_isolated set then checks that the
 * gate still accepts the zero credential from main -- so an isolated prototype
 * keeps clearing dead process records -- while a worker inside the namespace,
 * which presents a non-zero namespace-local pid, is still refused.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_main_process.h>
#include <nxt_application.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


#define NXT_APP_GATE_NEW_PORT    _NXT_PORT_MSG_NEW_PORT
#define NXT_APP_GATE_REMOVE_PID  _NXT_PORT_MSG_REMOVE_PID

/*
 * A c->cmsg that is not an offset from this process's pid: the kernel stamped
 * pid 0, i.e. the sender is outside the receiver's pid namespace (main or the
 * router under "isolation": {"namespaces": {"pid": true}}).
 */
#define NXT_APP_GATE_NS_OUTSIDE  ((nxt_uint_t) -1)

/* The processes of the fixture, by offset from the pid of this process. */
enum {
    NXT_APP_GATE_MAIN = 1,
    NXT_APP_GATE_RECV,
    NXT_APP_GATE_WORKER,
    NXT_APP_GATE_PIDS
};

/* Which handler table the message arrives through. */
enum {
    NXT_APP_GATE_PROTO_TABLE = 0,
    NXT_APP_GATE_DISCOVERY_TABLE,
};


typedef struct {
    nxt_uint_t  type;
    /*
     * The pid in the header; and the kernel pid: an offset from this process's
     * pid, NXT_APP_GATE_NS_OUTSIDE for a sender outside the receiver's pid
     * namespace (kernel pid 0), or 0 to reuse the header pid.
     */
    nxt_uint_t  sender;
    nxt_uint_t  cmsg;
    nxt_uint_t  table;
    nxt_bool_t  accept;
    const char  *label;
    /*
     * Sets rt->is_pid_isolated for the case; omitted (0) is a shared namespace,
     * where the sender's credential is its global pid.
     */
    nxt_bool_t  isolated;
} nxt_app_gate_case_t;


typedef struct {
    nxt_thread_t   *thr;
    nxt_task_t     *task;
    nxt_mp_t       *mp;
    nxt_runtime_t  *rt;
    nxt_port_t     *main_port;
    nxt_port_t     *recv_port;
    nxt_port_t     *worker_port;
} nxt_app_gate_ctx_t;


static const nxt_app_gate_case_t  nxt_app_gate_cases[] = {

    /* From the main process: accepted and dispatched to the real handler. */

    { NXT_APP_GATE_NEW_PORT, NXT_APP_GATE_MAIN, 0,
      NXT_APP_GATE_PROTO_TABLE, 1,
      "from main, prototype port", 0 },
    { NXT_APP_GATE_REMOVE_PID, NXT_APP_GATE_MAIN, 0,
      NXT_APP_GATE_PROTO_TABLE, 1,
      "from main, prototype port", 0 },
    { NXT_APP_GATE_NEW_PORT, NXT_APP_GATE_MAIN, 0,
      NXT_APP_GATE_DISCOVERY_TABLE, 1,
      "from main, discovery port", 0 },
    { NXT_APP_GATE_REMOVE_PID, NXT_APP_GATE_MAIN, 0,
      NXT_APP_GATE_DISCOVERY_TABLE, 1,
      "from main, discovery port", 0 },

    /* From a worker: refused, and the descriptors it carries are closed. */

    { NXT_APP_GATE_NEW_PORT, NXT_APP_GATE_WORKER, 0,
      NXT_APP_GATE_PROTO_TABLE, 0,
      "from a worker, prototype port", 0 },
    { NXT_APP_GATE_REMOVE_PID, NXT_APP_GATE_WORKER, 0,
      NXT_APP_GATE_PROTO_TABLE, 0,
      "from a worker, prototype port", 0 },
    { NXT_APP_GATE_NEW_PORT, NXT_APP_GATE_WORKER, 0,
      NXT_APP_GATE_DISCOVERY_TABLE, 0,
      "from a worker, discovery port", 0 },
    { NXT_APP_GATE_REMOVE_PID, NXT_APP_GATE_WORKER, 0,
      NXT_APP_GATE_DISCOVERY_TABLE, 0,
      "from a worker, discovery port", 0 },

#if (NXT_USE_CMSG_PID)

    /*
     * The header names main, but the kernel names a worker: the gate must read
     * the kernel pid and refuse.  Were it to trust the header, these would be
     * accepted.
     */

    { NXT_APP_GATE_NEW_PORT, NXT_APP_GATE_MAIN, NXT_APP_GATE_WORKER,
      NXT_APP_GATE_PROTO_TABLE, 0,
      "with main's pid in the header but a worker's kernel pid", 0 },
    { NXT_APP_GATE_REMOVE_PID, NXT_APP_GATE_MAIN, NXT_APP_GATE_WORKER,
      NXT_APP_GATE_PROTO_TABLE, 0,
      "with main's pid in the header but a worker's kernel pid", 0 },

    /*
     * Under pid isolation the prototype is the init of its own namespace, so
     * main's credential arrives as 0.  The gate must accept it and dispatch to
     * the real handler, or an isolated prototype never clears a dead process's
     * ports or record.  The last field sets rt->is_pid_isolated.
     */

    { NXT_APP_GATE_NEW_PORT, NXT_APP_GATE_MAIN, NXT_APP_GATE_NS_OUTSIDE,
      NXT_APP_GATE_PROTO_TABLE, 1,
      "from main, outside the prototype's pid namespace", 1 },
    { NXT_APP_GATE_REMOVE_PID, NXT_APP_GATE_MAIN, NXT_APP_GATE_NS_OUTSIDE,
      NXT_APP_GATE_PROTO_TABLE, 1,
      "from main, outside the prototype's pid namespace", 1 },

    /*
     * A worker inside that namespace presents a non-zero namespace-local pid,
     * so the gate still refuses it: the zero credential is a namespace boundary
     * the worker cannot cross, not an identity it can forge.
     */

    { NXT_APP_GATE_NEW_PORT, NXT_APP_GATE_WORKER, 0,
      NXT_APP_GATE_PROTO_TABLE, 0,
      "from a worker, inside the prototype's pid namespace", 1 },
    { NXT_APP_GATE_REMOVE_PID, NXT_APP_GATE_WORKER, 0,
      NXT_APP_GATE_PROTO_TABLE, 0,
      "from a worker, inside the prototype's pid namespace", 1 },

#endif
};


static nxt_uint_t         nxt_app_gate_alerts;
static nxt_log_handler_t  nxt_app_gate_next_handler;


/* Counts the alerts that the gate logs, and logs as before. */

static void nxt_cdecl
nxt_app_gate_log(nxt_uint_t level, nxt_log_t *log, const char *fmt, ...)
{
    u_char   *p;
    va_list  args;
    u_char   msg[NXT_MAX_ERROR_STR];

    if (level == NXT_LOG_ALERT) {
        nxt_app_gate_alerts++;
    }

    va_start(args, fmt);
    p = nxt_vsprintf(msg, msg + sizeof(msg), fmt, args);
    va_end(args);

    nxt_app_gate_next_handler(level, log, "%*s", (size_t) (p - msg), msg);
}


static nxt_pid_t
nxt_app_gate_pid(nxt_uint_t offset)
{
    return nxt_pid + (nxt_pid_t) offset;
}


static nxt_port_t *
nxt_app_gate_port(nxt_app_gate_ctx_t *ctx, nxt_uint_t offset,
    nxt_process_type_t type)
{
    nxt_port_t  *port;

    port = nxt_runtime_process_port_create(ctx->task, ctx->rt,
                                           nxt_app_gate_pid(offset), 0, type);
    if (port != NULL) {
        port->pair[0] = -1;
        port->pair[1] = -1;
        port->socket.fd = -1;
    }

    return port;
}


static void
nxt_app_gate_close(nxt_fd_t fd)
{
    if (fd != -1 && nxt_test_fd_is_open(fd)) {
        (void) close(fd);
    }
}


static nxt_int_t
nxt_app_gate_run(nxt_app_gate_ctx_t *ctx, const nxt_app_gate_case_t *c)
{
    size_t                   size;
    u_char                   *body;
    nxt_fd_t                 fd0, fd1;
    nxt_buf_t                *buf;
    nxt_int_t                ret;
    nxt_pid_t                worker_pid, gone_pid;
    nxt_bool_t               fds;
    nxt_uint_t               accepted, refused, alerts;
    const char               *name;
    nxt_port_recv_msg_t      msg;
    nxt_port_msg_new_port_t  new_port;

    name = (c->type == NXT_APP_GATE_NEW_PORT) ? "NEW_PORT" : "REMOVE_PID";

    worker_pid = nxt_app_gate_pid(NXT_APP_GATE_WORKER);

    /* A pid no process of the fixture has, so an accepted REMOVE_PID removes
       nothing and leaves the fixture intact. */
    gone_pid = nxt_app_gate_pid(NXT_APP_GATE_PIDS);

    nxt_memzero(&new_port, sizeof(nxt_port_msg_new_port_t));

    if (c->type == NXT_APP_GATE_NEW_PORT) {
        /* Names the worker port the fixture already has, so an accepted
           announcement finds it and creates no port. */
        new_port.id = 0;
        new_port.pid = worker_pid;
        new_port.type = NXT_PROCESS_APP;
        new_port.max_size = 16 * 1024;
        new_port.max_share = 64 * 1024;

        body = (u_char *) &new_port;
        size = sizeof(nxt_port_msg_new_port_t);

    } else {
        body = (u_char *) &gone_pid;
        size = sizeof(nxt_pid_t);
    }

    buf = nxt_buf_mem_alloc(ctx->mp, size, 0);
    if (nxt_slow_path(buf == NULL)) {
        nxt_log_alert(ctx->thr->log, "app gate test: no buffer");
        return NXT_ERROR;
    }

    buf->mem.free = nxt_cpymem(buf->mem.free, body, size);

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.buf = buf;
    msg.port = ctx->recv_port;
    msg.fd[0] = -1;
    msg.fd[1] = -1;
    msg.port_msg.type = c->type;
    msg.port_msg.pid = nxt_app_gate_pid(c->sender);

#if (NXT_USE_CMSG_PID)
    if (c->cmsg == NXT_APP_GATE_NS_OUTSIDE) {
        /* The kernel translates a sender outside the namespace to pid 0. */
        msg.cmsg_pid = 0;

    } else {
        msg.cmsg_pid = nxt_app_gate_pid((c->cmsg != 0) ? c->cmsg : c->sender);
    }
#endif

    ctx->rt->is_pid_isolated = c->isolated;

    /*
     * A refused message, and every NEW_PORT, carries two descriptors that must
     * be gone afterwards: the gate closes a refused message's, and
     * nxt_app_new_port_handler() closes an accepted announcement's.  An
     * accepted REMOVE_PID carries none.
     */

    fds = (c->type == NXT_APP_GATE_NEW_PORT || !c->accept);

    fd0 = -1;
    fd1 = -1;

    if (fds) {
        fd0 = open("/dev/null", O_RDONLY);
        fd1 = open("/dev/null", O_RDONLY);

        msg.fd[0] = fd0;
        msg.fd[1] = fd1;
    }

    accepted = nxt_app_test_gate_accepted();
    refused = nxt_app_test_gate_refused();
    alerts = nxt_app_gate_alerts;

    ret = NXT_ERROR;

    if (c->table == NXT_APP_GATE_DISCOVERY_TABLE) {
        nxt_app_test_run_discovery_port_handler(ctx->task, &msg);

    } else {
        nxt_app_test_run_proto_port_handler(ctx->task, &msg);
    }

    if (fds) {
        if (nxt_slow_path(fd0 == -1 || fd1 == -1
                          || nxt_test_fd_is_open(fd0)
                          || nxt_test_fd_is_open(fd1)
                          || msg.fd[0] != -1 || msg.fd[1] != -1))
        {
            nxt_log_alert(ctx->thr->log, "app gate test: %s %s kept a "
                          "descriptor", name, c->label);
            goto done;
        }
    }

    /* Exactly one of accepted or refused, and a refusal alerts once. */

    accepted = nxt_app_test_gate_accepted() - accepted;
    refused = nxt_app_test_gate_refused() - refused;
    alerts = nxt_app_gate_alerts - alerts;

    if (nxt_slow_path(accepted != (nxt_uint_t) (c->accept != 0)
                      || refused != (nxt_uint_t) (c->accept == 0)
                      || alerts != refused))
    {
        nxt_log_alert(ctx->thr->log, "app gate test: %s %s was accepted %ui "
                      "times and refused %ui times with %ui alerts; expected "
                      "%s", name, c->label, accepted, refused, alerts,
                      c->accept ? "one acceptance"
                                : "one refusal with one alert");
        goto done;
    }

    /* An accepted message reaches the handler that ran before the gate. */

    if (c->accept) {
        if (c->type == NXT_APP_GATE_NEW_PORT
            ? (msg.u.new_port != ctx->worker_port)
            : (msg.u.removed_pid != gone_pid))
        {
            nxt_log_alert(ctx->thr->log, "app gate test: %s %s was not "
                          "dispatched to the real handler", name, c->label);
            goto done;
        }
    }

    ret = NXT_OK;

done:

    nxt_app_gate_close(fd0);
    nxt_app_gate_close(fd1);

    nxt_mp_free(ctx->mp, buf);

    return ret;
}


nxt_int_t
nxt_app_gate_test(nxt_thread_t *thr)
{
    nxt_mp_t            *mp;
    nxt_int_t           ret;
    nxt_port_t          *port;
    nxt_uint_t          i;
    nxt_runtime_t       *rt, *saved_rt;
    nxt_event_engine_t  engine, *saved_engine;
    nxt_app_gate_ctx_t  ctx;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "app gate test started");

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

    /*
     * This process stands in for the prototype or discovery process.  Its main
     * port is what nxt_app_gate_handler() checks the sender against; the worker
     * is the process that forges a message, and the one an accepted NEW_PORT
     * names.
     */

    ctx.main_port = nxt_app_gate_port(&ctx, NXT_APP_GATE_MAIN,
                                      NXT_PROCESS_MAIN);
    ctx.recv_port = nxt_app_gate_port(&ctx, NXT_APP_GATE_RECV,
                                      NXT_PROCESS_PROTOTYPE);
    ctx.worker_port = nxt_app_gate_port(&ctx, NXT_APP_GATE_WORKER,
                                        NXT_PROCESS_APP);

    if (nxt_slow_path(ctx.main_port == NULL || ctx.recv_port == NULL
                      || ctx.worker_port == NULL))
    {
        goto done;
    }

    nxt_app_gate_next_handler = ctx.task->log->handler;
    ctx.task->log->handler = nxt_app_gate_log;

    for (i = 0; i < nxt_nitems(nxt_app_gate_cases); i++) {
        if (nxt_app_gate_run(&ctx, &nxt_app_gate_cases[i]) != NXT_OK) {
            break;
        }
    }

    ctx.task->log->handler = nxt_app_gate_next_handler;

    if (i != nxt_nitems(nxt_app_gate_cases)) {
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "app gate test passed");

    ret = NXT_OK;

done:

    for (i = NXT_APP_GATE_PIDS; i-- > 0; ) {
        port = nxt_runtime_port_find(rt, nxt_app_gate_pid(i), 0);

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
