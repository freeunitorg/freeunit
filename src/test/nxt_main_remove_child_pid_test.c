/*
 * Copyright (C) FreeUnit Community
 */

/*
 * Issue #310: under "isolation": {"namespaces": {"pid": true}} a worker that
 * died before PROCESS_CREATED left its record, port and descriptor in main
 * until the prototype exited.  The prototype knows such a worker only by its
 * namespace-local pid, which REMOVE_PID must not broadcast.
 *
 * Main keeps that pid next to the global one at WHOAMI time
 * (nxt_main_process_name_child()) and resolves REMOVE_CHILD_PID against it,
 * among the sender's own children only.  Both halves are driven here.
 *
 * Naming must refuse 0, a negative pid and a name a live sibling holds, and
 * must keep a name equal to the global pid.  Resolving must refuse a short
 * payload, pid 0, a negative pid, an unknown sender, a sender that is not a
 * prototype and a pid no child of the sender holds; when it accepts, it must
 * retire the record and tell the router by the global pid.
 *
 * Without NXT_USE_CMSG_PID the handler is not registered, and nothing is
 * tested.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_main_process.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


#if (NXT_USE_CMSG_PID)

/* Namespace-local pids of two workers. */
#define NXT_MAIN_RCP_NS_PID        7
#define NXT_MAIN_RCP_OTHER_NS_PID  8


typedef struct {
    nxt_pid_t   sender;
    nxt_pid_t   pid;
    nxt_bool_t  payload;
    const char  *label;
} nxt_main_rcp_case_t;


static nxt_port_t *
nxt_main_rcp_port(nxt_task_t *task, nxt_runtime_t *rt, nxt_pid_t pid,
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
nxt_main_rcp_port_release(nxt_task_t *task, nxt_port_t *port)
{
    if (port != NULL) {
        nxt_port_close(task, port);
        nxt_runtime_port_remove(task, port);
    }
}


/*
 * Drive the handler once.  The fixture ports are never write_ready, so what
 * the handler sends stays on port->messages for the test to read.
 */

static nxt_int_t
nxt_main_rcp_report(nxt_thread_t *thr, nxt_task_t *task, nxt_mp_t *mp,
    nxt_pid_t sender, nxt_pid_t pid, nxt_bool_t payload)
{
    nxt_buf_t            *buf;
    nxt_port_recv_msg_t  msg;

    buf = NULL;

    if (payload) {
        buf = nxt_buf_mem_alloc(mp, sizeof(nxt_pid_t), 0);
        if (nxt_slow_path(buf == NULL)) {
            nxt_log_alert(thr->log, "main remove child pid test: no buffer");
            return NXT_ERROR;
        }

        buf->mem.free = nxt_cpymem(buf->mem.free, &pid, sizeof(nxt_pid_t));
    }

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.buf = buf;
    msg.fd[0] = -1;
    msg.fd[1] = -1;
    msg.port_msg.type = _NXT_PORT_MSG_REMOVE_CHILD_PID;
    msg.port_msg.pid = sender;
    msg.cmsg_pid = sender;

    nxt_main_test_run_remove_child_pid_handler(task, &msg);

    if (buf != NULL) {
        nxt_mp_free(mp, buf);
    }

    return NXT_OK;
}


/* The router got one REMOVE_PID naming pid, and nothing else. */

static nxt_int_t
nxt_main_rcp_router_told(nxt_thread_t *thr, nxt_port_t *router_port,
    nxt_pid_t pid, const char *label)
{
    nxt_pid_t            sent_pid;
    nxt_port_send_msg_t  *sent;

    if (nxt_slow_path(nxt_queue_is_empty(&router_port->messages))) {
        nxt_log_alert(thr->log, "main remove child pid test: %s: the router "
                      "was not told", label);
        return NXT_ERROR;
    }

    sent = nxt_queue_link_data(nxt_queue_first(&router_port->messages),
                               nxt_port_send_msg_t, link);

    if (nxt_slow_path(sent->port_msg.type != _NXT_PORT_MSG_REMOVE_PID
                      || sent->buf == NULL
                      || nxt_buf_used_size(sent->buf) != sizeof(nxt_pid_t)))
    {
        nxt_log_alert(thr->log, "main remove child pid test: %s: the router "
                      "got a message of type %d, not a REMOVE_PID", label,
                      (int) sent->port_msg.type);
        return NXT_ERROR;
    }

    nxt_memcpy(&sent_pid, sent->buf->mem.pos, sizeof(nxt_pid_t));

    if (nxt_slow_path(sent_pid != pid)) {
        nxt_log_alert(thr->log, "main remove child pid test: %s: the router "
                      "was told to remove pid %PI, not %PI", label, sent_pid,
                      pid);
        return NXT_ERROR;
    }

    if (nxt_slow_path(nxt_queue_next(nxt_queue_first(&router_port->messages))
                      != nxt_queue_tail(&router_port->messages)))
    {
        nxt_log_alert(thr->log, "main remove child pid test: %s: the router "
                      "got more than one message", label);
        return NXT_ERROR;
    }

    /* Queued messages hold a port reference each; give them back. */
    nxt_port_test_run_error_handler(thr->task, router_port);

    return NXT_OK;
}


nxt_int_t
nxt_main_remove_child_pid_test(nxt_thread_t *thr)
{
    nxt_mp_t             *mp;
    nxt_int_t            ret;
    nxt_pid_t            proto_pid, child_pid, other_pid, twin_pid;
    nxt_task_t           *task;
    nxt_uint_t           i;
    nxt_port_t           *router_port, *proto_port, *child_port, *other_port;
    nxt_port_t           *twin_port;
    nxt_runtime_t        *rt, *saved_rt;
    nxt_process_t        *proto, *child, *other, *twin;
    nxt_event_engine_t   engine, *saved_engine;
    nxt_port_send_msg_t  *sent;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "main remove child pid test started");

    ret = NXT_ERROR;
    router_port = NULL;
    proto_port = NULL;
    child_port = NULL;
    other_port = NULL;
    twin_port = NULL;

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

    /* nxt_port_remove_notify_others() allocates from engine->mem_pool. */
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
     * This process stands in for main, so every fixture pid must differ from
     * nxt_pid: nxt_port_remove_notify_others() skips the running process.
     * twin is the worker whose local pid equals its global pid.
     */

    proto_pid = nxt_pid + 1;
    child_pid = nxt_pid + 2;
    other_pid = nxt_pid + 3;
    twin_pid = nxt_pid + 5;

    router_port = nxt_main_rcp_port(task, rt, nxt_pid + 4, NXT_PROCESS_ROUTER);
    proto_port = nxt_main_rcp_port(task, rt, proto_pid, NXT_PROCESS_PROTOTYPE);
    child_port = nxt_main_rcp_port(task, rt, child_pid, NXT_PROCESS_APP);
    other_port = nxt_main_rcp_port(task, rt, other_pid, NXT_PROCESS_APP);
    twin_port = nxt_main_rcp_port(task, rt, twin_pid, NXT_PROCESS_APP);

    if (nxt_slow_path(router_port == NULL || proto_port == NULL
                      || child_port == NULL || other_port == NULL
                      || twin_port == NULL))
    {
        goto done;
    }

    proto = proto_port->process;
    child = child_port->process;
    other = other_port->process;
    twin = twin_port->process;

    /* As nxt_main_process_whoami_handler() leaves the prototype's workers. */

    nxt_queue_insert_tail(&proto->children, &child->link);
    nxt_queue_insert_tail(&proto->children, &other->link);
    nxt_queue_insert_tail(&proto->children, &twin->link);

    /* Naming: 0 and a negative pid are refused. */

    nxt_main_test_run_name_child(task, proto, child, 0);
    nxt_main_test_run_name_child(task, proto, child, -1);

    if (nxt_slow_path(child->parent_ns_pid != 0)) {
        nxt_log_alert(thr->log, "main remove child pid test: a worker was "
                      "named %PI", child->parent_ns_pid);
        goto done;
    }

    nxt_main_test_run_name_child(task, proto, child, NXT_MAIN_RCP_NS_PID);
    nxt_main_test_run_name_child(task, proto, other,
                                 NXT_MAIN_RCP_OTHER_NS_PID);

    if (nxt_slow_path(child->parent_ns_pid != NXT_MAIN_RCP_NS_PID
                      || other->parent_ns_pid != NXT_MAIN_RCP_OTHER_NS_PID))
    {
        nxt_log_alert(thr->log, "main remove child pid test: a worker was "
                      "not named");
        goto done;
    }

    /* A name a live sibling holds is refused; the sibling keeps it. */

    nxt_main_test_run_name_child(task, proto, twin, NXT_MAIN_RCP_NS_PID);

    if (nxt_slow_path(twin->parent_ns_pid != 0
                      || child->parent_ns_pid != NXT_MAIN_RCP_NS_PID))
    {
        nxt_log_alert(thr->log, "main remove child pid test: a worker was "
                      "allowed to claim a live sibling's name");
        goto done;
    }

    /*
     * Reports that must retire nothing and tell the router nothing.  twin
     * still has no name, so a handler that reads 0 as a name would retire
     * it.  The router holds a named child of its own for the non-prototype
     * case, so that only the type check keeps that record.
     */

    nxt_queue_remove(&other->link);
    nxt_queue_insert_tail(&router_port->process->children, &other->link);

    {
        const nxt_main_rcp_case_t  refused[] = {
            { proto_pid, NXT_MAIN_RCP_NS_PID, 0, "a payload with no pid" },
            { proto_pid, 0, 1, "pid 0" },
            { proto_pid, -1, 1, "a negative pid" },
            { nxt_pid + 9, NXT_MAIN_RCP_NS_PID, 1, "an unknown sender" },
            { router_port->pid, NXT_MAIN_RCP_OTHER_NS_PID, 1,
              "a sender that is not a prototype" },
            { proto_pid, NXT_MAIN_RCP_NS_PID + 100, 1, "a pid of no child" },
        };

        for (i = 0; i < nxt_nitems(refused); i++) {
            ret = nxt_main_rcp_report(thr, task, mp, refused[i].sender,
                                      refused[i].pid, refused[i].payload);
            if (nxt_slow_path(ret != NXT_OK)) {
                goto done;
            }

            ret = NXT_ERROR;

            if (nxt_slow_path(nxt_runtime_process_find(rt, child_pid) != child
                              || nxt_runtime_process_find(rt, other_pid)
                                 != other
                              || nxt_runtime_process_find(rt, twin_pid)
                                 != twin))
            {
                nxt_log_alert(thr->log, "main remove child pid test: %s "
                              "retired a record", refused[i].label);
                goto done;
            }

            if (nxt_slow_path(!nxt_queue_is_empty(&router_port->messages))) {
                sent = nxt_queue_link_data(
                                        nxt_queue_first(&router_port->messages),
                                        nxt_port_send_msg_t, link);

                nxt_log_alert(thr->log, "main remove child pid test: %s made "
                              "the router a message of type %d",
                              refused[i].label, (int) sent->port_msg.type);
                goto done;
            }
        }
    }

    nxt_queue_remove(&other->link);
    nxt_queue_insert_tail(&proto->children, &other->link);

    /* A name equal to the global pid is kept. */

    nxt_main_test_run_name_child(task, proto, twin, twin_pid);

    if (nxt_slow_path(twin->parent_ns_pid != twin_pid)) {
        nxt_log_alert(thr->log, "main remove child pid test: a worker whose "
                      "two pids are equal was left unnamed");
        goto done;
    }

    /* Accepted: the record goes and the router is told by the global pid. */

    ret = nxt_main_rcp_report(thr, task, mp, proto_pid, twin_pid, 1);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = NXT_ERROR;
    twin_port = NULL;

    if (nxt_slow_path(nxt_runtime_process_find(rt, twin_pid) != NULL)) {
        nxt_log_alert(thr->log, "main remove child pid test: a worker whose "
                      "two pids are equal was not retired");
        goto done;
    }

    if (nxt_slow_path(nxt_main_rcp_router_told(thr, router_port, twin_pid,
                                               "equal pids") != NXT_OK))
    {
        goto done;
    }

    ret = nxt_main_rcp_report(thr, task, mp, proto_pid, NXT_MAIN_RCP_NS_PID,
                              1);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = NXT_ERROR;
    child_port = NULL;

    if (nxt_slow_path(nxt_runtime_process_find(rt, child_pid) != NULL)) {
        nxt_log_alert(thr->log, "main remove child pid test: the record of a "
                      "reported worker outlived the report");
        goto done;
    }

    if (nxt_slow_path(nxt_runtime_process_find(rt, other_pid) != other)) {
        nxt_log_alert(thr->log, "main remove child pid test: the report took "
                      "a sibling's record with it");
        goto done;
    }

    if (nxt_slow_path(nxt_main_rcp_router_told(thr, router_port, child_pid,
                                               "a reported worker") != NXT_OK))
    {
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "main remove child pid test passed");

    ret = NXT_OK;

done:

    /*
     * The ports carry pools of their own; nxt_port_close() is a no-op on the
     * -1 descriptors and nxt_runtime_port_remove() drops the last reference.
     * nxt_runtime_process_free() unlinks a child still in a children queue.
     */

    if (router_port != NULL) {
        nxt_port_test_run_error_handler(task, router_port);
    }

    nxt_main_rcp_port_release(task, router_port);
    nxt_main_rcp_port_release(task, child_port);
    nxt_main_rcp_port_release(task, twin_port);
    nxt_main_rcp_port_release(task, other_port);
    nxt_main_rcp_port_release(task, proto_port);

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);

    nxt_thread_mutex_destroy(&rt->processes_mutex);

    nxt_mp_destroy(mp);

    return ret;
}

#else

nxt_int_t
nxt_main_remove_child_pid_test(nxt_thread_t *thr)
{
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main remove child pid test "
                  "skipped: no sender credentials on this platform");

    return NXT_OK;
}

#endif
