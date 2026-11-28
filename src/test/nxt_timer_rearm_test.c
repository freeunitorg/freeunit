/*
 * Copyright (C) FreeUnit Community
 */

/*
 * A timer that expired and is armed again before its queued handler runs.
 *
 * nxt_timer_expire() removes a due timer from the tree and queues
 * nxt_timer_handler().  The owner can arm the timer again before that work
 * runs.  The new time is then a pending change, and the queued work must do
 * nothing: the timer fires later from the tree, at its new time.
 *
 * The change batch of an engine has a fixed size.  When it is full,
 * nxt_timer_change() commits it at once.  So if as many other timers change
 * after the new arming as the batch holds, the timer is in the tree again
 * before the queued work runs, and it has no pending change.  The queued
 * work must not call the handler then either.  Otherwise the handler runs
 * at the old time, and the timer node stays linked in the tree.  A handler
 * that frees the memory of the timer leaves a freed node in the tree.
 *
 * The test runs the same steps with a batch of 4 and with a batch of 128,
 * the size that every engine of unitd has.  It checks that the handler is
 * never called while its timer is in the tree, that it is called once, at
 * the new time, and that the tree is empty at the end.
 */

#include <nxt_main.h>
#include "nxt_tests.h"


#define NXT_TIMER_REARM_TEST_MAX  128


typedef struct {
    nxt_timer_t         timer;
    nxt_event_engine_t  *engine;
    nxt_uint_t          calls;
    nxt_uint_t          calls_in_tree;
    nxt_msec_t          at;
} nxt_timer_rearm_test_t;


typedef struct {
    nxt_thread_t        *thr;
    nxt_uint_t          batch;
} nxt_timer_rearm_test_ctx_t;


static void
nxt_timer_rearm_test_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_timer_rearm_test_t  *t;

    t = nxt_timer_data(obj, nxt_timer_rearm_test_t, timer);

    t->calls++;
    t->at = t->engine->timers.now;

    if (nxt_timer_is_in_tree(&t->timer)) {
        t->calls_in_tree++;
    }
}


static void
nxt_timer_rearm_test_noop(nxt_task_t *task, void *obj, void *data)
{
}


/* The queued work of one engine pass. */

static void
nxt_timer_rearm_test_drain(nxt_event_engine_t *engine)
{
    void                *obj, *data;
    nxt_task_t          *task;
    nxt_work_handler_t  handler;

    while (engine->fast_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine->fast_work_queue, &task, &obj,
                                     &data);
        handler(task, obj, data);
    }
}


static void
nxt_timer_rearm_test_init(nxt_event_engine_t *engine, nxt_timer_t *timer,
    nxt_work_handler_t handler)
{
    timer->bias = 0;
    timer->work_queue = &engine->fast_work_queue;
    timer->handler = handler;
    timer->task = &engine->task;
    timer->log = engine->task.log;
}


static int
nxt_timer_rearm_test_run(nxt_timer_rearm_test_ctx_t *ctx,
    nxt_event_engine_t *engine)
{
    nxt_log_t               *log;
    nxt_uint_t              i, failed;
    nxt_timer_t             other[NXT_TIMER_REARM_TEST_MAX];
    nxt_timer_rearm_test_t  t;

    log = ctx->thr->log;
    failed = 0;

    nxt_memzero(&t, sizeof(t));
    nxt_memzero(other, sizeof(other));

    t.engine = engine;
    nxt_timer_rearm_test_init(engine, &t.timer, nxt_timer_rearm_test_handler);

    for (i = 0; i < ctx->batch; i++) {
        nxt_timer_rearm_test_init(engine, &other[i], nxt_timer_rearm_test_noop);
    }

    engine->timers.now = 1000;

    nxt_timer_add(engine, &t.timer, 10);
    (void) nxt_timer_find(engine);

    /* The timer is due: it leaves the tree and its handler is queued. */

    nxt_timer_expire(engine, 1010);

    if (!t.timer.queued || nxt_timer_is_in_tree(&t.timer)) {
        nxt_log_alert(log, "timer rearm test, batch %ui: the timer was not "
                      "queued by the expiry", ctx->batch);
        return 2;
    }

    /*
     * The timer is armed again before the queued work runs.  Then as many
     * other timers change as the batch holds, so the batch is committed
     * before the queued work runs.
     */

    nxt_timer_add(engine, &t.timer, 100);

    for (i = 0; i < ctx->batch; i++) {
        nxt_timer_add(engine, &other[i], 1000);
    }

    if (!nxt_timer_is_in_tree(&t.timer)) {
        nxt_log_alert(log, "timer rearm test, batch %ui: the batch was not "
                      "committed early", ctx->batch);
        return 2;
    }

    nxt_timer_rearm_test_drain(engine);

    if (t.calls_in_tree != 0) {
        nxt_log_alert(log, "timer rearm test, batch %ui: the queued work "
                      "called the handler %ui time(s) while the timer was in "
                      "the tree, at %M", ctx->batch, t.calls_in_tree, t.at);
        failed = 1;
    }

    /* The new time comes. */

    (void) nxt_timer_find(engine);
    nxt_timer_expire(engine, 1110);
    nxt_timer_rearm_test_drain(engine);

    if (t.calls != 1 || t.at != 1110) {
        nxt_log_alert(log, "timer rearm test, batch %ui: the handler was "
                      "called %ui time(s), the last at %M, expected once at "
                      "1110", ctx->batch, t.calls, t.at);
        failed = 1;
    }

    if (nxt_timer_is_in_tree(&t.timer)) {
        nxt_log_alert(log, "timer rearm test, batch %ui: the timer is in the "
                      "tree after its handler ran", ctx->batch);
        failed = 1;
    }

    for (i = 0; i < ctx->batch; i++) {
        (void) nxt_timer_delete(engine, &other[i]);
    }

    (void) nxt_timer_find(engine);

    if (!nxt_rbtree_is_empty(&engine->timers.tree)) {
        nxt_log_alert(log, "timer rearm test, batch %ui: the tree is not "
                      "empty at the end", ctx->batch);
        failed = 1;
    }

    return failed;
}


static int
nxt_timer_rearm_test_child(void *data)
{
    int                         ret;
    nxt_event_engine_t          engine;
    nxt_timer_rearm_test_ctx_t  *ctx;

    ctx = data;

    nxt_memzero(&engine, sizeof(engine));

    nxt_work_queue_cache_create(&engine.work_queue_cache, 64);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    engine.task.thread = ctx->thr;
    engine.task.log = ctx->thr->log;

    if (nxt_timers_init(&engine.timers, ctx->batch) != NXT_OK) {
        nxt_work_queue_cache_destroy(&engine.work_queue_cache);
        return 2;
    }

    ret = nxt_timer_rearm_test_run(ctx, &engine);

    nxt_free(engine.timers.changes);
    nxt_work_queue_cache_destroy(&engine.work_queue_cache);

    return ret;
}


nxt_int_t
nxt_timer_rearm_test(nxt_thread_t *thr)
{
    int                         rc;
    nxt_uint_t                  i, failed;
    nxt_timer_rearm_test_ctx_t  ctx;

    static const nxt_uint_t  batches[] = { 4, NXT_TIMER_REARM_TEST_MAX };

    ctx.thr = thr;
    failed = 0;

    for (i = 0; i < nxt_nitems(batches); i++) {
        ctx.batch = batches[i];

        rc = nxt_test_in_child(thr, "timer rearm test",
                               nxt_timer_rearm_test_child, &ctx);

        if (rc != 0) {
            nxt_log_alert(thr->log, "timer rearm test failed, batch %ui "
                          "(child status %d)", ctx.batch, rc);
            failed++;
        }
    }

    NXT_TEST_CHECK(thr->log, failed == 0, "timer rearm test failed");

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "timer rearm test passed");

    return NXT_OK;
}
