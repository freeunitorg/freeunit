
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
    nxt_atomic_store(&qi->tracking, tracking);
    *cookie = i;

    /* Before the enqueue: a worker can dequeue as soon as the slot is in. */
    NXT_USDT(queue__enqueue, i, tracking);

    if (nxt_slow_path(nxt_app_nncq_enqueue_pos(&q->queue, i, &t) != NXT_OK)) {
        NXT_USDT(queue__enqueue__fail, i, tracking);

        (void) nxt_app_nncq_enqueue(&q->free_items, i);

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


nxt_inline nxt_bool_t
nxt_app_queue_cancel(nxt_app_queue_t volatile *q, uint32_t cookie,
    uint32_t tracking)
{
    nxt_app_queue_item_t  *qi;

    qi = (nxt_app_queue_item_t *) &q->items[cookie];

    return nxt_atomic_cmp_set(&qi->tracking, tracking, 0);
}


nxt_inline ssize_t
nxt_app_queue_recv(nxt_app_queue_t volatile *q, void *p, uint32_t *cookie)
{
    size_t                 size;
    nxt_app_queue_item_t   *qi;
    nxt_app_nncq_atomic_t  i;

    i = nxt_app_nncq_dequeue(&q->queue);
    if (i == nxt_app_nncq_empty(&q->queue)) {
        *cookie = 0;
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

    (void) nxt_app_nncq_enqueue(&q->free_items, i);

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


#endif /* _NXT_APP_QUEUE_H_INCLUDED_ */
