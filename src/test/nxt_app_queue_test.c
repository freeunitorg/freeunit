
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Test for the shared application queue doorbell logic and depth tracking.
 */

#include <nxt_main.h>


/*
 * A hook in the claim window of nxt_app_queue_recv_claim(), after the claim
 * and before the slot goes back to the free list.  Only this file defines
 * the macro, so no shipped object calls the hook.
 */

static void  (*nxt_app_queue_test_claim_cb)(volatile void *q, uint32_t i);

#define NXT_APP_QUEUE_TEST_CLAIM_HOOK(q, i)                                   \
    do {                                                                      \
        if (nxt_app_queue_test_claim_cb != NULL) {                            \
            nxt_app_queue_test_claim_cb((volatile void *) (q),                \
                                        (uint32_t) (i));                      \
        }                                                                     \
    } while (0)

#include <nxt_app_queue.h>
#include "nxt_tests.h"
#include <pthread.h>


/*
 * The hang guard around the concurrent cases.  ThreadSanitizer slows the
 * queue loops enough that the ordinary bound fires on a healthy run; the
 * guard stays, with room.  GCC defines __SANITIZE_THREAD__; Clang needs
 * __has_feature().
 */

#if defined(__SANITIZE_THREAD__)
#define NXT_APP_QUEUE_TEST_TSAN  1
#elif (defined(__clang__) && defined(__has_feature))
#if __has_feature(thread_sanitizer)
#define NXT_APP_QUEUE_TEST_TSAN  1
#endif
#endif

#ifndef NXT_APP_QUEUE_TEST_TSAN
#define NXT_APP_QUEUE_TEST_TSAN  0
#endif


/*
 * The concurrent race case does a fixed number of rounds.  A loaded builder
 * can make each round slow, so the case has no bound on its total time.
 * It fails only when no round finishes within NXT_APP_QUEUE_TEST_STALL
 * seconds.  The same guard also covers the teardown after the last round.
 * There is no total cap on purpose.  The case can take as long as it keeps
 * finishing rounds, and the CI job timeout is the outer bound.
 */

#define NXT_APP_QUEUE_TEST_ROUNDS  20000

#if (NXT_APP_QUEUE_TEST_TSAN)
#define NXT_APP_QUEUE_TEST_STALL      60
#define NXT_APP_QUEUE_TEST_STALL_STR  "60"
#else
#define NXT_APP_QUEUE_TEST_STALL      10
#define NXT_APP_QUEUE_TEST_STALL_STR  "10"
#endif


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


/* The number of finished rounds.  The SIGALRM handler reads it. */
static volatile sig_atomic_t  nxt_app_queue_race_rounds;


/*
 * The SIGALRM handler of the concurrent race case.  It uses only
 * async-signal-safe calls: it formats the round count by hand, writes the
 * message with write() and exits with _exit().
 */
static void
nxt_app_queue_race_stalled(int signo)
{
    long        n;
    size_t      len;
    char        *p;
    char        num[24];
    static const char  head[] = "app queue concurrent race: no progress in "
                                NXT_APP_QUEUE_TEST_STALL_STR " s, ";
    static const char  tail[] = " rounds done\n";

    (void) signo;

    n = nxt_app_queue_race_rounds;
    p = num + sizeof(num);

    do {
        *(--p) = (char) ('0' + n % 10);
        n /= 10;
    } while (n != 0);

    len = num + sizeof(num) - p;

    (void) write(STDERR_FILENO, head, sizeof(head) - 1);
    (void) write(STDERR_FILENO, p, len);
    (void) write(STDERR_FILENO, tail, sizeof(tail) - 1);

    _exit(3);
}


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

        if (nxt_atomic_load_acquire(&ctx->stop)) {
            break;
        }

        /* The tracking value stands for a stream, and a stream is never 0. */
        notify = 0;
        (void) nxt_app_queue_send_bound(ctx->q, msg, sizeof(msg),
                                        (uint32_t) w->id + 1, &notify, &cookie,
                                        1);
        if (notify > 0) {
            (void) nxt_atomic_fetch_add(&ctx->notify_count, 1);
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
    const long                   rounds = NXT_APP_QUEUE_TEST_ROUNDS;
    int                          i;
    struct sigaction             sa;

    nxt_memzero(&sa, sizeof(sa));
    sa.sa_handler = nxt_app_queue_race_stalled;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGALRM, &sa, NULL) != 0) {
        return 3;
    }

    nxt_app_queue_race_rounds = 0;
    alarm(NXT_APP_QUEUE_TEST_STALL);

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
            nxt_atomic_store_release(&ctx.stop, 1);
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
        nxt_atomic_store(&ctx.notify_count, 0);

        nxt_barrier_wait(&ctx.start);
        nxt_barrier_wait(&ctx.done);

        if (nxt_atomic_load(&ctx.notify_count) == 0) {
            zero_rounds++;
        }

        while (nxt_app_queue_recv(ctx.q, buf, &cookie) >= 0) {
            /* drain */
        }

        /* The round finished: restart the stall guard. */
        nxt_app_queue_race_rounds = r + 1;
        alarm(NXT_APP_QUEUE_TEST_STALL);
    }

    nxt_atomic_store_release(&ctx.stop, 1);
    nxt_barrier_wait(&ctx.start);

    for (i = 0; i < NXT_APP_QUEUE_TEST_RACERS; i++) {
        pthread_join(threads[i], NULL);
    }

    nxt_barrier_destroy(&ctx.start);
    nxt_barrier_destroy(&ctx.done);

    alarm(0);

    if (zero_rounds > 0) {
        fprintf(stderr, "concurrent race: %ld of %ld rounds had zero wakeups\n",
                zero_rounds, rounds);
        return 2;
    }

    return 0;
}


/*
 * The worker claims a queued request before the slot of the request goes
 * back to the free list.  Otherwise the router can reuse the slot for a new
 * request in between, and the claim fails for both sides (#889).
 *
 * The queue is full, so the free list is empty.  The only slot that a send
 * can get in the claim window is the slot that is being claimed.  The hook
 * tries a send in that window: it must get NXT_AGAIN.
 */

static nxt_int_t  nxt_app_queue_claim_send_rc;
static uint32_t   nxt_app_queue_claim_send_cookie;
static uint32_t   nxt_app_queue_claim_hook_calls;


static void
nxt_app_queue_test_claim_window(volatile void *data, uint32_t i)
{
    uint32_t         cookie, tracking;
    nxt_app_queue_t  *q;
    uint8_t          msg[NXT_APP_QUEUE_MSG_SIZE];

    q = (nxt_app_queue_t *) data;

    nxt_app_queue_claim_hook_calls++;

    tracking = 0xC1A1u;
    nxt_memzero(msg, sizeof(msg));
    nxt_memcpy(msg, &tracking, sizeof(uint32_t));

    cookie = 0xFFFFFFFFu;

    nxt_app_queue_claim_send_rc = nxt_app_queue_send(q, msg, sizeof(msg),
                                                     tracking, NULL, &cookie);
    nxt_app_queue_claim_send_cookie = cookie;
}


static int
nxt_app_queue_test_claim_before_free(void *data)
{
    uint32_t         n, tracking, cookie;
    ssize_t          size;
    nxt_bool_t       claimed;
    nxt_app_queue_t  *q;
    uint8_t          msg[NXT_APP_QUEUE_MSG_SIZE];
    uint8_t          buf[NXT_APP_QUEUE_MSG_SIZE];

    q = data;
    nxt_memzero(msg, sizeof(msg));

    /* The first four bytes of an item are its tracking value. */

    for (n = 0; n < NXT_APP_QUEUE_SIZE; n++) {
        tracking = n + 1;
        nxt_memcpy(msg, &tracking, sizeof(uint32_t));

        if (nxt_app_queue_send(q, msg, sizeof(msg), tracking, NULL, &cookie)
            != NXT_OK)
        {
            return 1;
        }
    }

    nxt_app_queue_claim_send_rc = NXT_OK;
    nxt_app_queue_claim_send_cookie = 0;
    nxt_app_queue_claim_hook_calls = 0;
    nxt_app_queue_test_claim_cb = nxt_app_queue_test_claim_window;

    claimed = 0;
    size = nxt_app_queue_recv_claim(q, buf, &cookie, &claimed);

    nxt_app_queue_test_claim_cb = NULL;

    if (size != (ssize_t) sizeof(msg) || nxt_app_queue_claim_hook_calls != 1) {
        return 2;
    }

    if (nxt_app_queue_claim_send_rc != NXT_AGAIN) {
        nxt_log_error(NXT_LOG_ALERT, &nxt_main_log,
                      "app queue claim: claimed slot %uD was reusable by a "
                      "send in the claim window (send cookie %uD)",
                      cookie, nxt_app_queue_claim_send_cookie);
        return 3;
    }

    if (!claimed) {
        return 4;
    }

    /* The router cannot take a claimed request back. */

    tracking = 1;

    if (nxt_app_queue_cancel(q, cookie, tracking)) {
        return 5;
    }

    return 0;
}


/*
 * A read from an empty queue sets "claimed" to 0: the out-parameter has a
 * value on every return.
 */

static int
nxt_app_queue_test_claim_empty(void *data)
{
    uint32_t         cookie;
    nxt_bool_t       claimed;
    nxt_app_queue_t  *q;
    uint8_t          buf[NXT_APP_QUEUE_MSG_SIZE];

    q = data;

    claimed = 0x5A;

    if (nxt_app_queue_recv_claim(q, buf, &cookie, &claimed) != -1) {
        return 1;
    }

    if (claimed != 0) {
        return 2;
    }

    return 0;
}


/*
 * Every legal transition of a slot, and the illegal ones, which fail and
 * change nothing.  The message carries its stream in its first four bytes,
 * as the router's nxt_port_msg_t does, so nxt_app_queue_recv_claim() can
 * claim it.
 */
static int
nxt_app_queue_test_slot_states(void *data)
{
    uint32_t         cookie, c1, c2, c3, stream;
    nxt_bool_t       claimed;
    nxt_app_queue_t  *q;
    uint8_t          msg[NXT_APP_QUEUE_MSG_SIZE];
    uint8_t          buf[NXT_APP_QUEUE_MSG_SIZE];

    q = data;
    nxt_memzero(msg, sizeof(msg));

    /* free -> queued, with stream 7. */
    stream = 7;
    nxt_memcpy(msg, &stream, sizeof(stream));

    if (nxt_app_queue_send(q, msg, sizeof(msg), 7, NULL, &c1) != NXT_OK) {
        return 1;
    }

    if (nxt_app_queue_slot_state(q, c1, 7) != NXT_APP_QUEUE_SLOT_QUEUED) {
        return 2;
    }

    /* The slot is not the slot of stream 8. */
    if (nxt_app_queue_slot_state(q, c1, 8) != NXT_APP_QUEUE_SLOT_IDLE) {
        return 3;
    }

    /* A retraction and a claim with the wrong stream fail, nothing moves. */
    if (nxt_app_queue_slot_retract(q, c1, 8)
        || nxt_app_queue_slot_claim(q, c1, 8)
        || nxt_app_queue_slot_state(q, c1, 7) != NXT_APP_QUEUE_SLOT_QUEUED)
    {
        return 4;
    }

    /* dequeued -> claimed -> free, by the reader. */
    if (nxt_app_queue_recv_claim(q, buf, &cookie, &claimed) != sizeof(msg)
        || cookie != c1 || !claimed)
    {
        return 5;
    }

    if (nxt_app_queue_slot_state(q, c1, 7) != NXT_APP_QUEUE_SLOT_IDLE) {
        return 6;
    }

    /* After a claim, a retraction fails; so does a second claim. */
    if (nxt_app_queue_cancel(q, c1, 7) || nxt_app_queue_slot_claim(q, c1, 7)) {
        return 7;
    }

    /* queued -> retracted, by the router, with stream 9. */
    stream = 9;
    nxt_memcpy(msg, &stream, sizeof(stream));

    if (nxt_app_queue_send(q, msg, sizeof(msg), 9, NULL, &c2) != NXT_OK) {
        return 8;
    }

    if (!nxt_app_queue_cancel(q, c2, 9)
        || nxt_app_queue_slot_state(q, c2, 9) != NXT_APP_QUEUE_SLOT_IDLE)
    {
        return 9;
    }

    /* A second retraction fails: the answer is given once. */
    if (nxt_app_queue_cancel(q, c2, 9)) {
        return 10;
    }

    /* The reader still dequeues the item, and its claim loses. */
    if (nxt_app_queue_recv_claim(q, buf, &cookie, &claimed) != sizeof(msg)
        || cookie != c2 || claimed)
    {
        return 11;
    }

    /*
     * A reader with no claim frees the slot with the stream still in it.
     * This is why free -> queued is a store: the next send reuses the slot
     * whatever the word holds.
     */
    stream = 10;
    nxt_memcpy(msg, &stream, sizeof(stream));

    if (nxt_app_queue_send(q, msg, sizeof(msg), 10, NULL, &c3) != NXT_OK) {
        return 12;
    }

    if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(msg) || cookie != c3) {
        return 13;
    }

    if (nxt_app_queue_slot_state(q, c3, 10) != NXT_APP_QUEUE_SLOT_QUEUED) {
        return 14;
    }

    stream = 11;
    nxt_memcpy(msg, &stream, sizeof(stream));

    if (nxt_app_queue_send(q, msg, sizeof(msg), 11, NULL, &cookie) != NXT_OK) {
        return 15;
    }

    if (nxt_app_queue_slot_state(q, cookie, 11) != NXT_APP_QUEUE_SLOT_QUEUED) {
        return 16;
    }

    if (nxt_app_queue_recv_claim(q, buf, &cookie, &claimed) != sizeof(msg)
        || !claimed || nxt_app_queue_depth(q) != 0)
    {
        return 17;
    }

    return 0;
}


typedef struct {
    nxt_app_queue_t  *q;
    nxt_barrier_t    start;
    nxt_barrier_t    done;
    nxt_atomic_t     stop;
    nxt_atomic_t     wins;
    uint32_t         cookie;
    uint32_t         stream;
} nxt_app_queue_take_ctx_t;


static void *
nxt_app_queue_take_worker(void *arg)
{
    nxt_app_queue_take_ctx_t  *ctx;

    ctx = arg;

    for ( ;; ) {
        nxt_barrier_wait(&ctx->start);

        if (nxt_atomic_load_acquire(&ctx->stop)) {
            break;
        }

        if (nxt_app_queue_slot_take(ctx->q, ctx->cookie, ctx->stream)) {
            (void) nxt_atomic_fetch_add(&ctx->wins, 1);
        }

        nxt_barrier_wait(&ctx->done);
    }

    return NULL;
}


#define NXT_APP_QUEUE_TEST_TAKE_ROUNDS  2000


/*
 * The claim of a worker and the retraction of the router are the same
 * compare-and-set on one slot.  Whatever the number of takers, exactly one
 * wins each round, and the slot is idle after it.
 */
static int
nxt_app_queue_test_take_race(void *data)
{
    int                       i, rc;
    long                      r;
    pthread_t                 threads[NXT_APP_QUEUE_TEST_RACERS];
    nxt_app_queue_take_ctx_t  ctx;
    uint8_t                   msg[NXT_APP_QUEUE_MSG_SIZE];
    uint8_t                   buf[NXT_APP_QUEUE_MSG_SIZE];
    uint32_t                  cookie;

    nxt_memzero(&ctx, sizeof(ctx));
    nxt_memzero(msg, sizeof(msg));
    ctx.q = data;
    rc = 0;

    nxt_barrier_init(&ctx.start, NXT_APP_QUEUE_TEST_RACERS + 1);
    nxt_barrier_init(&ctx.done, NXT_APP_QUEUE_TEST_RACERS + 1);

    for (i = 0; i < NXT_APP_QUEUE_TEST_RACERS; i++) {
        if (pthread_create(&threads[i], NULL, nxt_app_queue_take_worker, &ctx)
            != 0)
        {
            /* The child exits; its threads end with it. */
            return 1;
        }
    }

    for (r = 0; r < NXT_APP_QUEUE_TEST_TAKE_ROUNDS; r++) {
        ctx.stream = (uint32_t) r + 1;
        nxt_memcpy(msg, &ctx.stream, sizeof(ctx.stream));

        if (nxt_app_queue_send(ctx.q, msg, sizeof(msg), ctx.stream, NULL,
                               &ctx.cookie)
            != NXT_OK)
        {
            rc = 2;
            goto stop;
        }

        nxt_atomic_store(&ctx.wins, 0);

        nxt_barrier_wait(&ctx.start);
        nxt_barrier_wait(&ctx.done);

        if (nxt_atomic_load(&ctx.wins) != 1
            || nxt_app_queue_slot_state(ctx.q, ctx.cookie, ctx.stream)
               != NXT_APP_QUEUE_SLOT_IDLE)
        {
            rc = 3;
            goto stop;
        }

        /* The item is still in the queue; the reader finds it taken. */
        if (nxt_app_queue_recv(ctx.q, buf, &cookie) != sizeof(msg)
            || cookie != ctx.cookie)
        {
            rc = 4;
            goto stop;
        }
    }

stop:

    nxt_atomic_store_release(&ctx.stop, 1);
    nxt_barrier_wait(&ctx.start);

    while (i > 0) {
        pthread_join(threads[--i], NULL);
    }

    nxt_barrier_destroy(&ctx.start);
    nxt_barrier_destroy(&ctx.done);

    return rc;
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
    { "app queue claim before free", nxt_app_queue_test_claim_before_free },
    { "app queue claim on an empty queue", nxt_app_queue_test_claim_empty },
    { "app queue slot states", nxt_app_queue_test_slot_states },
    { "app queue take race", nxt_app_queue_test_take_race },
};


static int
nxt_app_queue_test_child(void *data)
{
    nxt_app_queue_t                  *q;
    const nxt_app_queue_test_case_t  *tc;

    tc = data;

    /*
     * A hang guard for the whole case.  The concurrent race case replaces
     * it with a guard that it restarts after each round.
     */
    alarm(NXT_APP_QUEUE_TEST_TSAN ? 60 : 30);

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
