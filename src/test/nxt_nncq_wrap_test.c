
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * head, tail and the entries of the shared memory queues (src/nxt_nncq.h,
 * src/nxt_app_nncq.h) are 32-bit counters that wrap.  nxt_app_nncq.h took
 * the cycle after 32767 to be 32768, while the index after 2^32 - 1 is in
 * cycle 0, so past the wrap no entry had a cycle that an enqueue or a
 * dequeue accepted.
 *
 * Each case puts a queue into the state that enqueue and dequeue pairs leave
 * just before the wrap, and runs across it in a child under alarm().  The
 * pairs enqueue the value k % size at index k.  Then the last write into
 * slot k % size, for k in [tail - size, tail), stored the entry for index k:
 * nxt_*_new_entry(cycle(k), k).  The values of the free slots are never
 * returned, so this is the state of a queue that served 2^32 - 2 * size
 * pairs, whatever it carried.
 */

#include <nxt_main.h>
#include <nxt_nncq.h>
#include <nxt_app_queue.h>
#include "nxt_tests.h"


#define NXT_NNCQ_WRAP_TEST_LAPS  4


static void
nxt_nncq_wrap_test_nncq_set(nxt_nncq_t *q, nxt_nncq_atomic_t head,
    nxt_nncq_atomic_t tail)
{
    nxt_nncq_atomic_t  k;

    for (k = tail - NXT_NNCQ_SIZE; k != tail; k++) {
        q->entries[k % NXT_NNCQ_SIZE] = nxt_nncq_new_entry(q,
                                                  nxt_nncq_cycle(q, k), k);
    }

    q->head = head;
    q->tail = tail;
}


/*
 * Not nxt_app_nncq_new_entry(): before the fix it overflowed int from cycle
 * 16384 on.  For index k and value k % size the entry is k itself.
 */

static void
nxt_nncq_wrap_test_app_set(nxt_app_nncq_t *q, nxt_app_nncq_atomic_t head,
    nxt_app_nncq_atomic_t tail)
{
    nxt_app_nncq_atomic_t  k;

    for (k = tail - NXT_APP_NNCQ_SIZE; k != tail; k++) {
        q->entries[k % NXT_APP_NNCQ_SIZE] = k;
    }

    q->head = head;
    q->tail = tail;
}


static int
nxt_nncq_wrap_test_port_pairs(void *data)
{
    nxt_uint_t         n;
    nxt_nncq_t         *q;
    nxt_nncq_atomic_t  x;

    q = data;
    x = (nxt_nncq_atomic_t) 0 - 2 * NXT_NNCQ_SIZE;

    nxt_nncq_wrap_test_nncq_set(q, x, x);

    for (n = 0; n < NXT_NNCQ_WRAP_TEST_LAPS * NXT_NNCQ_SIZE; n++) {
        if (nxt_nncq_enqueue(q, n % NXT_NNCQ_SIZE) != NXT_OK
            || nxt_nncq_dequeue(q) != n % NXT_NNCQ_SIZE
            || nxt_nncq_dequeue(q) != nxt_nncq_empty(q))
        {
            return 1;
        }
    }

    return 0;
}


static int
nxt_nncq_wrap_test_app_pairs(void *data)
{
    nxt_uint_t             n;
    nxt_app_nncq_t         *q;
    nxt_app_nncq_atomic_t  x;

    q = data;
    x = (nxt_app_nncq_atomic_t) 0 - 2 * NXT_APP_NNCQ_SIZE;

    nxt_nncq_wrap_test_app_set(q, x, x);

    for (n = 0; n < NXT_NNCQ_WRAP_TEST_LAPS * NXT_APP_NNCQ_SIZE; n++) {
        if (nxt_app_nncq_enqueue(q, n % NXT_APP_NNCQ_SIZE) != NXT_OK
            || nxt_app_nncq_dequeue(q) != n % NXT_APP_NNCQ_SIZE
            || nxt_app_nncq_dequeue(q) != nxt_app_nncq_empty(q))
        {
            return 1;
        }
    }

    return 0;
}


/* A full queue that straddles the wrap: size entries in, size out, empty. */

static int
nxt_nncq_wrap_test_app_full(void *data)
{
    nxt_uint_t             n;
    nxt_app_nncq_t         *q;
    nxt_app_nncq_atomic_t  x;

    q = data;
    x = (nxt_app_nncq_atomic_t) 0 - NXT_APP_NNCQ_SIZE / 2;

    nxt_nncq_wrap_test_app_set(q, x, x);

    for (n = 0; n < NXT_APP_NNCQ_SIZE; n++) {
        if (nxt_app_nncq_enqueue(q, (nxt_app_nncq_atomic_t) (x + n))
            != NXT_OK)
        {
            return 1;
        }
    }

    for (n = 0; n < NXT_APP_NNCQ_SIZE; n++) {
        if (nxt_app_nncq_dequeue(q)
            != (nxt_app_nncq_atomic_t) (x + n) % NXT_APP_NNCQ_SIZE)
        {
            return 1;
        }
    }

    return nxt_app_nncq_dequeue(q) != nxt_app_nncq_empty(q);
}


/*
 * The application queue after 2^32 - 3 * size requests, one at a time:
 * free_items holds every item, one lap ahead of queue.  free_items wraps
 * first, on the enqueue of nxt_app_queue_recv() in libunit; queue wraps
 * size requests later, on the enqueue of nxt_app_queue_send() in the router.
 * Each recv must give its item back: before the fix the enqueue failed, the
 * item leaked, and once free_items ran dry every send failed.
 */

static int
nxt_nncq_wrap_test_app_queue(void *data)
{
    int                    notify;
    uint32_t               cookie, n, got;
    nxt_app_queue_t        *q;
    nxt_app_nncq_atomic_t  x, tail;
    uint8_t                buf[NXT_APP_QUEUE_MSG_SIZE];

    q = data;
    x = (nxt_app_nncq_atomic_t) 0 - 2 * NXT_APP_NNCQ_SIZE;

    nxt_nncq_wrap_test_app_set(&q->queue, x, x);
    nxt_nncq_wrap_test_app_set(&q->free_items, x, x + NXT_APP_NNCQ_SIZE);

    for (n = 0; n < NXT_NNCQ_WRAP_TEST_LAPS * NXT_APP_NNCQ_SIZE; n++) {
        if (nxt_app_queue_send(q, &n, sizeof(n), 1, &notify, &cookie)
            != NXT_OK)
        {
            return 1;
        }

        tail = q->free_items.tail;

        if (nxt_app_queue_recv(q, buf, &cookie) != sizeof(n)) {
            return 1;
        }

        nxt_memcpy(&got, buf, sizeof(got));

        if (got != n || q->free_items.tail != tail + 1) {
            return 1;
        }
    }

    return 0;
}


typedef struct {
    const char  *name;
    size_t      size;
    int         (*op)(void *q);
} nxt_nncq_wrap_test_case_t;


static const nxt_nncq_wrap_test_case_t  nxt_nncq_wrap_test_cases[] = {
    { "port nncq pairs", sizeof(nxt_nncq_t), nxt_nncq_wrap_test_port_pairs },
    { "app nncq pairs", sizeof(nxt_app_nncq_t), nxt_nncq_wrap_test_app_pairs },
    { "app nncq full", sizeof(nxt_app_nncq_t), nxt_nncq_wrap_test_app_full },
    { "app queue", sizeof(nxt_app_queue_t), nxt_nncq_wrap_test_app_queue },
};


static int
nxt_nncq_wrap_test_child(void *data)
{
    void                             *q;
    const nxt_nncq_wrap_test_case_t  *tc;

    tc = data;

    alarm(5);

    q = nxt_zalloc(tc->size);
    if (q == NULL) {
        return 2;
    }

    if (tc->size == sizeof(nxt_app_queue_t)) {
        nxt_app_queue_init(q);
    }

    return tc->op(q);
}


nxt_int_t
nxt_nncq_wrap_test(nxt_thread_t *thr)
{
    int                              rc;
    nxt_uint_t                       i;
    const nxt_nncq_wrap_test_case_t  *tc;

    nxt_thread_time_update(thr);

    for (i = 0; i < nxt_nitems(nxt_nncq_wrap_test_cases); i++) {
        tc = &nxt_nncq_wrap_test_cases[i];

        rc = nxt_test_in_child(thr, tc->name, nxt_nncq_wrap_test_child,
                               (void *) tc);

        NXT_TEST_CHECK(thr->log, rc == 0, "nncq wrap test: %s failed "
                       "across the index wrap", tc->name);
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nncq wrap test passed");

    return NXT_OK;
}
