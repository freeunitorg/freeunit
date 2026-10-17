/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * The connection counters of an engine have one writer, the thread of the
 * engine.  nxt_router_status_handler() reads them from the router main
 * thread with nxt_atomic_load().  The writer must also use atomic accesses:
 * nxt_atomic_store() of nxt_atomic_load() plus or minus one.  A plain ++ or
 * -- on the writer side is a data race with the reader.
 *
 * The test runs one writer and one reader.  The writer moves a conn through
 * nxt_conn_idle(), nxt_conn_active() and nxt_conn_untrack(), the macros of
 * the connection path.  Each cycle adds one to idle_conns_cnt, removes it
 * again, and adds one to closed_conns_cnt.  The reader reads both counters
 * with nxt_atomic_load(), as the status handler does.  It checks that
 * closed_conns_cnt never decreases and never exceeds the number of cycles,
 * and that idle_conns_cnt is 0 or 1.  After the join, the writer checks the
 * final counts.
 *
 * ThreadSanitizer reports a data race on the plain ++ and -- of the macros
 * before the change.  A build without it checks only the values.
 */

#include <nxt_main.h>
#include <nxt_event_engine.h>
#include <nxt_conn.h>
#include "nxt_tests.h"


#define NXT_CONN_COUNTERS_TEST_CYCLES  100000


typedef struct {
    nxt_event_engine_t  engine;
    nxt_atomic_t        ready;
    nxt_atomic_t        stop;

    /* The reader sets these.  The writer reads them after the join. */
    nxt_uint_t          reads;
    nxt_uint_t          changes;
    nxt_uint_t          decreases;
    nxt_uint_t          over_bound;
    nxt_uint_t          bad_idle;
    nxt_uint_t          max_idle;
} nxt_conn_counters_test_t;


static void
nxt_conn_counters_test_reader(void *data)
{
    nxt_uint_t                idle, closed, last, stop;
    nxt_conn_counters_test_t  *t;

    t = data;
    last = 0;

    nxt_atomic_store(&t->ready, 1);

    do {
        stop = nxt_atomic_load(&t->stop);

        closed = nxt_atomic_load(&t->engine.closed_conns_cnt);
        idle = nxt_atomic_load(&t->engine.idle_conns_cnt);

        t->reads++;

        if (closed < last) {
            t->decreases++;

        } else if (closed > last) {
            t->changes++;
        }

        if (closed > NXT_CONN_COUNTERS_TEST_CYCLES) {
            t->over_bound++;
        }

        if (idle > 1) {
            t->bad_idle++;
            t->max_idle = nxt_max(t->max_idle, idle);
        }

        last = closed;

    } while (stop == 0);
}


static int
nxt_conn_counters_test_child(void *data)
{
    nxt_uint_t                i, idle, closed;
    nxt_conn_t                conn, *c;
    nxt_thread_t              *thr;
    nxt_thread_link_t         *link;
    nxt_thread_handle_t       handle;
    nxt_conn_counters_test_t  t;

    thr = data;

    /* The hang guard.  A run under ThreadSanitizer takes about a second. */
    alarm(60);

    nxt_memzero(&t, sizeof(nxt_conn_counters_test_t));
    nxt_queue_init(&t.engine.idle_connections);
    nxt_queue_init(&t.engine.active_connections);

    nxt_memzero(&conn, sizeof(nxt_conn_t));
    c = &conn;
    c->idle = NXT_CONN_TRACK_NONE;

    link = nxt_zalloc(sizeof(nxt_thread_link_t));
    NXT_TEST_CHECK(thr->log, link != NULL,
                   "conn counters test failed: no memory for the link");

    link->start = nxt_conn_counters_test_reader;
    link->work.data = &t;

    NXT_TEST_CHECK(thr->log, nxt_thread_create(&handle, link) == NXT_OK,
                   "conn counters test failed: thread create");

    /* The reader must read while the writer writes. */

    while (nxt_atomic_load(&t.ready) == 0) {
        nxt_cpu_pause();
    }

    for (i = 0; i < NXT_CONN_COUNTERS_TEST_CYCLES; i++) {
        nxt_conn_idle(&t.engine, c);
        nxt_conn_active(&t.engine, c);
        nxt_conn_untrack(&t.engine, c);
    }

    nxt_atomic_store(&t.stop, 1);

    nxt_thread_wait(handle);

    NXT_TEST_CHECK(thr->log, t.decreases == 0,
                   "conn counters test failed: the reader saw "
                   "closed_conns_cnt decrease %ui times", t.decreases);

    NXT_TEST_CHECK(thr->log, t.over_bound == 0,
                   "conn counters test failed: the reader saw "
                   "closed_conns_cnt over %ui %ui times",
                   (nxt_uint_t) NXT_CONN_COUNTERS_TEST_CYCLES, t.over_bound);

    NXT_TEST_CHECK(thr->log, t.bad_idle == 0,
                   "conn counters test failed: the reader saw "
                   "idle_conns_cnt %ui, more than 1, %ui times",
                   t.max_idle, t.bad_idle);

    closed = nxt_atomic_load(&t.engine.closed_conns_cnt);
    idle = nxt_atomic_load(&t.engine.idle_conns_cnt);

    NXT_TEST_CHECK(thr->log, closed == NXT_CONN_COUNTERS_TEST_CYCLES,
                   "conn counters test failed: closed_conns_cnt is %ui, "
                   "not %ui", closed,
                   (nxt_uint_t) NXT_CONN_COUNTERS_TEST_CYCLES);

    NXT_TEST_CHECK(thr->log, idle == 0,
                   "conn counters test failed: idle_conns_cnt is %ui, "
                   "not 0", idle);

    NXT_TEST_CHECK(thr->log, c->idle == NXT_CONN_TRACK_NONE
                             && nxt_queue_is_empty(&t.engine.idle_connections)
                             && nxt_queue_is_empty(
                                    &t.engine.active_connections),
                   "conn counters test failed: the conn is still tracked");

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "conn counters test: %ui reads, %ui changes seen",
                  t.reads, t.changes);

    return 0;
}


nxt_int_t
nxt_conn_counters_test(nxt_thread_t *thr)
{
    int  rc;

    rc = nxt_test_in_child(thr, "conn counters test",
                           nxt_conn_counters_test_child, thr);

    NXT_TEST_CHECK(thr->log, rc == 0,
                   "conn counters test failed (child status %d)", rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "conn counters test passed");

    return NXT_OK;
}
