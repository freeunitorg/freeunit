/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for M-5: nxt_conn_accept_close_idle() (nxt_conn_accept.c)
 * used to call nxt_fd_event_disable_read() unconditionally.  When the 100ms
 * listen timer fires and the spare conn still cannot be allocated, this
 * function runs again on a listener that is already inactive; disabling an
 * inactive epoll read is what turns into an epoll_ctl() ENOENT alert every
 * 100ms (see nxt_epoll_engine.c).  The fix guards the call with
 * nxt_fd_event_is_active(), matching nxt_router.c:4992-4994.
 *
 * nxt_conn_accept_close_idle() itself is static; nxt_conn_accept_error() is
 * the one entry point into it, for the EMFILE/ENFILE/ENOBUFS/ENOMEM cases.
 */

#include <nxt_main.h>
#include <nxt_event_engine.h>
#include <nxt_conn.h>
#include "nxt_tests.h"


static nxt_uint_t  nxt_conn_close_idle_test_disable_calls;


static void
nxt_conn_close_idle_test_disable_read(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev)
{
    nxt_conn_close_idle_test_disable_calls++;

    /* What a real disable does: the read side becomes inactive. */
    ev->read = NXT_EVENT_INACTIVE;
}


nxt_int_t
nxt_conn_close_idle_test(nxt_thread_t *thr)
{
    nxt_int_t               ret;
    nxt_task_t              *task;
    nxt_listen_event_t      lev;
    nxt_event_engine_t      engine, *saved_engine;
    nxt_work_queue_cache_t  cache;

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "conn close idle test started");

    ret = NXT_ERROR;

    task = thr->task;
    task->thread = thr;

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));
    nxt_memzero(&lev, sizeof(nxt_listen_event_t));

    if (nxt_slow_path(nxt_timers_init(&engine.timers, 4) != NXT_OK)) {
        nxt_log_alert(thr->log, "conn close idle test failed to init "
                      "timers");
        return NXT_ERROR;
    }

    engine.event.disable_read = nxt_conn_close_idle_test_disable_read;

    nxt_work_queue_cache_create(&cache, 1024);

    engine.close_work_queue.cache = &cache;
    nxt_work_queue_name(&engine.close_work_queue, "close");

    /* nxt_conn_accept_close_idle() reads the engine off task->thread. */
    saved_engine = thr->engine;
    thr->engine = &engine;

    lev.socket.task = task;
    lev.socket.log = thr->log;
    lev.socket.read = NXT_EVENT_DEFAULT;    /* armed, as a live listener is */
    lev.timer.task = task;
    lev.timer.log = thr->log;

    nxt_conn_close_idle_test_disable_calls = 0;

    /*
     * First round: the listener is active, so the guard must still let the
     * disable through -- this is not a "never disable" fix.
     */
    nxt_conn_accept_error(task, &lev, "test", NXT_ENOMEM);

    if (nxt_slow_path(nxt_conn_close_idle_test_disable_calls != 1)) {
        nxt_log_alert(thr->log, "conn close idle test: an active listener "
                      "was not disabled (%ui calls)",
                      nxt_conn_close_idle_test_disable_calls);
        goto done;
    }

    /*
     * Second round: the 100ms timer fired again and the spare conn still
     * cannot be allocated, so this runs again on the now-inactive listener.
     * Before the fix, disable_read() would be called again here.
     */
    nxt_conn_accept_error(task, &lev, "test", NXT_ENOMEM);

    if (nxt_slow_path(nxt_conn_close_idle_test_disable_calls != 1)) {
        nxt_log_alert(thr->log, "conn close idle test: disable_read() was "
                      "called on an already inactive listener (%ui calls "
                      "total) -- this is the epoll_ctl ENOENT storm",
                      nxt_conn_close_idle_test_disable_calls);
        goto done;
    }

    ret = NXT_OK;

done:

    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&cache);
    nxt_free(engine.timers.changes);

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "conn close idle test passed");
    }

    return ret;
}
