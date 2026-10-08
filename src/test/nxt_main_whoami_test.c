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
 * descriptor it carried.  A refused message that carried a descriptor must
 * also be answered with RPC_ERROR on it.  Before that answer, the sender
 * waited for a reply that never came: it holds its own copy of the socket,
 * so main closing its copy does not wake it.
 *
 * A record that is still linked into "children" but has no port must be
 * refused too, or it would be linked a second time.
 *
 * The accepted worker shape must take the descriptor (msg->fd[0] set to -1,
 * so the exit path does not close it) and write one reply on it.
 * If that reply cannot be sent, main must send RPC_ERROR on the port it
 * took, so that the worker does not wait for ever.
 *
 * The answer goes only to a socket that the named prototype created.  An
 * application holds the router's port, and before that check main wrote
 * RPC_ERROR there, which passes the router's sender checks.  So the test
 * runs in a child whose pid is the prototype's, and the router's socket is
 * created before the fork, by "main".
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_main_process.h>
#include "nxt_tests.h"


#if (NXT_HAVE_AF_UNIX_SOCK_SEQPACKET)
#define NXT_MAIN_WHOAMI_SOCKET  SOCK_SEQPACKET
#else
#define NXT_MAIN_WHOAMI_SOCKET  SOCK_DGRAM
#endif

#define NXT_MAIN_WHOAMI_STREAM  0x5a5a5a5a

/* Without SO_PEERCRED main cannot check the socket, so it never answers. */
#if (NXT_HAVE_UCRED)
#define NXT_MAIN_WHOAMI_ANSWERS  1
#else
#define NXT_MAIN_WHOAMI_ANSWERS  0
#endif


typedef struct {
    nxt_pid_t   pid;
    nxt_pid_t   ppid;
    nxt_bool_t  with_fd;
    nxt_bool_t  answered;
    const char  *label;
} nxt_main_whoami_case_t;


typedef struct {
    nxt_thread_t   *thr;
    nxt_mp_t       *mp;
    nxt_runtime_t  *rt;
    nxt_process_t  *proto;
    nxt_process_t  *child;
    nxt_pid_t      pid;
    int            pair[2];
} nxt_main_whoami_stale_t;


typedef struct {
    nxt_thread_t  *thr;
    int           router[2];
} nxt_main_whoami_run_t;


static nxt_int_t nxt_main_whoami_send(nxt_thread_t *thr, nxt_mp_t *mp,
    nxt_pid_t pid, nxt_pid_t ppid, nxt_fd_t fd, nxt_port_recv_msg_t *msg);


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


/*
 * Reads what main wrote to the sender's end of the socket.  The sender
 * still holds its own copy of the end it sent, so closing main's copy gives
 * no EOF.  A read that would block means main wrote nothing, and a real
 * sender would wait for ever.
 */

static nxt_int_t
nxt_main_whoami_reply(nxt_thread_t *thr, nxt_fd_t fd, nxt_uint_t type,
    const char *label)
{
    ssize_t         n;
    nxt_err_t       err;
    nxt_port_msg_t  *pm;
    union {
        nxt_port_msg_t  pm;
        u_char          buf[sizeof(nxt_port_msg_t) + sizeof(nxt_pid_t) + 16];
    } in;

    n = recv(fd, in.buf, sizeof(in.buf), MSG_DONTWAIT);
    err = (n == -1) ? nxt_errno : 0;

    if (n < (ssize_t) sizeof(nxt_port_msg_t)) {
        nxt_log_alert(thr->log, "main whoami test: %s: the sender got no "
                      "reply: recv() returned %z %E", label, n, err);
        return NXT_ERROR;
    }

    pm = &in.pm;

    if (pm->type != type || pm->stream != NXT_MAIN_WHOAMI_STREAM
        || pm->last != 1)
    {
        nxt_log_alert(thr->log, "main whoami test: %s: reply type %d, stream "
                      "%uD, last %d; expected %ui, %uD, 1", label,
                      (int) pm->type, pm->stream, (int) pm->last, type,
                      (uint32_t) NXT_MAIN_WHOAMI_STREAM);
        return NXT_ERROR;
    }

    if (type == _NXT_PORT_MSG_RPC_READY
        && n != (ssize_t) (sizeof(nxt_port_msg_t) + sizeof(nxt_pid_t)))
    {
        nxt_log_alert(thr->log, "main whoami test: %s: reply of %z bytes",
                      label, n);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/* Main must have written nothing to the socket. */

static nxt_int_t
nxt_main_whoami_silent(nxt_thread_t *thr, nxt_fd_t fd, const char *label)
{
    ssize_t         n;
    nxt_port_msg_t  pm;

    nxt_err_t       err;

    n = recv(fd, &pm, sizeof(nxt_port_msg_t), MSG_DONTWAIT);

    if (n != -1) {
        nxt_log_alert(thr->log, "main whoami test: %s: main wrote %z bytes, "
                      "type %d, stream %uD; expected nothing", label, n,
                      (int) pm.type, pm.stream);
        return NXT_ERROR;
    }

    /* Only an empty socket counts; a bad descriptor fails too. */
    err = nxt_errno;

    if (err != NXT_EAGAIN) {
        nxt_log_alert(thr->log, "main whoami test: %s: recv() failed %E",
                      label, err);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * An application sends WHOAMI again with its copy of the router's port.
 * Main must not write RPC_ERROR there: the router would take it as main's
 * and fail the RPC that the stream names.
 */

static nxt_int_t
nxt_main_whoami_router_fd(nxt_thread_t *thr, nxt_mp_t *mp, nxt_pid_t pid,
    nxt_pid_t ppid, int router[2])
{
    int                  fd;
    nxt_int_t            ret;
    nxt_port_recv_msg_t  msg;

    fd = dup(router[1]);
    if (fd == -1) {
        nxt_log_alert(thr->log, "main whoami test: dup() failed");
        return NXT_ERROR;
    }

    if (nxt_main_whoami_send(thr, mp, pid, ppid, fd, &msg) != NXT_OK) {
        nxt_fd_close(fd);
        return NXT_ERROR;
    }

    ret = nxt_main_whoami_silent(thr, router[0], "router's port attached");

    if (nxt_test_fd_is_open(fd)) {
        nxt_log_alert(thr->log, "main whoami test: router's port attached: "
                      "the descriptor was kept");
        nxt_fd_close(fd);
        ret = NXT_ERROR;
    }

    return ret;
}


static nxt_int_t
nxt_main_whoami_send(nxt_thread_t *thr, nxt_mp_t *mp, nxt_pid_t pid,
    nxt_pid_t ppid, nxt_fd_t fd, nxt_port_recv_msg_t *msg)
{
    nxt_buf_t  *buf;

    buf = nxt_buf_mem_alloc(mp, sizeof(nxt_pid_t), 0);
    if (nxt_slow_path(buf == NULL)) {
        return NXT_ERROR;
    }

    buf->mem.free = nxt_cpymem(buf->mem.free, &ppid, sizeof(nxt_pid_t));

    nxt_memzero(msg, sizeof(nxt_port_recv_msg_t));

    msg->buf = buf;
    msg->fd[0] = fd;
    msg->fd[1] = -1;
    msg->port_msg.type = _NXT_PORT_MSG_WHOAMI;
    msg->port_msg.pid = pid;
    msg->port_msg.stream = NXT_MAIN_WHOAMI_STREAM;
#if (NXT_USE_CMSG_PID)
    msg->cmsg_pid = pid;
#endif

    nxt_main_test_run_whoami_handler(thr->task, msg);

    nxt_mp_free(mp, buf);

    return NXT_OK;
}


/* A new worker of the prototype: the shape every pytest app start uses. */

static nxt_int_t
nxt_main_whoami_accepted(nxt_thread_t *thr, nxt_mp_t *mp, nxt_runtime_t *rt,
    nxt_process_t *proto, nxt_pid_t pid, nxt_bool_t ready_fails)
{
    const char           *label;
    int                  pair[2];
    u_char               n;
    nxt_int_t            ret;
    nxt_port_t           *port;
    nxt_port_recv_msg_t  msg;

    if (socketpair(AF_UNIX, NXT_MAIN_WHOAMI_SOCKET, 0, pair) != 0) {
        nxt_log_alert(thr->log, "main whoami test: socketpair() failed");
        return NXT_ERROR;
    }

    label = ready_fails ? "accepted, RPC_READY failed" : "accepted";

    /*
     * If RPC_READY cannot be sent, main owns the worker's port, and it must
     * send RPC_ERROR there.  Before, the worker waited for ever.
     */
    nxt_main_test_whoami_ready_failures(ready_fails ? 1 : 0);

    ret = nxt_main_whoami_send(thr, mp, pid, proto->pid, pair[0], &msg);

    nxt_main_test_whoami_ready_failures(0);

    /*
     * The port owns the descriptor now.  If the handler had left it in
     * msg->fd[0], its exit path would have closed it under the port.
     */
    if (ret == NXT_OK && !nxt_test_fd_is_open(pair[0])) {
        nxt_log_alert(thr->log, "main whoami test: %s: the port's "
                      "descriptor %d was closed", label, pair[0]);
        ret = NXT_ERROR;
    }

    if (ret == NXT_OK) {
        if (!ready_fails) {
            ret = nxt_main_whoami_reply(thr, pair[1], _NXT_PORT_MSG_RPC_READY,
                                        label);

        } else if (NXT_MAIN_WHOAMI_ANSWERS) {
            ret = nxt_main_whoami_reply(thr, pair[1], _NXT_PORT_MSG_RPC_ERROR,
                                        label);

        } else {
            ret = nxt_main_whoami_silent(thr, pair[1], label);
        }
    }

    if (ret == NXT_OK && recv(pair[1], &n, 1, MSG_DONTWAIT) != -1) {
        nxt_log_alert(thr->log, "main whoami test: %s: a second "
                      "message followed the reply", label);
        ret = NXT_ERROR;
    }

    port = nxt_runtime_port_find(rt, pid, 0);

    if (ret == NXT_OK
        && (port == NULL || port->pair[1] != pair[0]
            || (!ready_fails && nxt_main_whoami_count(&proto->children) != 2)))
    {
        nxt_log_alert(thr->log, "main whoami test: %s: port %p, "
                      "%ui children", label, port,
                      nxt_main_whoami_count(&proto->children));
        ret = NXT_ERROR;
    }

    if (port != NULL) {
        /* Closes pair[0], which the port owns now. */
        nxt_port_close(thr->task, port);
        nxt_runtime_port_remove(thr->task, port);

    } else if (nxt_test_fd_is_open(pair[0])) {
        nxt_fd_close(pair[0]);
    }

    nxt_fd_close(pair[1]);

    return ret;
}


/*
 * A record linked into "children" whose port is gone.  Before the check,
 * the handler reused the record and linked it again: nxt_assert() aborted a
 * debug build, and a release build corrupted the queue.  So this runs in a
 * child process.
 */

static int
nxt_main_whoami_stale(void *data)
{
    int                      pair[2], own;
    nxt_port_t               *port;
    nxt_process_t            *process;
    nxt_port_recv_msg_t      msg;
    nxt_main_whoami_stale_t  *st;

    st = data;

    port = nxt_main_whoami_port(st->thr->task, st->rt, st->pid,
                                NXT_PROCESS_APP);
    if (port == NULL) {
        return 2;
    }

    process = port->process;

    nxt_queue_insert_tail(&st->proto->children, &process->link);

    /* Keep the record after its only port is gone. */
    nxt_process_use(st->thr->task, process, 1);

    nxt_main_whoami_port_release(st->thr->task, port);

    /* Created before the fork, by the prototype. */
    pair[0] = st->pair[0];
    pair[1] = st->pair[1];

    /* The sender keeps its own copy of the end it sends, as a worker. */
    own = dup(pair[0]);
    if (own == -1) {
        return 2;
    }

    if (nxt_main_whoami_send(st->thr, st->mp, st->pid, st->proto->pid,
                             pair[0], &msg)
        != NXT_OK)
    {
        return 2;
    }

    if (NXT_MAIN_WHOAMI_ANSWERS
        ? nxt_main_whoami_reply(st->thr, pair[1], _NXT_PORT_MSG_RPC_ERROR,
                                "linked record without a port") != NXT_OK
        : nxt_main_whoami_silent(st->thr, pair[1],
                                 "linked record without a port") != NXT_OK)
    {
        return 1;
    }

    if (nxt_main_whoami_count(&st->proto->children) != 2
        || !nxt_queue_is_empty(&process->ports))
    {
        nxt_log_alert(st->thr->log, "main whoami test: linked record "
                      "without a port: %ui children, ports %s",
                      nxt_main_whoami_count(&st->proto->children),
                      nxt_queue_is_empty(&process->ports) ? "none" : "added");
        return 1;
    }

    return 0;
}


static nxt_int_t
nxt_main_whoami_refused(nxt_thread_t *thr, nxt_mp_t *mp, nxt_runtime_t *rt,
    nxt_process_t *proto, nxt_process_t *child, nxt_main_whoami_case_t *c)
{
    int                  pair[2], own;
    nxt_buf_t            *buf;
    nxt_int_t            ret;
    nxt_process_t        *process;
    nxt_port_recv_msg_t  msg;

    pair[0] = -1;
    pair[1] = -1;

    own = -1;

    if (c->with_fd) {
        if (socketpair(AF_UNIX, NXT_MAIN_WHOAMI_SOCKET, 0, pair) != 0) {
            nxt_log_alert(thr->log, "main whoami test: socketpair() failed");
            return NXT_ERROR;
        }

        /* The sender keeps its own copy of the end it sends, as a worker. */
        own = dup(pair[0]);
        if (own == -1) {
            nxt_log_alert(thr->log, "main whoami test: dup() failed");
            return NXT_ERROR;
        }
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
    msg.port_msg.stream = NXT_MAIN_WHOAMI_STREAM;
#if (NXT_USE_CMSG_PID)
    msg.cmsg_pid = c->pid;
#endif

    nxt_main_test_run_whoami_handler(thr->task, &msg);

    nxt_mp_free(mp, buf);

    if (pair[1] != -1 && c->answered) {
        ret = nxt_main_whoami_reply(thr, pair[1], _NXT_PORT_MSG_RPC_ERROR,
                                    c->label);
        nxt_fd_close(pair[1]);
        nxt_fd_close(own);

        if (ret != NXT_OK) {
            if (nxt_test_fd_is_open(pair[0])) {
                nxt_fd_close(pair[0]);
            }

            return NXT_ERROR;
        }

    } else if (pair[1] != -1) {
        ret = nxt_main_whoami_silent(thr, pair[1], c->label);
        nxt_fd_close(pair[1]);
        nxt_fd_close(own);

        if (ret != NXT_OK) {
            return NXT_ERROR;
        }
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


static int
nxt_main_whoami_run(void *data)
{
    nxt_mp_t                 *mp;
    nxt_int_t                ret;
    nxt_pid_t                proto_pid, child_pid, router_pid, new_pid;
    nxt_uint_t               i;
    nxt_port_t               *main_port, *router_port, *proto_port;
    nxt_port_t               *child_port;
    nxt_runtime_t            *rt, *saved_rt;
    nxt_process_t            *proto, *child;
    nxt_event_engine_t       engine, *saved_engine;
    nxt_pid_t                base;
    nxt_thread_t             *thr;
    nxt_main_whoami_run_t    *run;
    nxt_main_whoami_case_t   cases[4];
    nxt_main_whoami_stale_t  stale;

    run = data;
    thr = run->thr;

    ret = NXT_ERROR;
    main_port = NULL;
    router_port = NULL;
    proto_port = NULL;
    child_port = NULL;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return 2;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    if (nxt_slow_path(rt == NULL)) {
        nxt_mp_destroy(mp);
        return 2;
    }

    rt->mem_pool = mp;

    if (nxt_slow_path(nxt_thread_mutex_create(&rt->processes_mutex) != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return 2;
    }

    /*
     * The accepted case writes its reply through a port, which needs an
     * engine with a work queue and a memory pool.
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
     * This process is the prototype, so the sockets it creates are the
     * prototype's.  nxt_pid still names the test process, which is "main".
     */
    proto_pid = getpid();
    base = nxt_max(proto_pid, nxt_pid) + 10;
    child_pid = base + 1;
    router_pid = base + 2;
    new_pid = base + 3;

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
        child_pid, proto_pid, 1, NXT_MAIN_WHOAMI_ANSWERS,
        "second WHOAMI with a port" };
    cases[1] = (nxt_main_whoami_case_t) {
        child_pid, proto_pid, 0, 0, "second WHOAMI without a port" };
    cases[2] = (nxt_main_whoami_case_t) {
        new_pid, router_pid, 1, 0, "parent is not a prototype" };
    cases[3] = (nxt_main_whoami_case_t) {
        new_pid, nxt_pid, 1, 0, "child of main sends a port" };

    for (i = 0; i < nxt_nitems(cases); i++) {
        if (nxt_main_whoami_refused(thr, mp, rt, proto, child, &cases[i])
            != NXT_OK)
        {
            goto done;
        }
    }

    if (nxt_main_whoami_router_fd(thr, mp, child_pid, proto_pid, run->router)
        != NXT_OK)
    {
        goto done;
    }

    if (nxt_main_whoami_accepted(thr, mp, rt, proto, new_pid, 0) != NXT_OK) {
        goto done;
    }

    if (nxt_main_whoami_accepted(thr, mp, rt, proto, base + 5, 1) != NXT_OK) {
        goto done;
    }

    stale = (nxt_main_whoami_stale_t) { thr, mp, rt, proto, child,
                                        base + 4, { -1, -1 } };

    if (socketpair(AF_UNIX, NXT_MAIN_WHOAMI_SOCKET, 0, stale.pair) != 0) {
        goto done;
    }

    ret = nxt_test_in_child(thr, "main whoami test: linked record without "
                            "a port", nxt_main_whoami_stale, &stale);

    nxt_fd_close(stale.pair[0]);
    nxt_fd_close(stale.pair[1]);

    if (ret != 0) {
        ret = NXT_ERROR;
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_main_whoami_port_release(thr->task, child_port);
    nxt_main_whoami_port_release(thr->task, proto_port);
    nxt_main_whoami_port_release(thr->task, router_port);
    nxt_main_whoami_port_release(thr->task, main_port);

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_thread_mutex_destroy(&rt->processes_mutex);

    nxt_mp_destroy(mp);

    return (ret == NXT_OK) ? 0 : 1;
}


nxt_int_t
nxt_main_whoami_test(nxt_thread_t *thr)
{
    int                    ret;
    nxt_main_whoami_run_t  run;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main whoami test started");

    run.thr = thr;

    /* Created by "main", as the router's port is. */
    if (socketpair(AF_UNIX, NXT_MAIN_WHOAMI_SOCKET, 0, run.router) != 0) {
        nxt_log_alert(thr->log, "main whoami test: socketpair() failed");
        return NXT_ERROR;
    }

    ret = nxt_test_in_child(thr, "main whoami test", nxt_main_whoami_run,
                            &run);

    nxt_fd_close(run.router[0]);
    nxt_fd_close(run.router[1]);

    if (ret != 0) {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main whoami test passed");

    return NXT_OK;
}
