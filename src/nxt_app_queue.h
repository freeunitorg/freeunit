
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_APP_QUEUE_H_INCLUDED_
#define _NXT_APP_QUEUE_H_INCLUDED_


#include <nxt_app_nncq.h>
#include <nxt_usdt.h>


/* Using Numeric Naive Circular Queue as a backend. */

#define NXT_APP_QUEUE_SIZE      NXT_APP_NNCQ_SIZE
#define NXT_APP_QUEUE_MSG_SIZE  31

typedef struct {
    uint8_t   size;
    uint8_t   data[NXT_APP_QUEUE_MSG_SIZE];
    uint32_t  tracking;
} nxt_app_queue_item_t;


typedef struct {
    /*
     * One-wake flag.  With depth_bound 1 (one reader) the router rings
     * only on its 0->1 transition, and the receiver clears it
     * (nxt_app_queue_notification_received()).  With more readers the
     * doorbell is decided by queue position (nxt_app_queue_send_bound()),
     * so up to depth_bound READ_QUEUE datagrams can be outstanding; the
     * receiver still clears the flag for an older router that gates on it.
     */
    nxt_app_nncq_atomic_t  notified;
    nxt_app_nncq_t         free_items;
    nxt_app_nncq_t         queue;
    nxt_app_queue_item_t   items[NXT_APP_QUEUE_SIZE];
} nxt_app_queue_t;


nxt_inline void
nxt_app_queue_init(nxt_app_queue_t volatile *q)
{
    nxt_app_nncq_atomic_t  i;

    nxt_app_nncq_init(&q->free_items);
    nxt_app_nncq_init(&q->queue);

    for (i = 0; i < NXT_APP_QUEUE_SIZE; i++) {
        nxt_app_nncq_enqueue(&q->free_items, i);
    }

    q->notified = 0;
}


nxt_inline uint32_t
nxt_app_queue_depth(nxt_app_queue_t volatile *q)
{
    return (uint32_t) (nxt_app_nncq_tail(&q->queue)
                       - nxt_app_nncq_head(&q->queue));
}


/*
 * The state of a slot.
 *
 * The tracking word of a slot is shared between the router and every worker
 * of the application, so what it holds is part of the shared memory
 * protocol (nxt_app_queue_item_t in tools/perf/layout-structs.txt), and
 * nothing here changes it.  The word holds one of two kinds of value:
 *
 *   NXT_APP_QUEUE_TRACKING_IDLE (0): no request owns the slot.  The slot is
 *   on free_items, or a worker claimed it, or the router retracted it.
 *
 *   a stream number: the request with this stream owns the slot.  The slot
 *   is in the queue, or a worker dequeued it and has not claimed it yet.  A
 *   stream is never 0 (nxt_port_rpc_ex_stream()).
 *
 * So the word tells NXT_APP_QUEUE_SLOT_QUEUED from NXT_APP_QUEUE_SLOT_IDLE,
 * and no more.  The finer states are kept by the side that made the
 * transition: the worker knows that its claim won, the router keeps the
 * answer of its retraction in nxt_msg_info_t.cancel, and free_items says
 * whether an idle slot can be reused.  The transitions are below, one
 * function each, and docs/design/ipc-request-handoff.md has the whole state
 * machine with the steps that follow this one.
 */

#define NXT_APP_QUEUE_TRACKING_IDLE  0

typedef enum {
    NXT_APP_QUEUE_SLOT_IDLE = 0,
    NXT_APP_QUEUE_SLOT_QUEUED,
} nxt_app_queue_slot_state_t;


/*
 * A check on the caller's own values: a slot index that came from one of
 * the two rings, and a stream that the router made.  No peer can fail one
 * through shared memory.  libunit does not link the thread context that
 * nxt_assert() needs, and this header is part of libunit, so a failed check
 * aborts without a log line.
 */

#if (NXT_DEBUG)
#define nxt_app_queue_assert(c)                                               \
    do {                                                                      \
        if (nxt_slow_path(!(c))) {                                            \
            nxt_abort();                                                      \
        }                                                                     \
    } while (0)
#else
#define nxt_app_queue_assert(c)
#endif


nxt_inline nxt_app_queue_slot_state_t
nxt_app_queue_slot_state(nxt_app_queue_t volatile *q, uint32_t cookie,
    uint32_t stream)
{
    nxt_app_queue_item_t  *qi;

    nxt_app_queue_assert(cookie < NXT_APP_QUEUE_SIZE);
    nxt_app_queue_assert(stream != NXT_APP_QUEUE_TRACKING_IDLE);

    qi = (nxt_app_queue_item_t *) &q->items[cookie];

    return (nxt_atomic_load(&qi->tracking) == stream)
           ? NXT_APP_QUEUE_SLOT_QUEUED : NXT_APP_QUEUE_SLOT_IDLE;
}


/*
 * free -> queued.  The router, on a slot it took from free_items, before
 * the slot goes into the queue.
 *
 * A store, not a compare-and-set.  The word of a slot on free_items is not
 * always idle: a reader that takes the slot with nxt_app_queue_recv(), with
 * no claim, frees it with the stream still in it, and a libunit from before
 * nxt_app_queue_recv_claim() claims after the free.  A compare-and-set from
 * idle would refuse those slots.  It becomes one when every reader claims
 * before it frees (the design document, step 2).
 */

nxt_inline void
nxt_app_queue_slot_queue(nxt_app_queue_t volatile *q, uint32_t cookie,
    uint32_t stream)
{
    nxt_app_queue_item_t  *qi;

    nxt_app_queue_assert(cookie < NXT_APP_QUEUE_SIZE);
    nxt_app_queue_assert(stream != NXT_APP_QUEUE_TRACKING_IDLE);

    qi = (nxt_app_queue_item_t *) &q->items[cookie];

    nxt_atomic_store(&qi->tracking, stream);
}


/*
 * queued -> idle, the one compare-and-set that both sides run: the worker
 * to claim the request it dequeued, the router to retract a request it
 * queued.  Exactly one of the two wins, and the loser learns that the other
 * side owns the request.  The answer is the only record of who won, because
 * both outcomes leave the word idle, so each side asks once per request.
 * A compare-and-set with the wrong stream fails and changes nothing.
 */

nxt_inline nxt_bool_t
nxt_app_queue_slot_take(nxt_app_queue_t volatile *q, uint32_t cookie,
    uint32_t stream)
{
    nxt_app_queue_item_t  *qi;

    nxt_app_queue_assert(cookie < NXT_APP_QUEUE_SIZE);

    qi = (nxt_app_queue_item_t *) &q->items[cookie];

    return nxt_atomic_cmp_set(&qi->tracking, stream,
                              NXT_APP_QUEUE_TRACKING_IDLE);
}


/*
 * dequeued -> claimed.  The worker, with the stream it read from the item,
 * before the slot goes back to free_items.  The stream is not checked
 * against 0 here: it comes from shared memory that a sibling process can
 * write, and a debug abort on a poisoned item would be a new behaviour.
 */

nxt_inline nxt_bool_t
nxt_app_queue_slot_claim(nxt_app_queue_t volatile *q, uint32_t cookie,
    uint32_t stream)
{
    return nxt_app_queue_slot_take(q, cookie, stream);
}


/*
 * queued -> retracted.  The router, with its own stream, at most once per
 * request: nxt_router_msg_retract() keeps the answer.
 */

nxt_inline nxt_bool_t
nxt_app_queue_slot_retract(nxt_app_queue_t volatile *q, uint32_t cookie,
    uint32_t stream)
{
    nxt_app_queue_assert(stream != NXT_APP_QUEUE_TRACKING_IDLE);

    return nxt_app_queue_slot_take(q, cookie, stream);
}


/*
 * claimed | retracted -> free.  The reader that dequeued the slot, after
 * its claim, whatever the answer was; the router too, when its enqueue
 * failed.  The word is not checked: see nxt_app_queue_slot_queue().
 */

nxt_inline void
nxt_app_queue_slot_free(nxt_app_queue_t volatile *q, uint32_t cookie)
{
    nxt_app_queue_assert(cookie < NXT_APP_QUEUE_SIZE);

    (void) nxt_app_nncq_enqueue(&q->free_items, cookie);
}


/*
 * Doorbell decision for a producer whose item landed at queue position t,
 * given a (possibly late) sample of head.  pos < depth_bound rings so up to
 * depth_bound idle workers get woken.  pos < 0 must also ring: a concurrent
 * producer enqueued t + 1 and rang, a worker then consumed slot t before this
 * producer sampled head, and t + 1 is now queued with nobody to wake a second
 * idle worker.  An extra doorbell is harmless; a missing one stalls.
 */
nxt_inline int
nxt_app_queue_notify_decision(int32_t pos, uint32_t depth_bound)
{
    return (pos < 0 || (uint32_t) pos < depth_bound);
}


nxt_inline nxt_int_t
nxt_app_queue_send_bound(nxt_app_queue_t volatile *q, const void *p,
    uint8_t size, uint32_t tracking, int *notify, uint32_t *cookie,
    uint32_t depth_bound)
{
    int                    n;
    int32_t                pos;
    nxt_app_queue_item_t   *qi;
    nxt_app_nncq_atomic_t  i, t;

    /*
     * qi->data is NXT_APP_QUEUE_MSG_SIZE bytes; refuse to enqueue an
     * over-sized item rather than overwriting the adjacent queue slot.
     * Callers must respect the queue's per-item budget.
     */
    if (nxt_slow_path(size > NXT_APP_QUEUE_MSG_SIZE)) {
        return NXT_ERROR;
    }

    if (nxt_slow_path(depth_bound == 0)) {
        depth_bound = 1;
    }

    i = nxt_app_nncq_dequeue(&q->free_items);
    if (i == nxt_app_nncq_empty(&q->free_items)) {
        return NXT_AGAIN;
    }

    qi = (nxt_app_queue_item_t *) &q->items[i];

    qi->size = size;
    nxt_memcpy(qi->data, p, size);
    nxt_app_queue_slot_queue(q, i, tracking);
    *cookie = i;

    /* Before the enqueue: a worker can dequeue as soon as the slot is in. */
    NXT_USDT(queue__enqueue, i, tracking);

    if (nxt_slow_path(nxt_app_nncq_enqueue_pos(&q->queue, i, &t) != NXT_OK)) {
        NXT_USDT(queue__enqueue__fail, i, tracking);

        nxt_app_queue_slot_free(q, i);

        return NXT_ERROR;
    }

    if (depth_bound == 1) {
        /*
         * One reader: coalesce on the flag, ring only until the reader
         * takes the notification.  libunit drains the queue before it
         * sleeps (nxt_unit_process_port_msg() returns OK after a read).
         */
        n = nxt_atomic_cmp_set(&q->notified, 0, 1);

    } else {
        pos = (int32_t) (t - nxt_app_nncq_head(&q->queue));
        n = nxt_app_queue_notify_decision(pos, depth_bound);
    }

    if (notify != NULL) {
        *notify = n;
    }

    return NXT_OK;
}


nxt_inline nxt_int_t
nxt_app_queue_send(nxt_app_queue_t volatile *q, const void *p,
    uint8_t size, uint32_t tracking, int *notify, uint32_t *cookie)
{
    return nxt_app_queue_send_bound(q, p, size, tracking, notify, cookie, 1);
}


nxt_inline void
nxt_app_queue_notification_received(nxt_app_queue_t volatile *q)
{
    /*
     * Clear the legacy flag for compatibility with pre-change unitd
     * routers (e.g. an application rebuilt against this libunit run by an
     * older router that still gates notifications with q->notified).
     */
    (void) nxt_atomic_cmp_set(&q->notified, 1, 0);
}


/* The router's retraction; the name the router calls it by. */

nxt_inline nxt_bool_t
nxt_app_queue_cancel(nxt_app_queue_t volatile *q, uint32_t cookie,
    uint32_t tracking)
{
    return nxt_app_queue_slot_retract(q, cookie, tracking);
}


/*
 * Take the next item.  When "claimed" is not NULL, also claim the item
 * before its slot goes back to the free list: the first four bytes of the
 * item are the tracking value it was sent with (the stream of the
 * nxt_port_msg_t), and the CAS on the slot's tracking word runs while the
 * slot is still ours.  Once the slot is free the router may reuse it for a
 * new request with a new tracking value; a CAS after that would fail, the
 * reader would drop the request as cancelled, and the router's own CAS in
 * nxt_app_queue_cancel() would fail too and report a claim, so nobody would
 * answer the request.
 */

nxt_inline ssize_t
nxt_app_queue_recv_claim(nxt_app_queue_t volatile *q, void *p,
    uint32_t *cookie, nxt_bool_t *claimed)
{
    uint32_t               tracking;
    size_t                 size;
    nxt_app_queue_item_t   *qi;
    nxt_app_nncq_atomic_t  i;

    i = nxt_app_nncq_dequeue(&q->queue);
    if (i == nxt_app_nncq_empty(&q->queue)) {
        *cookie = 0;

        if (claimed != NULL) {
            *claimed = 0;
        }

        return -1;
    }

    qi = (nxt_app_queue_item_t *) &q->items[i];

    /*
     * qi lives in shared memory that the peer can write.  Cap qi->size at
     * the slot's data bound (NXT_APP_QUEUE_MSG_SIZE) before the memcpy so
     * a poisoned item cannot overflow the caller's receive buffer, which
     * is sized for NXT_APP_QUEUE_MSG_SIZE.  Read from the volatile queue
     * structure directly so the compiler cannot fold the bound-check and
     * the memcpy length into two separate reads of qi->size that a
     * concurrent peer write could make disagree.
     */
    size = q->items[i].size;
    if (nxt_slow_path(size > NXT_APP_QUEUE_MSG_SIZE)) {
        size = NXT_APP_QUEUE_MSG_SIZE;
    }

    nxt_memcpy(p, qi->data, size);
    *cookie = i;

    if (claimed != NULL) {
        *claimed = 0;

        if (size >= sizeof(uint32_t)) {
            nxt_memcpy(&tracking, p, sizeof(uint32_t));
            *claimed = nxt_app_queue_slot_claim(q, i, tracking);
        }
    }

#if (NXT_TESTS) && defined(NXT_APP_QUEUE_TEST_CLAIM_HOOK)
    /*
     * The claim window: the item is claimed, its slot is not free yet.
     * Only src/test/nxt_app_queue_test.c defines the macro.
     */
    NXT_APP_QUEUE_TEST_CLAIM_HOOK(q, i);
#endif

    nxt_app_queue_slot_free(q, i);

    /*
     * Re-arm the one-reader doorbell once the queue is drained, whether or
     * not the READ_QUEUE that set the flag was read.  A worker that is
     * already awake, or that dies before it reads the datagram, would
     * otherwise leave "notified" at 1 and every later request in silence.
     * A producer that enqueues between the dequeue and this clear sees the
     * flag still set and does not ring; the caller re-checks the queue
     * before it sleeps (nxt_unit_process_port_msg() returns OK after a
     * read), so that item is still found.
     */
    if (nxt_app_queue_depth(q) == 0) {
        (void) nxt_atomic_cmp_set(&q->notified, 1, 0);
    }

    return size;
}


nxt_inline ssize_t
nxt_app_queue_recv(nxt_app_queue_t volatile *q, void *p, uint32_t *cookie)
{
    return nxt_app_queue_recv_claim(q, p, cookie, NULL);
}


#endif /* _NXT_APP_QUEUE_H_INCLUDED_ */
