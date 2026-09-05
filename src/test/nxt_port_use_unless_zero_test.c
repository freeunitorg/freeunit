/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include "nxt_tests.h"

#include <pthread.h>


#define NXT_PORT_UUZ_RACERS  8
#define NXT_PORT_UUZ_ROUNDS  2000


typedef struct {
    nxt_port_t        *port;
    nxt_atomic_t      gen;
    nxt_atomic_t      done;
    nxt_atomic_t      released;
    nxt_atomic_t      wins;
    nxt_atomic_t      late_wins;
    nxt_atomic_t      stop;
} nxt_port_uuz_ctx_t;


static nxt_int_t nxt_port_uuz_test_semantics(nxt_thread_t *thr);
static nxt_int_t nxt_port_uuz_test_after_last_drop(nxt_thread_t *thr);
static nxt_int_t nxt_port_uuz_test_race(nxt_thread_t *thr);
static void *nxt_port_uuz_racer(void *data);
static nxt_port_t *nxt_port_uuz_port(nxt_task_t *task);
static void nxt_port_uuz_free(nxt_port_t *port);


nxt_int_t
nxt_port_use_unless_zero_test(nxt_thread_t *thr)
{
    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "port use_unless_zero test started");

    if (nxt_port_uuz_test_semantics(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_uuz_test_after_last_drop(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_uuz_test_race(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "port use_unless_zero test passed");

    return NXT_OK;
}


/* A live port hands out references, and the count follows them exactly. */

static nxt_int_t
nxt_port_uuz_test_semantics(nxt_thread_t *thr)
{
    nxt_port_t  *port;

    port = nxt_port_uuz_port(thr->task);
    if (nxt_slow_path(port == NULL)) {
        return NXT_ERROR;
    }

    if (nxt_slow_path(!nxt_port_use_unless_zero(port))) {
        nxt_log_alert(thr->log, "port use_unless_zero test: a live port "
                      "(use_count %A) refused a reference", port->use_count);
        goto fail;
    }

    if (nxt_slow_path(port->use_count != 2)) {
        nxt_log_alert(thr->log, "port use_unless_zero test: use_count is %A, "
                      "expected 2", port->use_count);
        goto fail;
    }

    if (nxt_slow_path(!nxt_port_use_unless_zero(port)
                      || port->use_count != 3))
    {
        nxt_log_alert(thr->log, "port use_unless_zero test: a second "
                      "reference did not stack (use_count %A)",
                      port->use_count);
        goto fail;
    }

    nxt_atomic_fetch_add(&port->use_count, -2);

    if (nxt_slow_path(port->use_count != 1)) {
        nxt_log_alert(thr->log, "port use_unless_zero test: use_count is %A "
                      "after two drops, expected 1", port->use_count);
        goto fail;
    }

    nxt_port_uuz_free(port);

    return NXT_OK;

fail:

    nxt_port_uuz_free(port);

    return NXT_ERROR;
}


/*
 * The regression itself (issue #195).
 *
 * nxt_process_broadcast_shm_ack() reaches an app port through
 * process->ports, which is a lookup: the port may already have dropped its
 * last reference and be inside nxt_port_release(), destroying its memory
 * pool.  The unconditional nxt_atomic_fetch_add() that nxt_port_post() does
 * on that pointer resurrects a dead port -- the `use_count == 0` assertion
 * in nxt_port_mp_cleanup() in a debug build, a use-after-free in a release
 * one.
 *
 * The state reproduced here is exactly the window: use_count has reached
 * zero and the port is still linked, because nxt_port_release() unlinks it
 * only afterwards.  A try-ref must fail, and must leave the count alone --
 * an implementation that increments first and inspects afterwards leaves 1
 * here and fails the same way the router did.
 */

static nxt_int_t
nxt_port_uuz_test_after_last_drop(nxt_thread_t *thr)
{
    nxt_int_t   ret;
    nxt_uint_t  i;
    nxt_port_t  *port;

    port = nxt_port_uuz_port(thr->task);
    if (nxt_slow_path(port == NULL)) {
        return NXT_ERROR;
    }

    ret = NXT_OK;

    /* The last drop, without the release nxt_port_use() would run. */
    nxt_atomic_fetch_add(&port->use_count, -1);

    for (i = 0; i < 3; i++) {
        if (nxt_slow_path(nxt_port_use_unless_zero(port))) {
            nxt_log_alert(thr->log, "port use_unless_zero test: a port whose "
                          "last reference is gone handed out one anyway "
                          "(attempt %ui)", i + 1);
            ret = NXT_ERROR;
            break;
        }

        if (nxt_slow_path(port->use_count != 0)) {
            nxt_log_alert(thr->log, "port use_unless_zero test: a refused "
                          "try-ref changed use_count to %A", port->use_count);
            ret = NXT_ERROR;
            break;
        }
    }

    port->use_count = 0;

    nxt_port_uuz_free(port);

    return ret;
}


/*
 * The same thing under contention, with the ordering the mutex in
 * nxt_process_broadcast_shm_ack() and nxt_port_release() guarantees: the
 * racers only look the port up after the owner has dropped the last
 * reference.  Every one of them must be refused.
 *
 * The rounds also exercise the CAS loop against concurrent increments, so a
 * lost update would show up as a non-zero count at the end of a round.
 */

static nxt_int_t
nxt_port_uuz_test_race(nxt_thread_t *thr)
{
    nxt_int_t           ret;
    nxt_uint_t          i, round;
    pthread_t           racers[NXT_PORT_UUZ_RACERS];
    nxt_port_uuz_ctx_t  ctx;

    nxt_memzero(&ctx, sizeof(ctx));

    ctx.port = nxt_port_uuz_port(thr->task);
    if (nxt_slow_path(ctx.port == NULL)) {
        return NXT_ERROR;
    }

    /*
     * The port outlives every round: the count is put back by hand rather
     * than letting the port be released, so that the racers never touch
     * freed memory even when the implementation under test is wrong.
     */
    ctx.port->use_count = 0;

    for (i = 0; i < NXT_PORT_UUZ_RACERS; i++) {
        if (nxt_slow_path(pthread_create(&racers[i], NULL, nxt_port_uuz_racer,
                                         &ctx) != 0))
        {
            nxt_log_alert(thr->log, "port use_unless_zero test: "
                          "pthread_create() failed");

            ctx.stop = 1;
            nxt_atomic_fetch_add(&ctx.gen, 1);

            while (i > 0) {
                pthread_join(racers[--i], NULL);
            }

            nxt_port_uuz_free(ctx.port);

            return NXT_ERROR;
        }
    }

    ret = NXT_OK;

    for (round = 0; round < NXT_PORT_UUZ_ROUNDS; round++) {
        ctx.port->use_count = 1;
        ctx.released = 0;
        ctx.late_wins = 0;
        ctx.done = 0;

        nxt_atomic_fetch_add(&ctx.gen, 1);

        /* Contended try-refs, which may or may not win. */
        nxt_thread_yield();

        if (nxt_atomic_fetch_add(&ctx.port->use_count, -1) != 1) {
            /*
             * A racer that won still holds a reference; wait for it.  It
             * cannot be held for long -- every winner drops immediately.
             */
            while (ctx.port->use_count != 0) {
                nxt_thread_yield();
            }
        }

        /* From here on no lookup may produce a reference. */
        ctx.released = 1;

        while (ctx.done != NXT_PORT_UUZ_RACERS) {
            nxt_thread_yield();
        }

        if (nxt_slow_path(ctx.port->use_count != 0)) {
            nxt_log_alert(thr->log, "port use_unless_zero test: use_count is "
                          "%A after round %ui, expected 0",
                          ctx.port->use_count, round);
            ret = NXT_ERROR;
            break;
        }

        if (nxt_slow_path(ctx.late_wins != 0)) {
            nxt_log_alert(thr->log, "port use_unless_zero test: %A "
                          "reference(s) taken after the last drop, in round "
                          "%ui", ctx.late_wins, round);
            ret = NXT_ERROR;
            break;
        }
    }

    ctx.stop = 1;
    nxt_atomic_fetch_add(&ctx.gen, 1);

    for (i = 0; i < NXT_PORT_UUZ_RACERS; i++) {
        pthread_join(racers[i], NULL);
    }

    if (ret == NXT_OK && ctx.wins == 0) {
        /*
         * Not a failure of the code under test, but nothing was contended,
         * so say so rather than pass quietly.
         */
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port use_unless_zero test: "
                      "no contended try-ref won; the race was not exercised");
    }

    ctx.port->use_count = 0;

    nxt_port_uuz_free(ctx.port);

    return ret;
}


static void *
nxt_port_uuz_racer(void *data)
{
    nxt_bool_t          late;
    nxt_atomic_int_t    gen, last_gen;
    nxt_port_uuz_ctx_t  *ctx;

    ctx = data;
    last_gen = 0;

    for ( ;; ) {
        for ( ;; ) {
            gen = ctx->gen;

            if (gen != last_gen) {
                break;
            }

            nxt_thread_yield();
        }

        last_gen = gen;

        if (ctx->stop) {
            return NULL;
        }

        /*
         * Whether this lookup is racing the owner's last drop or is
         * unambiguously after it decides what the outcome is allowed to be:
         * a try-ref after the drop must always be refused.
         */
        late = (ctx->released != 0);

        if (nxt_port_use_unless_zero(ctx->port)) {
            if (late) {
                nxt_atomic_fetch_add(&ctx->late_wins, 1);
            }

            nxt_atomic_fetch_add(&ctx->wins, 1);
            nxt_atomic_fetch_add(&ctx->port->use_count, -1);
        }

        nxt_atomic_fetch_add(&ctx->done, 1);
    }
}


static nxt_port_t *
nxt_port_uuz_port(nxt_task_t *task)
{
    nxt_port_t  *port;

    port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_APP);

    if (nxt_slow_path(port == NULL)) {
        return NULL;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    return port;
}


/*
 * nxt_port_use() cannot be used here: the last drop goes to
 * nxt_port_release(), which needs a runtime this test does not build.  The
 * pool cleanup that nxt_port_new() registered still runs, and still asserts
 * the port is quiescent.
 */

static void
nxt_port_uuz_free(nxt_port_t *port)
{
    port->use_count = 0;

    nxt_mp_destroy(port->mem_pool);
}
