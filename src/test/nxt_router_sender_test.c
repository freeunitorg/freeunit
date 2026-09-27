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
 *   RPC_READY, RPC_ERROR               main, the controller or a prototype
 *   GET_PORT, GET_MMAP, MMAP, OOSM     a registered worker, for itself
 *   NEW_PORT                           main; a prototype for an application
 *                                      port; a worker for its own
 *                                      application port, with no stream
 *
 * The test sends each type through the handler table of the router from
 * each sender: main, the controller, a prototype, a worker, an unknown pid
 * and no credential.  A wrong sender sends twice: with its own pid in the
 * header, and with the pid of the controller or of the worker.  A refused
 * message must add one to the refusal counter, close its two descriptors,
 * and not run the handler.  The fixture makes most handlers that run crash:
 * nxt_router is NULL, and the reply ports that the messages name are
 * registered.  Thus the test first calls the check alone, which reports a
 * failure as a message, and then sends the message through the table.
 *
 * An accepted message must not change the counter.  The test calls the
 * check of each type directly.  It also sends CHANGE_FILE, DATA,
 * APP_RESTART, STATUS and MMAP through the table: their bodies find no
 * payload, no reply port or no segment, log that, and return.  The other
 * types are not sent from their sender, because their bodies stop the
 * process, need a started router, or write to a port.
 *
 * A table of cases then sends a correct sender with a wrong body: a
 * NEW_PORT that is not an application port, not the port of the sender,
 * has a stream or is short; a GET_PORT for a port that is not a router
 * port; a GET_MMAP that names a reply port that is not an application
 * port.
 *
 * A second pass runs with no main port and no controller port, as in a
 * restarted router before NEW_PORT.
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

/*
 * The senders.  The first four have a port; NONE sends no credential.
 * ROUTER is not a sender: it owns a port with the pid of this process.
 */

enum {
    NXT_ROUTER_SENDER_TEST_MAIN = 0,
    NXT_ROUTER_SENDER_TEST_CONTROLLER,
    NXT_ROUTER_SENDER_TEST_PROTO,
    NXT_ROUTER_SENDER_TEST_WORKER,
    NXT_ROUTER_SENDER_TEST_STRANGER,
    NXT_ROUTER_SENDER_TEST_NONE,
    NXT_ROUTER_SENDER_TEST_PORTS = NXT_ROUTER_SENDER_TEST_STRANGER,
    NXT_ROUTER_SENDER_TEST_SENDERS = NXT_ROUTER_SENDER_TEST_NONE + 1,
    NXT_ROUTER_SENDER_TEST_ROUTER = NXT_ROUTER_SENDER_TEST_SENDERS,
};


/* The senders that a type accepts. */

enum {
    NXT_ROUTER_SENDER_TEST_FROM_MAIN = 0,
    NXT_ROUTER_SENDER_TEST_FROM_CONTROLLER,
    NXT_ROUTER_SENDER_TEST_FROM_MAIN_OR_PROTO,
    NXT_ROUTER_SENDER_TEST_FROM_NOT_APP,
    NXT_ROUTER_SENDER_TEST_FROM_SELF,
    NXT_ROUTER_SENDER_TEST_FROM_NEW_PORT,
};


/* The changes to a correct message in the table of cases. */

enum {
    NXT_ROUTER_SENDER_TEST_PORT_MAIN = 0,
    NXT_ROUTER_SENDER_TEST_PORT_CONTROLLER,
    NXT_ROUTER_SENDER_TEST_PORT_OF_PROTO,
    NXT_ROUTER_SENDER_TEST_STREAM,
    NXT_ROUTER_SENDER_TEST_SHORT,
    NXT_ROUTER_SENDER_TEST_FOREIGN_PORT,
    NXT_ROUTER_SENDER_TEST_REPLY_NOT_APP,
};


static const char  *nxt_router_sender_test_names[] = {
    "main", "the controller", "a prototype", "a worker", "a stranger",
    "no credential",
};


static const nxt_process_type_t  nxt_router_sender_test_port_types[] = {
    NXT_PROCESS_MAIN, NXT_PROCESS_CONTROLLER, NXT_PROCESS_PROTOTYPE,
    NXT_PROCESS_APP,
};


/* The port ids in the port hash of the fixture. */

#define NXT_ROUTER_SENDER_TEST_REPLY_ID       1
#define NXT_ROUTER_SENDER_TEST_ENGINE_ID      2
#define NXT_ROUTER_SENDER_TEST_NOT_APP_ID     3


typedef struct {
    nxt_uint_t          owner;
    nxt_port_id_t       id;
    nxt_process_type_t  type;
} nxt_router_sender_test_hashed_t;


static const nxt_router_sender_test_hashed_t
    nxt_router_sender_test_hashed[] =
{
    /* The reply port that a forged DATA, APP_RESTART or STATUS names. */
    { NXT_ROUTER_SENDER_TEST_CONTROLLER, NXT_ROUTER_SENDER_TEST_REPLY_ID,
      NXT_PROCESS_CONTROLLER },
    /* The reply port of the worker for GET_PORT and GET_MMAP. */
    { NXT_ROUTER_SENDER_TEST_WORKER, NXT_ROUTER_SENDER_TEST_REPLY_ID,
      NXT_PROCESS_APP },
    /* A router engine port: the only kind that GET_PORT gives out. */
    { NXT_ROUTER_SENDER_TEST_ROUTER, NXT_ROUTER_SENDER_TEST_ENGINE_ID,
      NXT_PROCESS_ROUTER },
    /* A port of another type with the pid of the worker, for GET_MMAP. */
    { NXT_ROUTER_SENDER_TEST_WORKER, NXT_ROUTER_SENDER_TEST_NOT_APP_ID,
      NXT_PROCESS_ROUTER },
};


typedef struct {
    const char  *name;
    nxt_uint_t  type;
    nxt_uint_t  from;
    /* The pid in the header of a message from a wrong sender. */
    nxt_uint_t  claimed;
    /* The body is safe to run from the correct sender. */
    nxt_bool_t  run;
} nxt_router_sender_test_type_t;


static const nxt_router_sender_test_type_t  nxt_router_sender_test_types[] = {
    { "QUIT",        _NXT_PORT_MSG_QUIT,
      NXT_ROUTER_SENDER_TEST_FROM_MAIN,
      NXT_ROUTER_SENDER_TEST_CONTROLLER, 0 },
    { "CHANGE_FILE", _NXT_PORT_MSG_CHANGE_FILE,
      NXT_ROUTER_SENDER_TEST_FROM_MAIN,
      NXT_ROUTER_SENDER_TEST_CONTROLLER, 1 },
    { "ACCESS_LOG",  _NXT_PORT_MSG_ACCESS_LOG,
      NXT_ROUTER_SENDER_TEST_FROM_MAIN,
      NXT_ROUTER_SENDER_TEST_CONTROLLER, 0 },
    { "REMOVE_PID",  _NXT_PORT_MSG_REMOVE_PID,
      NXT_ROUTER_SENDER_TEST_FROM_MAIN_OR_PROTO,
      NXT_ROUTER_SENDER_TEST_CONTROLLER, 0 },
    { "DATA",        _NXT_PORT_MSG_DATA,
      NXT_ROUTER_SENDER_TEST_FROM_CONTROLLER,
      NXT_ROUTER_SENDER_TEST_CONTROLLER, 1 },
    { "APP_RESTART", _NXT_PORT_MSG_APP_RESTART,
      NXT_ROUTER_SENDER_TEST_FROM_CONTROLLER,
      NXT_ROUTER_SENDER_TEST_CONTROLLER, 1 },
    { "STATUS",      _NXT_PORT_MSG_STATUS,
      NXT_ROUTER_SENDER_TEST_FROM_CONTROLLER,
      NXT_ROUTER_SENDER_TEST_CONTROLLER, 1 },
    { "RPC_READY",   _NXT_PORT_MSG_RPC_READY,
      NXT_ROUTER_SENDER_TEST_FROM_NOT_APP,
      NXT_ROUTER_SENDER_TEST_CONTROLLER, 0 },
    { "RPC_ERROR",   _NXT_PORT_MSG_RPC_ERROR,
      NXT_ROUTER_SENDER_TEST_FROM_NOT_APP,
      NXT_ROUTER_SENDER_TEST_CONTROLLER, 0 },
    { "GET_PORT",    _NXT_PORT_MSG_GET_PORT,
      NXT_ROUTER_SENDER_TEST_FROM_SELF,
      NXT_ROUTER_SENDER_TEST_WORKER, 0 },
    { "GET_MMAP",    _NXT_PORT_MSG_GET_MMAP,
      NXT_ROUTER_SENDER_TEST_FROM_SELF,
      NXT_ROUTER_SENDER_TEST_WORKER, 0 },
    { "MMAP",        _NXT_PORT_MSG_MMAP,
      NXT_ROUTER_SENDER_TEST_FROM_SELF,
      NXT_ROUTER_SENDER_TEST_WORKER, 1 },
    { "OOSM",        _NXT_PORT_MSG_OOSM,
      NXT_ROUTER_SENDER_TEST_FROM_SELF,
      NXT_ROUTER_SENDER_TEST_WORKER, 0 },
    { "NEW_PORT",    _NXT_PORT_MSG_NEW_PORT,
      NXT_ROUTER_SENDER_TEST_FROM_NEW_PORT,
      NXT_ROUTER_SENDER_TEST_WORKER, 0 },
};


typedef struct {
    const char  *name;
    nxt_uint_t  type;
    nxt_uint_t  sender;
    nxt_uint_t  change;
} nxt_router_sender_test_case_t;


static const nxt_router_sender_test_case_t  nxt_router_sender_test_cases[] = {
    { "NEW_PORT of type MAIN", _NXT_PORT_MSG_NEW_PORT,
      NXT_ROUTER_SENDER_TEST_WORKER, NXT_ROUTER_SENDER_TEST_PORT_MAIN },
    { "NEW_PORT of type CONTROLLER", _NXT_PORT_MSG_NEW_PORT,
      NXT_ROUTER_SENDER_TEST_PROTO, NXT_ROUTER_SENDER_TEST_PORT_CONTROLLER },
    { "NEW_PORT of type MAIN", _NXT_PORT_MSG_NEW_PORT,
      NXT_ROUTER_SENDER_TEST_PROTO, NXT_ROUTER_SENDER_TEST_PORT_MAIN },
    { "NEW_PORT for the prototype", _NXT_PORT_MSG_NEW_PORT,
      NXT_ROUTER_SENDER_TEST_WORKER, NXT_ROUTER_SENDER_TEST_PORT_OF_PROTO },
    { "NEW_PORT with a stream", _NXT_PORT_MSG_NEW_PORT,
      NXT_ROUTER_SENDER_TEST_WORKER, NXT_ROUTER_SENDER_TEST_STREAM },
    { "short NEW_PORT", _NXT_PORT_MSG_NEW_PORT,
      NXT_ROUTER_SENDER_TEST_WORKER, NXT_ROUTER_SENDER_TEST_SHORT },
    { "short NEW_PORT", _NXT_PORT_MSG_NEW_PORT,
      NXT_ROUTER_SENDER_TEST_PROTO, NXT_ROUTER_SENDER_TEST_SHORT },
    { "GET_PORT for the controller port", _NXT_PORT_MSG_GET_PORT,
      NXT_ROUTER_SENDER_TEST_WORKER, NXT_ROUTER_SENDER_TEST_FOREIGN_PORT },
    { "short GET_PORT", _NXT_PORT_MSG_GET_PORT,
      NXT_ROUTER_SENDER_TEST_WORKER, NXT_ROUTER_SENDER_TEST_SHORT },
    { "GET_MMAP to a port that is not an application port",
      _NXT_PORT_MSG_GET_MMAP,
      NXT_ROUTER_SENDER_TEST_WORKER, NXT_ROUTER_SENDER_TEST_REPLY_NOT_APP },
};


typedef struct {
    nxt_port_recv_msg_t          msg;
    nxt_buf_t                    buf;

    union {
        nxt_port_msg_new_port_t  new_port;
        nxt_port_msg_get_port_t  get_port;
        nxt_port_msg_get_mmap_t  get_mmap;
    } body;
} nxt_router_sender_test_msg_t;


static nxt_pid_t
nxt_router_sender_test_pid(nxt_uint_t sender)
{
    if (sender == NXT_ROUTER_SENDER_TEST_NONE) {
        return -1;
    }

    if (sender == NXT_ROUTER_SENDER_TEST_ROUTER) {
        return nxt_pid;
    }

    return nxt_pid + 1 + sender;
}


/* Whether the type accepts the sender; known: the router has both ports. */

static nxt_bool_t
nxt_router_sender_test_accepts(nxt_uint_t from, nxt_uint_t sender,
    nxt_bool_t known)
{
    nxt_bool_t  main, controller, proto, worker;

    main = (known && sender == NXT_ROUTER_SENDER_TEST_MAIN);
    controller = (known && sender == NXT_ROUTER_SENDER_TEST_CONTROLLER);
    proto = (sender == NXT_ROUTER_SENDER_TEST_PROTO);
    worker = (sender == NXT_ROUTER_SENDER_TEST_WORKER);

    switch (from) {

    case NXT_ROUTER_SENDER_TEST_FROM_MAIN:
        return main;

    case NXT_ROUTER_SENDER_TEST_FROM_CONTROLLER:
        return controller;

    case NXT_ROUTER_SENDER_TEST_FROM_MAIN_OR_PROTO:
        return (main || proto);

    case NXT_ROUTER_SENDER_TEST_FROM_NOT_APP:
        return (main || controller || proto);

    case NXT_ROUTER_SENDER_TEST_FROM_SELF:
        return worker;

    default:
        return (main || proto || worker);
    }
}


/*
 * A message of the type from "sender", with the pid of "claimed" in the
 * header.  NEW_PORT announces an application port of "claimed", GET_PORT
 * asks for the router engine port, and GET_MMAP asks for segment 0.  The
 * reply port of each message has the id NXT_ROUTER_SENDER_TEST_REPLY_ID.
 */

static void
nxt_router_sender_test_msg(nxt_router_sender_test_msg_t *m, nxt_uint_t type,
    nxt_uint_t claimed, nxt_uint_t sender)
{
    size_t               size;
    nxt_port_recv_msg_t  *msg;

    nxt_memzero(m, sizeof(nxt_router_sender_test_msg_t));

    msg = &m->msg;

    msg->fd[0] = -1;
    msg->fd[1] = -1;
    msg->port_msg.pid = nxt_router_sender_test_pid(claimed);
    msg->port_msg.reply_port = NXT_ROUTER_SENDER_TEST_REPLY_ID;
    msg->port_msg.type = type;
    msg->port_msg.last = 1;
    msg->cmsg_pid = nxt_router_sender_test_pid(sender);

    switch (type) {

    case _NXT_PORT_MSG_NEW_PORT:
        m->body.new_port.pid = msg->port_msg.pid;
        m->body.new_port.id = 5;
        m->body.new_port.type = NXT_PROCESS_APP;
        m->body.new_port.max_size = 1024;
        m->body.new_port.max_share = 1024;
        size = sizeof(nxt_port_msg_new_port_t);
        break;

    case _NXT_PORT_MSG_GET_PORT:
        m->body.get_port.pid = nxt_pid;
        m->body.get_port.id = NXT_ROUTER_SENDER_TEST_ENGINE_ID;
        size = sizeof(nxt_port_msg_get_port_t);
        break;

    case _NXT_PORT_MSG_GET_MMAP:
        m->body.get_mmap.id = 0;
        size = sizeof(nxt_port_msg_get_mmap_t);
        break;

    default:
        /* The other types are sent with no payload. */
        return;
    }

    m->buf.mem.start = (u_char *) &m->body;
    m->buf.mem.pos = m->buf.mem.start;
    m->buf.mem.free = m->buf.mem.start + size;
    m->buf.mem.end = m->buf.mem.free;

    msg->buf = &m->buf;
}


static void
nxt_router_sender_test_change(nxt_router_sender_test_msg_t *m,
    nxt_uint_t change)
{
    switch (change) {

    case NXT_ROUTER_SENDER_TEST_PORT_MAIN:
        m->body.new_port.type = NXT_PROCESS_MAIN;
        break;

    case NXT_ROUTER_SENDER_TEST_PORT_CONTROLLER:
        m->body.new_port.type = NXT_PROCESS_CONTROLLER;
        break;

    case NXT_ROUTER_SENDER_TEST_PORT_OF_PROTO:
        m->body.new_port.pid =
            nxt_router_sender_test_pid(NXT_ROUTER_SENDER_TEST_PROTO);
        break;

    case NXT_ROUTER_SENDER_TEST_STREAM:
        m->msg.port_msg.stream = 7;
        break;

    case NXT_ROUTER_SENDER_TEST_SHORT:
        m->buf.mem.free--;
        break;

    case NXT_ROUTER_SENDER_TEST_FOREIGN_PORT:
        m->body.get_port.pid =
            nxt_router_sender_test_pid(NXT_ROUTER_SENDER_TEST_CONTROLLER);
        m->body.get_port.id = NXT_ROUTER_SENDER_TEST_REPLY_ID;
        break;

    default:
        m->msg.port_msg.reply_port = NXT_ROUTER_SENDER_TEST_NOT_APP_ID;
        break;
    }
}


/* As nxt_port_handler() does, through the table of the router. */

static void
nxt_router_sender_test_dispatch(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_port_handler_t  *handlers;

    handlers = (nxt_port_handler_t *) nxt_router_process.port_handlers;

    handlers[msg->port_msg.type](task, msg);
}


/*
 * The check alone must refuse the message.  Then the message goes through
 * the table with two descriptors, and the router must refuse it again and
 * close them.  "m" is changed.
 */

static nxt_int_t
nxt_router_sender_test_refused(nxt_thread_t *thr, nxt_task_t *task,
    nxt_router_sender_test_msg_t *m, const char *name, const char *who)
{
    nxt_fd_t    fd[2];
    nxt_bool_t  res;
    nxt_uint_t  i, refused;
    const char  *err;

    refused = nxt_router_test_senders_refused;

    res = nxt_router_test_msg_sender_ok(task, &m->msg);

    NXT_TEST_CHECK(thr->log, !res, "router sender test: %s from %s: check "
                   "returned 1", name, who);

    NXT_TEST_CHECK(thr->log, nxt_router_test_senders_refused == refused + 1,
                   "router sender test: %s from %s: check did not count a "
                   "refusal", name, who);

    for (i = 0; i < 2; i++) {
        fd[i] = open("/dev/null", O_RDONLY);
        m->msg.fd[i] = fd[i];
    }

    err = NULL;

    if (fd[0] == -1 || fd[1] == -1) {
        err = "failed to open /dev/null";
        goto done;
    }

    refused = nxt_router_test_senders_refused;

    nxt_router_sender_test_dispatch(task, &m->msg);

    if (nxt_router_test_senders_refused != refused + 1) {
        err = "was not refused";

    } else if (nxt_test_fd_is_open(fd[0]) || nxt_test_fd_is_open(fd[1])) {
        err = "left a descriptor open";

    } else if (m->msg.fd[0] != -1 || m->msg.fd[1] != -1) {
        err = "left fds in the message";
    }

done:

    for (i = 0; i < 2; i++) {
        if (fd[i] != -1 && nxt_test_fd_is_open(fd[i])) {
            (void) close(fd[i]);
        }
    }

    if (err != NULL) {
        nxt_log_alert(thr->log, "router sender test: %s from %s %s", name,
                      who, err);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * A wrong sender, twice: with its own pid in the header, and with the pid
 * of the controller or of the worker.  The body of a DATA, APP_RESTART or
 * STATUS that names the controller finds the reply port and crashes on
 * nxt_router.
 */

static nxt_int_t
nxt_router_sender_test_wrong(nxt_thread_t *thr, nxt_task_t *task,
    const nxt_router_sender_test_type_t *t, nxt_uint_t sender)
{
    nxt_int_t                     ret;
    const char                    *who;
    nxt_router_sender_test_msg_t  m;

    who = nxt_router_sender_test_names[sender];

    nxt_router_sender_test_msg(&m, t->type, sender, sender);

    ret = nxt_router_sender_test_refused(thr, task, &m, t->name, who);

    if (ret == NXT_OK && sender != t->claimed) {
        nxt_router_sender_test_msg(&m, t->type, t->claimed, sender);

        ret = nxt_router_sender_test_refused(thr, task, &m, t->name, who);
    }

    return ret;
}


static nxt_int_t
nxt_router_sender_test_accepted(nxt_thread_t *thr, nxt_task_t *task,
    const nxt_router_sender_test_type_t *t, nxt_uint_t sender)
{
    nxt_bool_t                    res;
    nxt_uint_t                    refused, claimed;
    const char                    *who;
    nxt_router_sender_test_msg_t  m;

    who = nxt_router_sender_test_names[sender];

    refused = nxt_router_test_senders_refused;

    nxt_router_sender_test_msg(&m, t->type, sender, sender);

    res = nxt_router_test_msg_sender_ok(task, &m.msg);

    NXT_TEST_CHECK(thr->log, res, "router sender test: %s from %s: check "
                   "returned 0", t->name, who);

    if (t->run) {
        /*
         * The header names a stranger: the body finds no reply port.  A
         * worker must name itself.
         */
        claimed = (t->from == NXT_ROUTER_SENDER_TEST_FROM_SELF)
                  ? sender : NXT_ROUTER_SENDER_TEST_STRANGER;

        nxt_router_sender_test_msg(&m, t->type, claimed, sender);

        nxt_router_sender_test_dispatch(task, &m.msg);
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
    nxt_int_t                     ret;
    nxt_uint_t                    i, s;
    nxt_router_sender_test_msg_t  m;

    const nxt_router_sender_test_case_t  *c;
    const nxt_router_sender_test_type_t  *t;

    for (i = 0; i < nxt_nitems(nxt_router_sender_test_types); i++) {
        t = &nxt_router_sender_test_types[i];

        for (s = 0; s < NXT_ROUTER_SENDER_TEST_SENDERS; s++) {
            if (nxt_router_sender_test_accepts(t->from, s, known)) {
                ret = nxt_router_sender_test_accepted(thr, task, t, s);

            } else {
                ret = nxt_router_sender_test_wrong(thr, task, t, s);
            }

            if (ret != NXT_OK) {
                return ret;
            }
        }
    }

    for (i = 0; i < nxt_nitems(nxt_router_sender_test_cases); i++) {
        c = &nxt_router_sender_test_cases[i];

        nxt_router_sender_test_msg(&m, c->type, c->sender, c->sender);
        nxt_router_sender_test_change(&m, c->change);

        ret = nxt_router_sender_test_refused(thr, task, &m, c->name,
                                      nxt_router_sender_test_names[c->sender]);
        if (ret != NXT_OK) {
            return ret;
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
    nxt_port_t          *port, *ports[NXT_ROUTER_SENDER_TEST_PORTS];
    nxt_port_t          *hashed[nxt_nitems(nxt_router_sender_test_hashed)];
    nxt_bool_t          mutex;
    nxt_router_t        *saved_router;
    nxt_runtime_t       *rt, *saved_rt;
    nxt_process_t       *processes[NXT_ROUTER_SENDER_TEST_PORTS];
    nxt_event_engine_t  engine, *saved_engine;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router sender test started");

    ret = NXT_ERROR;
    mutex = 0;

    nxt_memzero(ports, sizeof(ports));
    nxt_memzero(hashed, sizeof(hashed));
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

    /* The ports that nxt_runtime_port_find() finds. */

    for (i = 0; i < nxt_nitems(nxt_router_sender_test_hashed); i++) {
        port = nxt_router_sender_test_port(task,
                                           nxt_router_sender_test_hashed[i].id,
                                        nxt_router_sender_test_hashed[i].owner,
                                        nxt_router_sender_test_hashed[i].type);
        if (nxt_slow_path(port == NULL)) {
            goto done;
        }

        if (nxt_slow_path(nxt_port_hash_add(&rt->ports, port) != NXT_OK)) {
            nxt_port_use(task, port, -1);
            goto done;
        }

        hashed[i] = port;
    }

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

    for (i = 0; i < nxt_nitems(nxt_router_sender_test_hashed); i++) {
        if (hashed[i] != NULL) {
            (void) nxt_port_hash_remove(&rt->ports, hashed[i]);
            nxt_port_use(task, hashed[i], -1);
        }
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
