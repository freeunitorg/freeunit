/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for the sender check in nxt_router_detached_handler()
 * (src/nxt_router.c).
 *
 * Every application worker holds a copy of the router port, and the pid in
 * a port message is chosen by the sender.  A detached edge that named
 * another worker's pid used to be applied to that worker: a forged start
 * pinned it busy together with a reference to its application, and a forged
 * finish let the reaper take a worker that was still running.  The edge is
 * now applied only when the kernel's credential names the same process.
 *
 * The fixture is one application with the main port of one worker.  The port
 * has no socket (pair[1] is -1) and is in no idle queue, so the edges change
 * only ->detached, the count and the application reference, and those are
 * what the test reads.  Each forged edge is followed by the genuine one, which
 * shows that the fixture reaches the accounting at all.
 *
 * It also covers the counting the accounting rests on: two starts and one
 * finish leave the worker detached, the second finish releases it, and a
 * finish that matches no start is refused without disturbing either count.
 * The first such edge logs an alert, which is the expected output; later ones
 * go to the debug log.  Two presets reach the branches the application's
 * edges alone do not: a start while the router holds its own mark, and a
 * start at the maximum count.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


#if (NXT_USE_CMSG_PID)

/* Which credential the edge arrives with. */

#define NXT_ROUTER_DETACHED_TEST_SELF   0
#define NXT_ROUTER_DETACHED_TEST_OTHER  1
#define NXT_ROUTER_DETACHED_TEST_NONE   2


/*
 * The accounting an edge finds.  An edge with a preset starts from it rather
 * than from what the previous edge left, which is how the test reaches the
 * states only the router's own path or a long run of edges would produce.
 */

typedef struct {
    nxt_atomic_uint_t  use_count;
    uint32_t           detached_app;
    uint32_t           detached_router;
    uint32_t           processes;
    uint8_t            detached;
} nxt_router_detached_test_state_t;


typedef struct {
    const nxt_router_detached_test_state_t  *preset;
    const char                              *name;
    uint32_t                                detached_app;
    uint32_t                                processes;
    nxt_atomic_uint_t                       use_count;
    uint8_t                                 state;
    uint8_t                                 sender;
    uint8_t                                 detached;
    uint8_t                                 alerted;
} nxt_router_detached_test_edge_t;


/*
 * What nxt_router_app_abandon() leaves on an idle port: the router's own
 * mark, the detached state it set, and the application reference that state
 * holds.
 */

static const nxt_router_detached_test_state_t
    nxt_router_detached_test_router_mark = { 9, 0, 1, 1, 1 };


/* A worker whose count has reached the maximum. */

static const nxt_router_detached_test_state_t
    nxt_router_detached_test_saturated = { 9, UINT32_MAX, 0, 1, 1 };


#define NXT_RDT_START   NXT_PORT_DETACHED_START
#define NXT_RDT_FINISH  NXT_PORT_DETACHED_FINISH
#define NXT_RDT_SELF    NXT_ROUTER_DETACHED_TEST_SELF
#define NXT_RDT_OTHER   NXT_ROUTER_DETACHED_TEST_OTHER
#define NXT_RDT_NONE    NXT_ROUTER_DETACHED_TEST_NONE


/*
 * The edges, and the accounting each one must leave behind.  The start and
 * the finish are a counted pair: only the first start and the last finish
 * move ->detached, app->detached_processes and the application reference,
 * and a finish that matches no start moves none of them.  A refused edge is
 * an alert the first time on a port only.
 *
 * A start that finds the router's own mark already set changes no state, so
 * the reference it took goes straight back: the state's reference belongs to
 * nxt_router_app_abandoned_settle().  The finish that follows leaves the
 * state to the router too.  A start at the maximum count is refused rather
 * than wrapped to "no detached work".
 */

static const nxt_router_detached_test_edge_t
    nxt_router_detached_test_edges[] =
{
    { NULL, "a forged start",
      0, 0, 8, NXT_RDT_START, NXT_RDT_OTHER, 0, 0 },
    { NULL, "a genuine start",
      1, 1, 9, NXT_RDT_START, NXT_RDT_SELF, 1, 0 },
    { NULL, "a forged finish",
      1, 1, 9, NXT_RDT_FINISH, NXT_RDT_OTHER, 1, 0 },
    { NULL, "a finish with no credential",
      1, 1, 9, NXT_RDT_FINISH, NXT_RDT_NONE, 1, 0 },
    { NULL, "a second start",
      2, 1, 9, NXT_RDT_START, NXT_RDT_SELF, 1, 0 },
    { NULL, "the first of two finishes",
      1, 1, 9, NXT_RDT_FINISH, NXT_RDT_SELF, 1, 0 },
    { NULL, "the last of two finishes",
      0, 0, 8, NXT_RDT_FINISH, NXT_RDT_SELF, 0, 0 },
    { NULL, "an unmatched finish",
      0, 0, 8, NXT_RDT_FINISH, NXT_RDT_SELF, 0, 1 },
    { NULL, "a second unmatched finish",
      0, 0, 8, NXT_RDT_FINISH, NXT_RDT_SELF, 0, 1 },
    { NULL, "a start after an unmatched finish",
      1, 1, 9, NXT_RDT_START, NXT_RDT_SELF, 1, 1 },
    { NULL, "the matching finish",
      0, 0, 8, NXT_RDT_FINISH, NXT_RDT_SELF, 0, 1 },
    { &nxt_router_detached_test_router_mark,
      "a start while the router holds its own mark",
      1, 1, 9, NXT_RDT_START, NXT_RDT_SELF, 1, 1 },
    { NULL, "a finish while the router holds its own mark",
      0, 1, 9, NXT_RDT_FINISH, NXT_RDT_SELF, 1, 1 },
    { &nxt_router_detached_test_saturated, "a start at the maximum count",
      UINT32_MAX, 1, 9, NXT_RDT_START, NXT_RDT_SELF, 1, 1 },
    { NULL, "a finish below the maximum count",
      UINT32_MAX - 1, 1, 9, NXT_RDT_FINISH, NXT_RDT_SELF, 1, 1 },
};


static nxt_int_t
nxt_router_detached_test_edge(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_t *port, nxt_app_t *app,
    const nxt_router_detached_test_edge_t *e, nxt_pid_t sender)
{
    uint8_t              state;
    nxt_buf_t            b;
    nxt_port_recv_msg_t  msg;

    state = e->state;

    nxt_memzero(&b, sizeof(nxt_buf_t));
    b.mem.start = &state;
    b.mem.pos = &state;
    b.mem.free = &state + 1;
    b.mem.end = &state + 1;

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));
    msg.buf = &b;
    msg.size = 1;
    msg.fd[0] = -1;
    msg.fd[1] = -1;
    msg.port_msg.pid = port->pid;
    msg.port_msg.type = _NXT_PORT_MSG_DETACHED;
    msg.port_msg.last = 1;
    msg.cmsg_pid = sender;

    if (e->preset != NULL) {
        port->detached_app = e->preset->detached_app;
        port->detached_router = e->preset->detached_router;
        port->detached = e->preset->detached;
        app->detached_processes = e->preset->processes;
        app->use_count = e->preset->use_count;
    }

    nxt_router_test_detached_handler(task, &msg);

    if (port->detached_app != e->detached_app
        || port->detached != e->detached
        || port->detached_alerted != e->alerted
        || app->detached_processes != e->processes
        || app->use_count != e->use_count)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router detached test: %s: detached_app %uD, "
                      "detached %d, alerted %d, count %uD, use count %d; "
                      "expected %uD, %d, %d, %uD, %d",
                      e->name, port->detached_app, (int) port->detached,
                      (int) port->detached_alerted, app->detached_processes,
                      (int) app->use_count, e->detached_app,
                      (int) e->detached, (int) e->alerted, e->processes,
                      (int) e->use_count);
        return NXT_ERROR;
    }

    return NXT_OK;
}

#endif


nxt_int_t
nxt_router_detached_test(nxt_thread_t *thr)
{
#if !(NXT_USE_CMSG_PID)

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router detached test skipped: no sender credentials");
    return NXT_OK;

#else

    nxt_mp_t            *mp;
    nxt_app_t           *app;
    nxt_int_t           ret;
    nxt_uint_t          i;
    nxt_pid_t           pid, other, sender;
    nxt_task_t          *task;
    nxt_port_t          *port;
    nxt_bool_t          app_mutex, hashed;
    nxt_runtime_t       *rt, *saved_rt;
    nxt_event_engine_t  engine, *saved_engine;

    const nxt_router_detached_test_edge_t  *e;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router detached test started");

    ret = NXT_ERROR;
    port = NULL;
    app_mutex = 0;
    hashed = 0;

    task = thr->task;
    task->thread = thr;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    app = nxt_mp_zalloc(mp, sizeof(nxt_app_t));
    if (nxt_slow_path(rt == NULL || app == NULL)) {
        goto done;
    }

    rt->mem_pool = mp;

    /* The handler runs on the main engine only. */

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));

    thr->runtime = rt;
    thr->engine = &engine;
    rt->main_engine = &engine;

    if (nxt_slow_path(nxt_thread_mutex_create(&app->mutex) != NXT_OK)) {
        goto done;
    }

    app_mutex = 1;

    nxt_queue_init(&app->ports);
    nxt_queue_init(&app->spare_ports);
    nxt_queue_init(&app->idle_ports);
    nxt_queue_init(&app->ack_waiting_req);

    nxt_str_set(&app->name, "detached-test");

    /* Never reached by the edges below, so nothing frees the application. */
    app->use_count = 8;

    /* A pid that is not this process, so a sender of nxt_pid is foreign. */

    pid = nxt_pid + 1;
    other = nxt_pid;

    port = nxt_port_new(task, 0, pid, NXT_PROCESS_APP);
    if (nxt_slow_path(port == NULL)) {
        goto done;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;
    port->app = app;

    if (nxt_slow_path(nxt_port_hash_add(&rt->ports, port) != NXT_OK)) {
        goto done;
    }

    hashed = 1;

    for (i = 0; i < nxt_nitems(nxt_router_detached_test_edges); i++) {
        e = &nxt_router_detached_test_edges[i];

        switch (e->sender) {
        case NXT_ROUTER_DETACHED_TEST_SELF:
            sender = pid;
            break;

        case NXT_ROUTER_DETACHED_TEST_OTHER:
            sender = other;
            break;

        default:
            sender = -1;
            break;
        }

        if (nxt_slow_path(nxt_router_detached_test_edge(thr, task, port, app,
                                                        e, sender)
                          != NXT_OK))
        {
            goto done;
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router detached test passed");

    ret = NXT_OK;

done:

    if (hashed) {
        (void) nxt_port_hash_remove(&rt->ports, port);
    }

    if (port != NULL) {
        port->app = NULL;
        nxt_port_use(task, port, -1);
    }

    thr->engine = saved_engine;
    thr->runtime = saved_rt;

    if (app_mutex) {
        nxt_thread_mutex_destroy(&app->mutex);
    }

    nxt_mp_destroy(mp);

    return ret;

#endif
}
