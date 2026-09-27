/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Test for the sender checks on the main port of the router
 * (src/nxt_router.c, issue #341).  The router accepts these message types
 * only from these senders:
 *
 *   QUIT, CHANGE_FILE, ACCESS_LOG      main
 *   REMOVE_PID                         main or a registered prototype
 *   DATA, APP_RESTART, STATUS          the controller
 *
 * The test sends each type through the handler table of the router from
 * each sender: main, the controller, a prototype, a worker, an unknown pid
 * and no credential.  A refused message must add one to the refusal
 * counter, close its two descriptors, and not run the handler.  The fixture
 * makes a handler that runs crash: nxt_router is NULL, and the reply port
 * that the message names is registered.
 *
 * An accepted message must not change the counter.  The test calls the
 * check of each type directly.  It also sends CHANGE_FILE, DATA, APP_RESTART
 * and STATUS through the table: their bodies find no payload or no reply
 * port, log that, and return.  QUIT, ACCESS_LOG and REMOVE_PID are not sent
 * from their sender, because their bodies stop the process or need a
 * started router.
 *
 * A second pass runs with no main port and no controller port, as in a
 * restarted router before NEW_PORT.  Only a prototype passes, with
 * REMOVE_PID.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_main_process.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"

#include <fcntl.h>


#if (NXT_USE_CMSG_PID)

/* The senders.  The first four have a port; NONE sends no credential. */

enum {
    NXT_ROUTER_SENDER_TEST_MAIN = 0,
    NXT_ROUTER_SENDER_TEST_CONTROLLER,
    NXT_ROUTER_SENDER_TEST_PROTO,
    NXT_ROUTER_SENDER_TEST_WORKER,
    NXT_ROUTER_SENDER_TEST_STRANGER,
    NXT_ROUTER_SENDER_TEST_NONE,
    NXT_ROUTER_SENDER_TEST_PORTS = NXT_ROUTER_SENDER_TEST_STRANGER,
    NXT_ROUTER_SENDER_TEST_SENDERS = NXT_ROUTER_SENDER_TEST_NONE + 1
};


static const char  *nxt_router_sender_test_names[] = {
    "main", "the controller", "a prototype", "a worker", "a stranger",
    "no credential",
};


static const nxt_process_type_t  nxt_router_sender_test_port_types[] = {
    NXT_PROCESS_MAIN, NXT_PROCESS_CONTROLLER, NXT_PROCESS_PROTOTYPE,
    NXT_PROCESS_APP,
};


typedef struct {
    const char          *name;
    nxt_uint_t          type;
    /* The expected sender; NXT_PROCESS_PROTOTYPE means main or a prototype. */
    nxt_process_type_t  check;
    /* The body is safe to run from the correct sender. */
    nxt_bool_t          run;
} nxt_router_sender_test_type_t;


static const nxt_router_sender_test_type_t  nxt_router_sender_test_types[] = {
    { "QUIT",        _NXT_PORT_MSG_QUIT,        NXT_PROCESS_MAIN,       0 },
    { "CHANGE_FILE", _NXT_PORT_MSG_CHANGE_FILE, NXT_PROCESS_MAIN,       1 },
    { "ACCESS_LOG",  _NXT_PORT_MSG_ACCESS_LOG,  NXT_PROCESS_MAIN,       0 },
    { "REMOVE_PID",  _NXT_PORT_MSG_REMOVE_PID,  NXT_PROCESS_PROTOTYPE,  0 },
    { "DATA",        _NXT_PORT_MSG_DATA,        NXT_PROCESS_CONTROLLER, 1 },
    { "APP_RESTART", _NXT_PORT_MSG_APP_RESTART, NXT_PROCESS_CONTROLLER, 1 },
    { "STATUS",      _NXT_PORT_MSG_STATUS,      NXT_PROCESS_CONTROLLER, 1 },
};


static nxt_pid_t
nxt_router_sender_test_pid(nxt_uint_t sender)
{
    if (sender == NXT_ROUTER_SENDER_TEST_NONE) {
        return -1;
    }

    return nxt_pid + 1 + sender;
}


/* Whether the check accepts the sender; known: the router has both ports. */

static nxt_bool_t
nxt_router_sender_test_accepts(nxt_process_type_t check, nxt_uint_t sender,
    nxt_bool_t known)
{
    switch (check) {

    case NXT_PROCESS_MAIN:
        return (known && sender == NXT_ROUTER_SENDER_TEST_MAIN);

    case NXT_PROCESS_CONTROLLER:
        return (known && sender == NXT_ROUTER_SENDER_TEST_CONTROLLER);

    default:
        return (sender == NXT_ROUTER_SENDER_TEST_PROTO
                || (known && sender == NXT_ROUTER_SENDER_TEST_MAIN));
    }
}


/* A message from "sender", with the pid of "claimed" in the header. */

static void
nxt_router_sender_test_msg(nxt_port_recv_msg_t *msg, nxt_uint_t type,
    nxt_uint_t claimed, nxt_uint_t sender)
{
    nxt_memzero(msg, sizeof(nxt_port_recv_msg_t));

    msg->fd[0] = -1;
    msg->fd[1] = -1;
    msg->port_msg.pid = nxt_router_sender_test_pid(claimed);
    msg->port_msg.reply_port = 1;
    msg->port_msg.type = type;
    msg->port_msg.last = 1;
    msg->cmsg_pid = nxt_router_sender_test_pid(sender);
}


/* As nxt_port_handler() does, through the table of the router. */

static void
nxt_router_sender_test_dispatch(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_port_handler_t  *handlers;

    handlers = (nxt_port_handler_t *) nxt_router_process.port_handlers;

    handlers[msg->port_msg.type](task, msg);
}


static nxt_int_t
nxt_router_sender_test_refused(nxt_thread_t *thr, nxt_task_t *task,
    const nxt_router_sender_test_type_t *t, nxt_uint_t sender)
{
    nxt_fd_t             fd[2];
    nxt_uint_t           i, refused;
    const char           *err;
    nxt_port_recv_msg_t  msg;

    /*
     * The header names the controller, so a DATA, APP_RESTART or STATUS
     * body that runs finds the reply port and crashes on nxt_router.
     */
    nxt_router_sender_test_msg(&msg, t->type, NXT_ROUTER_SENDER_TEST_CONTROLLER,
                               sender);

    for (i = 0; i < 2; i++) {
        fd[i] = open("/dev/null", O_RDONLY);
        msg.fd[i] = fd[i];
    }

    err = NULL;

    if (fd[0] == -1 || fd[1] == -1) {
        err = "failed to open /dev/null";
        goto done;
    }

    refused = nxt_router_test_senders_refused;

    nxt_router_sender_test_dispatch(task, &msg);

    if (nxt_router_test_senders_refused != refused + 1) {
        err = "was not refused";

    } else if (nxt_test_fd_is_open(fd[0]) || nxt_test_fd_is_open(fd[1])) {
        err = "left a descriptor open";

    } else if (msg.fd[0] != -1 || msg.fd[1] != -1) {
        err = "left fds in the message";
    }

done:

    for (i = 0; i < 2; i++) {
        if (fd[i] != -1 && nxt_test_fd_is_open(fd[i])) {
            (void) close(fd[i]);
        }
    }

    if (err != NULL) {
        nxt_log_alert(thr->log, "router sender test: %s from %s %s", t->name,
                      nxt_router_sender_test_names[sender], err);
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_router_sender_test_accepted(nxt_thread_t *thr, nxt_task_t *task,
    const nxt_router_sender_test_type_t *t, nxt_uint_t sender)
{
    nxt_bool_t           res;
    nxt_uint_t           refused;
    const char           *who;
    nxt_port_recv_msg_t  msg;

    who = nxt_router_sender_test_names[sender];

    refused = nxt_router_test_senders_refused;

    nxt_router_sender_test_msg(&msg, t->type, sender, sender);

    res = nxt_router_test_msg_sender_ok(task, &msg);

    NXT_TEST_CHECK(thr->log, res, "router sender test: %s from %s: check "
                   "returned 0", t->name, who);

    if (t->run) {
        /* The header names a stranger: the body finds no reply port. */
        nxt_router_sender_test_msg(&msg, t->type,
                                   NXT_ROUTER_SENDER_TEST_STRANGER, sender);

        nxt_router_sender_test_dispatch(task, &msg);
    }

    NXT_TEST_CHECK(thr->log, nxt_router_test_senders_refused == refused,
                   "router sender test: %s from %s was refused", t->name,
                   who);

    return NXT_OK;
}


static nxt_int_t
nxt_router_sender_test_run(nxt_thread_t *thr, nxt_task_t *task,
    nxt_bool_t known)
{
    nxt_int_t   ret;
    nxt_uint_t  i, s;

    const nxt_router_sender_test_type_t  *t;

    for (i = 0; i < nxt_nitems(nxt_router_sender_test_types); i++) {
        t = &nxt_router_sender_test_types[i];

        for (s = 0; s < NXT_ROUTER_SENDER_TEST_SENDERS; s++) {
            if (nxt_router_sender_test_accepts(t->check, s, known)) {
                ret = nxt_router_sender_test_accepted(thr, task, t, s);

            } else {
                ret = nxt_router_sender_test_refused(thr, task, t, s);
            }

            if (ret != NXT_OK) {
                return ret;
            }
        }
    }

    return NXT_OK;
}


static nxt_port_t *
nxt_router_sender_test_port(nxt_task_t *task, nxt_port_id_t id,
    nxt_uint_t sender, nxt_process_type_t type)
{
    nxt_port_t  *port;

    port = nxt_port_new(task, id, nxt_router_sender_test_pid(sender), type);

    if (nxt_fast_path(port != NULL)) {
        port->pair[0] = -1;
        port->pair[1] = -1;
        port->socket.fd = -1;
    }

    return port;
}


/* A registered process with one port, as the NEW_PORT of main leaves it. */

static nxt_port_t *
nxt_router_sender_test_process(nxt_task_t *task, nxt_mp_t *mp,
    nxt_uint_t sender, nxt_process_type_t type, nxt_process_t **processp)
{
    nxt_port_t     *port;
    nxt_process_t  *process;

    process = nxt_mp_zalloc(mp, sizeof(nxt_process_t));
    if (nxt_slow_path(process == NULL)) {
        return NULL;
    }

    process->pid = nxt_router_sender_test_pid(sender);
    process->isolated_pid = process->pid;
    process->state = NXT_PROCESS_STATE_READY;
    nxt_queue_init(&process->ports);

    nxt_runtime_process_add(task, process);

    *processp = process;

    port = nxt_router_sender_test_port(task, 0, sender, type);
    if (nxt_slow_path(port == NULL)) {
        return NULL;
    }

    /* The hold of the fixture on the record. */
    process->use_count = 1;

    nxt_process_port_add(task, process, port);

    return port;
}

#endif


nxt_int_t
nxt_router_sender_test(nxt_thread_t *thr)
{
#if !(NXT_USE_CMSG_PID)

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router sender test skipped: no sender credentials");
    return NXT_OK;

#else

    nxt_mp_t            *mp;
    nxt_int_t           ret;
    nxt_uint_t          i;
    nxt_task_t          *task;
    nxt_port_t          *ports[NXT_ROUTER_SENDER_TEST_PORTS], *reply_port;
    nxt_bool_t          mutex, hashed;
    nxt_router_t        *saved_router;
    nxt_runtime_t       *rt, *saved_rt;
    nxt_process_t       *processes[NXT_ROUTER_SENDER_TEST_PORTS];
    nxt_event_engine_t  engine, *saved_engine;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router sender test started");

    ret = NXT_ERROR;
    reply_port = NULL;
    mutex = 0;
    hashed = 0;

    nxt_memzero(ports, sizeof(ports));
    nxt_memzero(processes, sizeof(processes));

    task = thr->task;
    task->thread = thr;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;
    saved_router = nxt_router;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    if (nxt_slow_path(rt == NULL)) {
        goto done;
    }

    rt->mem_pool = mp;

    if (nxt_slow_path(nxt_thread_mutex_create(&rt->processes_mutex) != NXT_OK))
    {
        goto done;
    }

    mutex = 1;

    /* The handlers run on the main engine only. */

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));

    thr->runtime = rt;
    thr->engine = &engine;
    rt->main_engine = &engine;

    /* A body that got past its check would dereference this. */

    nxt_router = NULL;

    for (i = 0; i < NXT_ROUTER_SENDER_TEST_PORTS; i++) {
        if (i < NXT_ROUTER_SENDER_TEST_PROTO) {
            ports[i] = nxt_router_sender_test_port(task, 0, i,
                                         nxt_router_sender_test_port_types[i]);

        } else {
            ports[i] = nxt_router_sender_test_process(task, mp, i,
                                         nxt_router_sender_test_port_types[i],
                                         &processes[i]);
        }

        if (nxt_slow_path(ports[i] == NULL)) {
            goto done;
        }
    }

    /* The reply port that a forged DATA, APP_RESTART or STATUS names. */

    reply_port = nxt_router_sender_test_port(task, 1,
                                             NXT_ROUTER_SENDER_TEST_CONTROLLER,
                                             NXT_PROCESS_CONTROLLER);

    if (nxt_slow_path(reply_port == NULL
                      || nxt_port_hash_add(&rt->ports, reply_port) != NXT_OK))
    {
        goto done;
    }

    hashed = 1;

    rt->port_by_type[NXT_PROCESS_MAIN] = ports[NXT_ROUTER_SENDER_TEST_MAIN];
    rt->port_by_type[NXT_PROCESS_CONTROLLER] =
        ports[NXT_ROUTER_SENDER_TEST_CONTROLLER];

    ret = nxt_router_sender_test_run(thr, task, 1);

    if (ret == NXT_OK) {
        rt->port_by_type[NXT_PROCESS_MAIN] = NULL;
        rt->port_by_type[NXT_PROCESS_CONTROLLER] = NULL;

        ret = nxt_router_sender_test_run(thr, task, 0);
    }

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "router sender test passed");
    }

done:

    if (rt != NULL) {
        rt->port_by_type[NXT_PROCESS_MAIN] = NULL;
        rt->port_by_type[NXT_PROCESS_CONTROLLER] = NULL;
    }

    if (hashed) {
        (void) nxt_port_hash_remove(&rt->ports, reply_port);
    }

    if (reply_port != NULL) {
        nxt_port_use(task, reply_port, -1);
    }

    for (i = 0; i < NXT_ROUTER_SENDER_TEST_PORTS; i++) {
        if (ports[i] != NULL) {
            nxt_port_use(task, ports[i], -1);
        }
    }

    for (i = 0; i < NXT_ROUTER_SENDER_TEST_PORTS; i++) {
        if (processes[i] != NULL && processes[i]->registered) {
            nxt_runtime_process_remove(rt, processes[i]);
        }
    }

    nxt_router = saved_router;
    thr->engine = saved_engine;
    thr->runtime = saved_rt;

    if (mutex) {
        nxt_thread_mutex_destroy(&rt->processes_mutex);
    }

    nxt_mp_destroy(mp);

    return ret;

#endif
}
