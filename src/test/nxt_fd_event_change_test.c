/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


/*
 * A queued fd-event change is held by pointer in the engine's change batch
 * and dereferenced when that batch is committed, at the top of the next poll
 * at the latest.  Nothing else keeps the struct the pointer names alive, so
 * anything that frees such a struct has to drop its pending changes first --
 * nxt_fd_event_cancel_changes().
 *
 * These legs run against the platform's real event engine, not a stub: the
 * batch and its commit are the thing under test.  Each asserts on the
 * engine's change count, so a leg that stops doing what it claims fails
 * rather than passing quietly.  Under a sanitized build the final poll in
 * each leg is the second oracle: it commits whatever is left, so a change
 * that was not dropped is reported as a use-after-free.
 */

/*
 * ->changing is not a cross-engine contract.  The epoll engine keeps it and
 * clears it when it commits, so the legs below can assert on it there.  The
 * kqueue engine does not keep it at all -- its paths work by descriptor or
 * by ->udata -- so on kqueue the change count is the only oracle.
 */

#if (NXT_HAVE_EPOLL)
#define nxt_fd_event_change_test_nchanges(engine)                             \
    ((nxt_uint_t) (engine)->u.epoll.nchanges)
#define NXT_FD_EVENT_CHANGE_TEST  1
#define NXT_FD_EVENT_CHANGE_TEST_CHANGING  1

#elif (NXT_HAVE_KQUEUE)
#define nxt_fd_event_change_test_nchanges(engine)                             \
    ((nxt_uint_t) (engine)->u.kqueue.nchanges)
#define NXT_FD_EVENT_CHANGE_TEST  1
#define NXT_FD_EVENT_CHANGE_TEST_CHANGING  0

#else
#define NXT_FD_EVENT_CHANGE_TEST  0
#define NXT_FD_EVENT_CHANGE_TEST_CHANGING  0
#endif


#if (NXT_FD_EVENT_CHANGE_TEST)

static nxt_event_engine_t *nxt_fd_event_change_test_engine(nxt_task_t *task);
static nxt_int_t nxt_fd_event_change_test_drop(nxt_thread_t *thr,
    nxt_event_engine_t *engine);
static nxt_int_t nxt_fd_event_change_test_keeps_others(nxt_thread_t *thr,
    nxt_event_engine_t *engine);
static nxt_int_t nxt_fd_event_change_test_port(nxt_thread_t *thr,
    nxt_event_engine_t *engine);
static nxt_fd_event_t *nxt_fd_event_change_test_event(nxt_task_t *task,
    nxt_fd_t fd);
static void nxt_fd_event_change_test_handler(nxt_task_t *task, void *obj,
    void *data);


nxt_int_t
nxt_fd_event_change_test(nxt_thread_t *thr)
{
    nxt_int_t           ret;
    nxt_task_t          *task;
    nxt_event_engine_t  *engine, *saved_engine;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "fd event change test started");

    task = thr->task;
    task->thread = thr;

    engine = nxt_fd_event_change_test_engine(task);
    if (nxt_slow_path(engine == NULL)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: no event engine");
        return NXT_ERROR;
    }

    saved_engine = thr->engine;
    thr->engine = engine;

    /*
     * Without eventfd (epoll) or EVFILT_USER (kqueue) the engine posts
     * through a signal pipe, and nxt_event_engine_create() enables its read
     * event in this same batch.  Commit it, so that every leg counts only
     * the changes it queued itself.
     */

    engine->event.poll(engine, 0);

    if (nxt_slow_path(nxt_fd_event_change_test_nchanges(engine) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: the engine's own changes were "
                      "not committed, %ui left",
                      nxt_fd_event_change_test_nchanges(engine));
        ret = NXT_ERROR;
        goto done;
    }

    ret = nxt_fd_event_change_test_drop(thr, engine);

    if (ret == NXT_OK) {
        ret = nxt_fd_event_change_test_keeps_others(thr, engine);
    }

    if (ret == NXT_OK) {
        ret = nxt_fd_event_change_test_port(thr, engine);
    }

done:

    thr->engine = saved_engine;

    nxt_event_engine_free(engine);

    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "fd event change test passed");

    return NXT_OK;
}


static nxt_event_engine_t *
nxt_fd_event_change_test_engine(nxt_task_t *task)
{
    nxt_mp_t                     *mp;
    nxt_array_t                  *services;
    nxt_event_engine_t           *engine;
    const nxt_event_interface_t  *interface;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NULL;
    }

    services = nxt_services_init(mp);
    if (nxt_slow_path(services == NULL)) {
        nxt_mp_destroy(mp);
        return NULL;
    }

    interface = nxt_service_get(services, "engine", NULL);
    if (nxt_slow_path(interface == NULL)) {
        nxt_mp_destroy(mp);
        return NULL;
    }

    /*
     * No signal set: this fixture drives the engine by hand and never runs
     * nxt_event_engine_start(), so nothing here delivers a signal.
     */

    engine = nxt_event_engine_create(task, interface, NULL, 0, 0);

    nxt_mp_destroy(mp);

    return engine;
}


static nxt_fd_event_t *
nxt_fd_event_change_test_event(nxt_task_t *task, nxt_fd_t fd)
{
    nxt_fd_event_t  *ev;

    /*
     * nxt_malloc(), not the mem pool a port uses, so that the poll at the
     * end of the leg reads freed memory outright under a sanitizer rather
     * than memory a pool happens to keep mapped.
     */

    ev = nxt_zalloc(sizeof(nxt_fd_event_t));
    if (nxt_slow_path(ev == NULL)) {
        return NULL;
    }

    ev->fd = fd;
    ev->task = task;
    ev->log = task->log;
    ev->read_work_queue = &task->thread->engine->fast_work_queue;
    ev->write_work_queue = &task->thread->engine->fast_work_queue;
    ev->read_handler = nxt_fd_event_change_test_handler;
    ev->write_handler = nxt_fd_event_change_test_handler;
    ev->error_handler = nxt_fd_event_change_test_handler;

    return ev;
}


static void
nxt_fd_event_change_test_handler(nxt_task_t *task, void *obj, void *data)
{
}


/*
 * The bare mechanism: a change queued for an event, then dropped, leaves the
 * batch empty, so freeing the event afterwards cannot be reached by a commit.
 */

static nxt_int_t
nxt_fd_event_change_test_drop(nxt_thread_t *thr, nxt_event_engine_t *engine)
{
    nxt_int_t       ret;
    nxt_fd_event_t  *ev;
    nxt_socket_t    pair[2];

    ret = NXT_ERROR;
    ev = NULL;
    pair[0] = -1;
    pair[1] = -1;

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: socketpair failed");
        return NXT_ERROR;
    }

    ev = nxt_fd_event_change_test_event(thr->task, pair[1]);
    if (nxt_slow_path(ev == NULL)) {
        goto done;
    }

    nxt_fd_event_enable_write(engine, ev);

    if (nxt_slow_path(nxt_fd_event_change_test_nchanges(engine) != 1)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: enabling the write event queued "
                      "%ui changes, expected 1",
                      nxt_fd_event_change_test_nchanges(engine));
        goto done;
    }

#if (NXT_FD_EVENT_CHANGE_TEST_CHANGING)
    if (nxt_slow_path(ev->changing != 1)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: a queued change left changing:%d",
                      (int) ev->changing);
        goto done;
    }
#endif

    nxt_fd_event_cancel_changes(engine, ev);

    if (nxt_slow_path(nxt_fd_event_change_test_nchanges(engine) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: cancelling left %ui changes",
                      nxt_fd_event_change_test_nchanges(engine));
        goto done;
    }

#if (NXT_FD_EVENT_CHANGE_TEST_CHANGING)
    if (nxt_slow_path(ev->changing != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: cancelling left changing:%d",
                      (int) ev->changing);
        goto done;
    }
#endif

    nxt_free(ev);
    ev = NULL;

    /* Commits whatever is left.  Nothing is, so nothing reads the free. */

    engine->event.poll(engine, 0);

    ret = NXT_OK;

done:

    nxt_free(ev);

    if (pair[0] != -1) {
        nxt_socket_close(thr->task, pair[0]);
    }

    if (pair[1] != -1) {
        nxt_socket_close(thr->task, pair[1]);
    }

    return ret;
}


/*
 * Cancelling one event's change leaves every other event's change in the
 * batch, and still commits it.  The compaction is the part of this that can
 * silently go wrong.
 */

static nxt_int_t
nxt_fd_event_change_test_keeps_others(nxt_thread_t *thr,
    nxt_event_engine_t *engine)
{
    nxt_int_t       ret;
    nxt_uint_t      i;
    nxt_fd_event_t  *ev[3];
    nxt_socket_t    pair[3][2];

    ret = NXT_ERROR;

    for (i = 0; i < 3; i++) {
        ev[i] = NULL;
        pair[i][0] = -1;
        pair[i][1] = -1;
    }

    for (i = 0; i < 3; i++) {
        if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair[i]) != 0)) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "fd event change test: socketpair failed");
            goto done;
        }

        ev[i] = nxt_fd_event_change_test_event(thr->task, pair[i][1]);
        if (nxt_slow_path(ev[i] == NULL)) {
            goto done;
        }

        nxt_fd_event_enable_write(engine, ev[i]);
    }

    if (nxt_slow_path(nxt_fd_event_change_test_nchanges(engine) != 3)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: three events queued %ui changes",
                      nxt_fd_event_change_test_nchanges(engine));
        goto done;
    }

    /* The middle one, so that compaction has to move a tail entry. */

    nxt_fd_event_cancel_changes(engine, ev[1]);

    if (nxt_slow_path(nxt_fd_event_change_test_nchanges(engine) != 2)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: cancelling one of three left %ui "
                      "changes, expected 2",
                      nxt_fd_event_change_test_nchanges(engine));
        goto done;
    }

#if (NXT_FD_EVENT_CHANGE_TEST_CHANGING)
    if (nxt_slow_path(ev[0]->changing != 1 || ev[2]->changing != 1)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: cancelling one event cleared "
                      "changing on another (%d, %d)",
                      (int) ev[0]->changing, (int) ev[2]->changing);
        goto done;
    }
#endif

    nxt_free(ev[1]);
    ev[1] = NULL;

    /*
     * Commits the two that were kept.  Both are still allocated, and both
     * descriptors are still open, so this is the check that compaction left
     * usable entries rather than merely the right count.
     */

    engine->event.poll(engine, 0);

    if (nxt_slow_path(nxt_fd_event_change_test_nchanges(engine) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: the poll left %ui changes",
                      nxt_fd_event_change_test_nchanges(engine));
        goto done;
    }

#if (NXT_FD_EVENT_CHANGE_TEST_CHANGING)
    if (nxt_slow_path(ev[0]->changing != 0 || ev[2]->changing != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: a committed change left "
                      "changing set (%d, %d)",
                      (int) ev[0]->changing, (int) ev[2]->changing);
        goto done;
    }
#endif

#if (NXT_HAVE_EPOLL)
    /*
     * The count above says two entries survived; this says they are the
     * right two.  A swapped entry commits silently and keeps the count, so
     * ask the kernel instead: the kept descriptors are in the epoll set and
     * EPOLL_CTL_MOD finds them, the cancelled one was never added and
     * EPOLL_CTL_MOD reports ENOENT.
     */

    {
        int                 err;
        nxt_uint_t          k;
        struct epoll_event  ee;

        for (k = 0; k < 3; k++) {
            nxt_memzero(&ee, sizeof(struct epoll_event));
            ee.events = EPOLLOUT;
            ee.data.ptr = ev[k];

            err = epoll_ctl(engine->u.epoll.fd, EPOLL_CTL_MOD, pair[k][1],
                            &ee);

            if (k == 1) {
                if (nxt_slow_path(err == 0 || nxt_errno != NXT_ENOENT)) {
                    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                                  "fd event change test: the cancelled event "
                                  "reached the epoll set (fd:%d err:%d)",
                                  pair[k][1], err);
                    goto done;
                }

            } else if (nxt_slow_path(err != 0)) {
                nxt_log_error(NXT_LOG_NOTICE, thr->log,
                              "fd event change test: a kept event is not in "
                              "the epoll set (fd:%d) %E",
                              pair[k][1], nxt_errno);
                goto done;
            }
        }
    }
#endif

    ret = NXT_OK;

done:

    for (i = 0; i < 3; i++) {
        nxt_free(ev[i]);

        if (pair[i][0] != -1) {
            nxt_socket_close(thr->task, pair[i][0]);
        }

        if (pair[i][1] != -1) {
            nxt_socket_close(thr->task, pair[i][1]);
        }
    }

    return ret;
}


/*
 * The reachable case.  nxt_port_rearm_now() arms the write event, and
 * nxt_port_write_msgs() calls it immediately before the nxt_port_use() that
 * can take the count to zero -- which releases the port's memory pool, and
 * port->socket lives in it.  This leg is that pair of calls with nothing in
 * between: arm, then drop the last reference.
 *
 * Before the fix the poll below commits a change into freed memory.  Under a
 * sanitized build that is a heap-use-after-free report; in a plain build it
 * is silent, which is why the change count is asserted as well.
 */

static nxt_int_t
nxt_fd_event_change_test_port(nxt_thread_t *thr, nxt_event_engine_t *engine)
{
    nxt_int_t     ret;
    nxt_task_t    *task;
    nxt_port_t    *port;
    nxt_socket_t  pair[2];

    ret = NXT_ERROR;
    task = thr->task;

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: socketpair failed");
        return NXT_ERROR;
    }

    port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_MAIN);
    if (nxt_slow_path(port == NULL)) {
        nxt_socket_close(task, pair[0]);
        nxt_socket_close(task, pair[1]);
        return NXT_ERROR;
    }

    port->pair[0] = pair[0];
    port->pair[1] = pair[1];
    port->socket.task = task;

    nxt_port_write_enable(task, port);

    if (nxt_slow_path(port->engine != engine)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: the port took another engine");
        goto done;
    }

    /* Same engine, so this arms the write event inline. */

    nxt_port_rearm(task, port);

    if (nxt_slow_path(nxt_fd_event_change_test_nchanges(engine) != 1)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: re-arming the port queued %ui "
                      "changes, expected 1",
                      nxt_fd_event_change_test_nchanges(engine));
        goto done;
    }

    ret = NXT_OK;

done:

    /*
     * What nxt_port_close() does to the descriptors, which every release
     * path runs first -- nxt_port_mp_cleanup() asserts on it.  port->socket
     * keeps the descriptor number it had, so a change left in the batch now
     * names a number the next open() may hand to somebody else.
     */

    nxt_socket_close(task, port->pair[0]);
    port->pair[0] = -1;

    nxt_socket_close(task, port->pair[1]);
    port->pair[1] = -1;

    /*
     * The last reference.  nxt_port_release() runs from here and releases
     * port->mem_pool, which is where port->socket lives.
     */

    nxt_port_use(task, port, -1);

    if (ret != NXT_OK) {
        return ret;
    }

    if (nxt_slow_path(nxt_fd_event_change_test_nchanges(engine) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "fd event change test: releasing the port left %ui "
                      "changes pointing into its freed memory pool",
                      nxt_fd_event_change_test_nchanges(engine));
        return NXT_ERROR;
    }

    /* Would commit that change, if one were left. */

    engine->event.poll(engine, 0);

    return NXT_OK;
}

#else /* !NXT_FD_EVENT_CHANGE_TEST */

nxt_int_t
nxt_fd_event_change_test(nxt_thread_t *thr)
{
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "fd event change test skipped: the change batch is asserted "
                  "on only for epoll and kqueue");

    return NXT_OK;
}

#endif
