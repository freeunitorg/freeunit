
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_PORT_QUEUE_H_INCLUDED_
#define _NXT_PORT_QUEUE_H_INCLUDED_


#include <nxt_nncq.h>


/* Using Numeric Naive Circular Queue as a backend. */

#define NXT_PORT_QUEUE_SIZE      NXT_NNCQ_SIZE
#define NXT_PORT_QUEUE_MSG_SIZE  31


typedef struct {
    uint8_t   size;
    uint8_t   data[NXT_PORT_QUEUE_MSG_SIZE];
} nxt_port_queue_item_t;


typedef struct {
    nxt_nncq_atomic_t      nitems;
    nxt_nncq_t             free_items;
    nxt_nncq_t             queue;
    nxt_port_queue_item_t  items[NXT_PORT_QUEUE_SIZE];
} nxt_port_queue_t;


nxt_inline void
nxt_port_queue_init(nxt_port_queue_t volatile *q)
{
    nxt_nncq_atomic_t  i;

    nxt_nncq_init(&q->free_items);
    nxt_nncq_init(&q->queue);

    for (i = 0; i < NXT_PORT_QUEUE_SIZE; i++) {
        nxt_nncq_enqueue(&q->free_items, i);
    }

    q->nitems = 0;
}


nxt_inline nxt_int_t
nxt_port_queue_send(nxt_port_queue_t volatile *q, const void *p, uint8_t size,
    int *notify)
{
    nxt_nncq_atomic_t      i;
    nxt_port_queue_item_t  *qi;

    /*
     * qi->data is NXT_PORT_QUEUE_MSG_SIZE bytes; refuse to enqueue an
     * over-sized item rather than overwriting the adjacent queue slot.
     * Callers must respect the queue's per-item budget.
     */
    if (nxt_slow_path(size > NXT_PORT_QUEUE_MSG_SIZE)) {
        *notify = 0;
        return NXT_ERROR;
    }

    i = nxt_nncq_dequeue(&q->free_items);
    if (i == nxt_nncq_empty(&q->free_items)) {
        *notify = 0;
        return NXT_AGAIN;
    }

    qi = (nxt_port_queue_item_t *) &q->items[i];

    qi->size = size;
    nxt_memcpy(qi->data, p, size);

    nxt_nncq_enqueue(&q->queue, i);

    i = nxt_atomic_fetch_add(&q->nitems, 1);

    *notify = (i == 0);

    return NXT_OK;
}


nxt_inline ssize_t
nxt_port_queue_recv(nxt_port_queue_t volatile *q, void *p)
{
    size_t                 size;
    nxt_nncq_atomic_t      i;
    nxt_port_queue_item_t  *qi;

    i = nxt_nncq_dequeue(&q->queue);
    if (i == nxt_nncq_empty(&q->queue)) {
        return -1;
    }

    qi = (nxt_port_queue_item_t *) &q->items[i];

    /*
     * qi lives in shared memory that the peer can write.  Cap qi->size at
     * the slot's data bound (NXT_PORT_QUEUE_MSG_SIZE) before the memcpy so
     * a poisoned item cannot overflow the caller's receive buffer, which
     * is sized for NXT_PORT_QUEUE_MSG_SIZE.  Read from the volatile queue
     * structure directly so the compiler cannot fold the bound-check and
     * the memcpy length into two separate reads of qi->size that a
     * concurrent peer write could make disagree.
     */
    size = q->items[i].size;
    if (nxt_slow_path(size > NXT_PORT_QUEUE_MSG_SIZE)) {
        size = NXT_PORT_QUEUE_MSG_SIZE;
    }

    nxt_memcpy(p, qi->data, size);

    nxt_nncq_enqueue(&q->free_items, i);

    nxt_atomic_fetch_add(&q->nitems, -1);

    return size;
}


#endif /* _NXT_PORT_QUEUE_H_INCLUDED_ */
