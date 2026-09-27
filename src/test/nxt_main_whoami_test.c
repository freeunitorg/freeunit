/*
 * Copyright (C) FreeUnit Community
 */

/*
 * The WHOAMI handler in main must refuse a message whose shape does not
 * match how the sender was started.  "ppid" comes from the sender, which is
 * not trusted.
 *
 *   - a worker already recorded sends WHOAMI again, with or without a port:
 *     before the fix it got a second port and was linked into its parent's
 *     "children" twice, which corrupts the queue;
 *   - a process names a parent that is not a prototype;
 *   - a process names main as its parent and sends a port.
 *
 * Each refused message must leave the records as they were and close the
 * descriptor it carried.  The accepted shapes run in every pytest that
 * starts an application.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_main_process.h>
#include "nxt_tests.h"


typedef struct {
    nxt_pid_t   pid;
    nxt_pid_t   ppid;
    nxt_bool_t  with_fd;
    const char  *label;
} nxt_main_whoami_case_t;


static nxt_port_t *
nxt_main_whoami_port(nxt_task_t *task, nxt_runtime_t *rt, nxt_pid_t pid,
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
nxt_main_whoami_port_release(nxt_task_t *task, nxt_port_t *port)
{
    if (port != NULL) {
        nxt_port_close(task, port);
        nxt_runtime_port_remove(task, port);
    }
}


static nxt_uint_t
nxt_main_whoami_count(nxt_queue_t *queue)
{
    nxt_uint_t        n;
    nxt_queue_link_t  *lnk;

    n = 0;

    for (lnk = nxt_queue_first(queue);
         lnk != nxt_queue_tail(queue) && n < 100;
         lnk = nxt_queue_next(lnk))
    {
        n++;
    }

    return n;
}


static nxt_int_t
nxt_main_whoami_refused(nxt_thread_t *thr, nxt_mp_t *mp, nxt_runtime_t *rt,
    nxt_process_t *proto, nxt_process_t *child, nxt_main_whoami_case_t *c)
{
    int                  pair[2];
    nxt_buf_t            *buf;
    nxt_process_t        *process;
    nxt_port_recv_msg_t  msg;

    pair[0] = -1;
    pair[1] = -1;

    if (c->with_fd && socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0) {
        nxt_log_alert(thr->log, "main whoami test: socketpair() failed");
        return NXT_ERROR;
    }

    buf = nxt_buf_mem_alloc(mp, sizeof(nxt_pid_t), 0);
    if (nxt_slow_path(buf == NULL)) {
        return NXT_ERROR;
    }

    buf->mem.free = nxt_cpymem(buf->mem.free, &c->ppid, sizeof(nxt_pid_t));

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.buf = buf;
    msg.fd[0] = pair[0];
    msg.fd[1] = -1;
    msg.port_msg.type = _NXT_PORT_MSG_WHOAMI;
    msg.port_msg.pid = c->pid;
#if (NXT_USE_CMSG_PID)
    msg.cmsg_pid = c->pid;
#endif

    nxt_main_test_run_whoami_handler(thr->task, &msg);

    nxt_mp_free(mp, buf);

    if (pair[1] != -1) {
        nxt_fd_close(pair[1]);
    }

    if (c->with_fd && nxt_test_fd_is_open(pair[0])) {
        nxt_log_alert(thr->log, "main whoami test: %s: the descriptor was "
                      "kept", c->label);
        nxt_fd_close(pair[0]);
        return NXT_ERROR;
    }

    if (nxt_main_whoami_count(&proto->children) != 1
        || nxt_main_whoami_count(&child->ports) != 1)
    {
        nxt_log_alert(thr->log, "main whoami test: %s: %ui children, %ui "
                      "ports of the worker; expected 1 and 1", c->label,
                      nxt_main_whoami_count(&proto->children),
                      nxt_main_whoami_count(&child->ports));
        return NXT_ERROR;
    }

    if (c->pid != child->pid) {
        process = nxt_runtime_process_find(rt, c->pid);

        if (process != NULL) {
            nxt_log_alert(thr->log, "main whoami test: %s: a record was "
                          "created", c->label);
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


nxt_int_t
nxt_main_whoami_test(nxt_thread_t *thr)
{
    nxt_mp_t                *mp;
    nxt_int_t               ret;
    nxt_pid_t               proto_pid, child_pid, router_pid, new_pid;
    nxt_uint_t              i;
    nxt_port_t              *main_port, *router_port, *proto_port;
    nxt_port_t              *child_port;
    nxt_runtime_t           *rt, *saved_rt;
    nxt_process_t           *proto, *child;
    nxt_main_whoami_case_t  cases[4];

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main whoami test started");

    ret = NXT_ERROR;
    main_port = NULL;
    router_port = NULL;
    proto_port = NULL;
    child_port = NULL;

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

    saved_rt = thr->runtime;
    thr->runtime = rt;

    proto_pid = nxt_pid + 1;
    child_pid = nxt_pid + 2;
    router_pid = nxt_pid + 3;
    new_pid = nxt_pid + 4;

    main_port = nxt_main_whoami_port(thr->task, rt, nxt_pid,
                                     NXT_PROCESS_MAIN);
    router_port = nxt_main_whoami_port(thr->task, rt, router_pid,
                                       NXT_PROCESS_ROUTER);
    proto_port = nxt_main_whoami_port(thr->task, rt, proto_pid,
                                      NXT_PROCESS_PROTOTYPE);
    child_port = nxt_main_whoami_port(thr->task, rt, child_pid,
                                      NXT_PROCESS_APP);

    if (nxt_slow_path(main_port == NULL || router_port == NULL
                      || proto_port == NULL || child_port == NULL))
    {
        goto done;
    }

    proto = proto_port->process;
    child = child_port->process;

    /* As an accepted WHOAMI leaves a worker. */
    nxt_queue_insert_tail(&proto->children, &child->link);

    cases[0] = (nxt_main_whoami_case_t) {
        child_pid, proto_pid, 1, "second WHOAMI with a port" };
    cases[1] = (nxt_main_whoami_case_t) {
        child_pid, proto_pid, 0, "second WHOAMI without a port" };
    cases[2] = (nxt_main_whoami_case_t) {
        new_pid, router_pid, 1, "parent is not a prototype" };
    cases[3] = (nxt_main_whoami_case_t) {
        new_pid, nxt_pid, 1, "child of main sends a port" };

    for (i = 0; i < nxt_nitems(cases); i++) {
        if (nxt_main_whoami_refused(thr, mp, rt, proto, child, &cases[i])
            != NXT_OK)
        {
            goto done;
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main whoami test passed");

    ret = NXT_OK;

done:

    nxt_main_whoami_port_release(thr->task, child_port);
    nxt_main_whoami_port_release(thr->task, proto_port);
    nxt_main_whoami_port_release(thr->task, router_port);
    nxt_main_whoami_port_release(thr->task, main_port);

    thr->runtime = saved_rt;

    nxt_thread_mutex_destroy(&rt->processes_mutex);

    nxt_mp_destroy(mp);

    return ret;
}
