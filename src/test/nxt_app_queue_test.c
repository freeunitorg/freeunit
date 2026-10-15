
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Test for the shared application queue doorbell logic and depth tracking.
 */

#include <nxt_main.h>
#include <nxt_app_queue.h>
#include "nxt_tests.h"
#include <pthread.h>


static int
nxt_app_queue_test_death_recovery(void *data)
{
    int                    notify;
    uint32_t               cookie;
    nxt_app_queue_t        *q;
    uint8_t                msg[NXT_APP_QUEUE_MSG_SIZE];
    uint8_t                buf[NXT_APP_QUEUE_MSG_SIZE];

    q = data;
    nxt_memzero(msg, sizeof(msg));

    /* Send request 1: enqueued and rings. */
    if (nxt_app_queue_send(q, msg, sizeof(msg), 1, &notify, &cookie) != NXT_OK) {
        return 1;
    }

    if (notify != 1) {
        return 2;
    }

    /*
     * Simulate the stall condition: worker dequeues request 1 from the queue
     * without clearing the notification flag (e.g. worker was already awake or
     * died before receiving the datagram).  Under the old binary flag, notified
     * remains 1.
     */
    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 3;
    }

    /*
     * The queue is now empty.  Send request 2.  Under depth tracking, request 2
     * must ring (depth 1 <= 1).  Under master's sticky binary flag, it
     * failed to ring (cmp_set(&notified, 0, 1) was 0) and permanently stalled.
     */
    if (nxt_app_queue_send(q, msg, sizeof(msg), 2, &notify, &cookie) != NXT_OK) {
        return 4;
    }

    if (notify != 1) {
        return 5;
    }

    return 0;
}


static int
nxt_app_queue_test_depth(void *data)
{
    uint32_t         cookie;
    nxt_app_queue_t  *q;
    uint8_t          msg[NXT_APP_QUEUE_MSG_SIZE];
    uint8_t          buf[NXT_APP_QUEUE_MSG_SIZE];

    q = data;
    nxt_memzero(msg, sizeof(msg));

    if (nxt_app_queue_depth(q) != 0) {
        return 1;
    }

    if (nxt_app_queue_send(q, msg, sizeof(msg), 1, NULL, &cookie) != NXT_OK) {
        return 2;
    }

    if (nxt_app_queue_depth(q) != 1) {
        return 3;
    }

    if (nxt_app_queue_send(q, msg, sizeof(msg), 2, NULL, &cookie) != NXT_OK) {
        return 4;
    }

    if (nxt_app_queue_depth(q) != 2) {
        return 5;
    }

    if (nxt_app_queue_send(q, msg, sizeof(msg), 3, NULL, &cookie) != NXT_OK) {
        return 6;
    }

    if (nxt_app_queue_depth(q) != 3) {
        return 7;
    }

    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 8;
    }

    if (nxt_app_queue_depth(q) != 2) {
        return 9;
    }

    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 10;
    }

    if (nxt_app_queue_depth(q) != 1) {
        return 11;
    }

    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 12;
    }

    if (nxt_app_queue_depth(q) != 0) {
        return 13;
    }

    return 0;
}


static int
nxt_app_queue_test_burst(void *data)
{
    int              notify;
    uint32_t         cookie;
    nxt_app_queue_t  *q;
    uint8_t          msg[NXT_APP_QUEUE_MSG_SIZE];
    uint8_t          buf[NXT_APP_QUEUE_MSG_SIZE];

    q = data;
    nxt_memzero(msg, sizeof(msg));

    /* Simulate 4 worker processes with depth_bound = 4. */
    /* Items 1..4 must each notify = 1 to wake all 4 workers. */
    if (nxt_app_queue_send_bound(q, msg, sizeof(msg), 1, &notify, &cookie, 4)
        != NXT_OK || notify != 1)
    {
        return 1;
    }

    if (nxt_app_queue_send_bound(q, msg, sizeof(msg), 2, &notify, &cookie, 4)
        != NXT_OK || notify != 1)
    {
        return 2;
    }

    if (nxt_app_queue_send_bound(q, msg, sizeof(msg), 3, &notify, &cookie, 4)
        != NXT_OK || notify != 1)
    {
        return 3;
    }

    if (nxt_app_queue_send_bound(q, msg, sizeof(msg), 4, &notify, &cookie, 4)
        != NXT_OK || notify != 1)
    {
        return 4;
    }

    /* Items 5 and 6: depth > 4, all 4 workers already engaged, notify = 0. */
    if (nxt_app_queue_send_bound(q, msg, sizeof(msg), 5, &notify, &cookie, 4)
        != NXT_OK || notify != 0)
    {
        return 5;
    }

    if (nxt_app_queue_send_bound(q, msg, sizeof(msg), 6, &notify, &cookie, 4)
        != NXT_OK || notify != 0)
    {
        return 6;
    }

    /* Drain 3 items: queue depth drops from 6 to 3. */
    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 7;
    }
    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 8;
    }
    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 9;
    }

    /* Next item: depth becomes 4 <= 4, so notify = 1 again. */
    if (nxt_app_queue_send_bound(q, msg, sizeof(msg), 7, &notify, &cookie, 4)
        != NXT_OK || notify != 1)
    {
        return 10;
    }

    /* Drain remaining 4 items. */
    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 11;
    }
    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 12;
    }
    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 13;
    }
    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 14;
    }

    if (nxt_app_queue_depth(q) != 0) {
        return 15;
    }

    return 0;
}


/*
 * Late head sample: producer A enqueues slot t, producer B enqueues t + 1
 * and rings, a worker consumes t, then A samples head and sees pos == -1.
 * A must still ring, otherwise t + 1 waits with no doorbell.
 */
static int
nxt_app_queue_test_late_head(void *data)
{
    int                    notify;
    int32_t                pos;
    uint32_t               cookie;
    nxt_app_queue_t        *q;
    nxt_app_nncq_atomic_t  t;
    uint8_t                msg[NXT_APP_QUEUE_MSG_SIZE];
    uint8_t                buf[NXT_APP_QUEUE_MSG_SIZE];

    q = data;
    nxt_memzero(msg, sizeof(msg));

    /* A's slot t. */
    t = nxt_app_nncq_tail(&q->queue);
    if (nxt_app_queue_send_bound(q, msg, sizeof(msg), 1, &notify, &cookie, 1)
        != NXT_OK || notify != 1)
    {
        return 1;
    }

    /* B's slot t + 1: depth 1 >= bound 1, no ring from position alone. */
    if (nxt_app_queue_send_bound(q, msg, sizeof(msg), 2, &notify, &cookie, 1)
        != NXT_OK || notify != 0)
    {
        return 2;
    }

    /* A worker consumes slot t. */
    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 3;
    }

    /* A samples head only now. */
    pos = (int32_t) (t - nxt_app_nncq_head(&q->queue));
    if (pos != -1) {
        return 4;
    }

    if (!nxt_app_queue_notify_decision(pos, 1)
        || !nxt_app_queue_notify_decision(-1, 4)
        || !nxt_app_queue_notify_decision(INT32_MIN, 1))
    {
        return 5;
    }

    /* Boundaries unchanged for pos >= 0. */
    if (!nxt_app_queue_notify_decision(0, 1)
        || nxt_app_queue_notify_decision(1, 1)
        || !nxt_app_queue_notify_decision(3, 4)
        || nxt_app_queue_notify_decision(4, 4))
    {
        return 6;
    }

    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg)) {
        return 7;
    }

    return 0;
}


#define NXT_APP_QUEUE_TEST_RACERS  8

#if defined(__APPLE__) || !defined(PTHREAD_BARRIER_SERIAL_THREAD)

typedef struct {
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    int              count;
    int              total;
    int              cycle;
} nxt_barrier_t;

static inline void
nxt_barrier_init(nxt_barrier_t *b, int total)
{
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->cond, NULL);
    b->count = 0;
    b->total = total;
    b->cycle = 0;
}

static inline void
nxt_barrier_wait(nxt_barrier_t *b)
{
    int  cycle;

    pthread_mutex_lock(&b->mutex);
    cycle = b->cycle;
    if (++b->count == b->total) {
        b->cycle++;
        b->count = 0;
        pthread_cond_broadcast(&b->cond);
    } else {
        while (cycle == b->cycle) {
            pthread_cond_wait(&b->cond, &b->mutex);
        }
    }
    pthread_mutex_unlock(&b->mutex);
}

static inline void
nxt_barrier_destroy(nxt_barrier_t *b)
{
    pthread_cond_destroy(&b->cond);
    pthread_mutex_destroy(&b->mutex);
}

#else

typedef pthread_barrier_t nxt_barrier_t;

#define nxt_barrier_init(b, total)  (void) pthread_barrier_init(b, NULL, total)
#define nxt_barrier_wait(b)         (void) pthread_barrier_wait(b)
#define nxt_barrier_destroy(b)      (void) pthread_barrier_destroy(b)

#endif


typedef struct {
    nxt_app_queue_t  *q;
    nxt_barrier_t    start;
    nxt_barrier_t    done;
    nxt_atomic_t     stop;
    nxt_atomic_t     notify_count;
} nxt_app_queue_race_ctx_t;

typedef struct {
    nxt_app_queue_race_ctx_t  *ctx;
    long                      id;
} nxt_app_queue_race_worker_t;


static void *
nxt_app_queue_race_worker(void *arg)
{
    nxt_app_queue_race_worker_t  *w;
    nxt_app_queue_race_ctx_t     *ctx;
    uint32_t                     cookie;
    int                          notify;
    uint8_t                      msg[NXT_APP_QUEUE_MSG_SIZE];

    w = arg;
    ctx = w->ctx;
    nxt_memzero(msg, sizeof(msg));

    for ( ;; ) {
        nxt_barrier_wait(&ctx->start);

        if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE)) {
            break;
        }

        notify = 0;
        (void) nxt_app_queue_send_bound(ctx->q, msg, sizeof(msg),
                                        (uint32_t) w->id, &notify, &cookie, 1);
        if (notify > 0) {
            __atomic_fetch_add(&ctx->notify_count, 1, __ATOMIC_RELAXED);
        }

        nxt_barrier_wait(&ctx->done);
    }

    return NULL;
}


static int
nxt_app_queue_test_concurrent_race(void *data)
{
    nxt_app_queue_race_ctx_t     ctx;
    nxt_app_queue_race_worker_t  workers[NXT_APP_QUEUE_TEST_RACERS];
    pthread_t                    threads[NXT_APP_QUEUE_TEST_RACERS];
    uint32_t                     cookie;
    uint8_t                      buf[NXT_APP_QUEUE_MSG_SIZE];
    long                         r, zero_rounds;
    const long                   rounds = 20000;
    int                          i;

    nxt_memzero(&ctx, sizeof(ctx));
    ctx.q = data;
    zero_rounds = 0;

    nxt_barrier_init(&ctx.start, NXT_APP_QUEUE_TEST_RACERS + 1);
    nxt_barrier_init(&ctx.done, NXT_APP_QUEUE_TEST_RACERS + 1);

    for (i = 0; i < NXT_APP_QUEUE_TEST_RACERS; i++) {
        workers[i].ctx = &ctx;
        workers[i].id = i;

        if (pthread_create(&threads[i], NULL, nxt_app_queue_race_worker,
                           &workers[i]) != 0)
        {
            __atomic_store_n(&ctx.stop, 1, __ATOMIC_RELEASE);
            nxt_barrier_wait(&ctx.start);

            while (i > 0) {
                pthread_join(threads[--i], NULL);
            }

            nxt_barrier_destroy(&ctx.start);
            nxt_barrier_destroy(&ctx.done);

            return 1;
        }
    }

    for (r = 0; r < rounds; r++) {
        __atomic_store_n(&ctx.notify_count, 0, __ATOMIC_RELAXED);

        nxt_barrier_wait(&ctx.start);
        nxt_barrier_wait(&ctx.done);

        if (__atomic_load_n(&ctx.notify_count, __ATOMIC_RELAXED) == 0) {
            zero_rounds++;
        }

        while (nxt_app_queue_recv(ctx.q, buf, &cookie) >= 0) {
            /* drain */
        }
    }

    __atomic_store_n(&ctx.stop, 1, __ATOMIC_RELEASE);
    nxt_barrier_wait(&ctx.start);

    for (i = 0; i < NXT_APP_QUEUE_TEST_RACERS; i++) {
        pthread_join(threads[i], NULL);
    }

    nxt_barrier_destroy(&ctx.start);
    nxt_barrier_destroy(&ctx.done);

    if (zero_rounds > 0) {
        fprintf(stderr, "concurrent race: %ld of %ld rounds had zero wakeups\n",
                zero_rounds, rounds);
        return 2;
    }

    return 0;
}


typedef struct {
    const char  *name;
    int         (*op)(void *q);
} nxt_app_queue_test_case_t;


static const nxt_app_queue_test_case_t  nxt_app_queue_test_cases[] = {
    { "app queue death recovery", nxt_app_queue_test_death_recovery },
    { "app queue depth", nxt_app_queue_test_depth },
    { "app queue burst", nxt_app_queue_test_burst },
    { "app queue late head", nxt_app_queue_test_late_head },
    { "app queue concurrent race", nxt_app_queue_test_concurrent_race },
};


static int
nxt_app_queue_test_child(void *data)
{
    nxt_app_queue_t                  *q;
    const nxt_app_queue_test_case_t  *tc;

    tc = data;

    alarm(5);

    q = nxt_zalloc(sizeof(nxt_app_queue_t));
    if (q == NULL) {
        return 2;
    }

    nxt_app_queue_init(q);

    return tc->op(q);
}


nxt_int_t
nxt_app_queue_test(nxt_thread_t *thr)
{
    int                              rc;
    nxt_uint_t                       i;
    const nxt_app_queue_test_case_t  *tc;

    nxt_thread_time_update(thr);

    for (i = 0; i < nxt_nitems(nxt_app_queue_test_cases); i++) {
        tc = &nxt_app_queue_test_cases[i];

        rc = nxt_test_in_child(thr, tc->name, nxt_app_queue_test_child,
                               (void *) tc);

        NXT_TEST_CHECK(thr->log, rc == 0, "app queue test: %s failed", tc->name);
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "app queue test passed");

    return NXT_OK;
}
