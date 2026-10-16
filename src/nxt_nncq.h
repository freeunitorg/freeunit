
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_NNCQ_H_INCLUDED_
#define _NXT_NNCQ_H_INCLUDED_


/* Numeric Naive Circular Queue */

#define NXT_NNCQ_SIZE  16384

/*
 * The peer maps the queue writable and can put a foreign cycle under head
 * or tail.  Each loop below retries at most this many times, so the peer
 * cannot make this side spin for ever.  Legitimate contention needs far
 * fewer: each retry means another party made progress.
 */
#define NXT_NNCQ_RETRIES  NXT_NNCQ_SIZE

typedef uint32_t nxt_nncq_atomic_t;
typedef uint16_t nxt_nncq_cycle_t;

/*
 * head, tail and the entries wrap at the width of nxt_nncq_atomic_t.  The
 * cycle of an index i is i / NXT_NNCQ_SIZE, truncated to nxt_nncq_cycle_t,
 * and nxt_nncq_next_cycle() wraps with that type.  This is correct across
 * the wrap only while the number of cycles is a multiple of the range of
 * nxt_nncq_cycle_t: 2^32 / 16384 = 2^18 is.  nxt_app_nncq.h does not have
 * this property and counts its cycles explicitly.
 */
nxt_static_assert((nxt_nncq_atomic_t) -1 % NXT_NNCQ_SIZE == NXT_NNCQ_SIZE - 1,
                  "NXT_NNCQ_SIZE must divide the range of the index");
nxt_static_assert(((nxt_nncq_atomic_t) -1 / NXT_NNCQ_SIZE + 1)
                  % ((nxt_nncq_atomic_t) (nxt_nncq_cycle_t) -1 + 1) == 0,
                  "the cycles must wrap with nxt_nncq_cycle_t");

typedef struct {
    nxt_nncq_atomic_t  head;
    nxt_nncq_atomic_t  entries[NXT_NNCQ_SIZE];
    nxt_nncq_atomic_t  tail;
} nxt_nncq_t;


static inline nxt_nncq_atomic_t
nxt_nncq_head(nxt_nncq_t const volatile *q)
{
    return nxt_atomic_load(&q->head);
}


static inline nxt_nncq_atomic_t
nxt_nncq_tail(nxt_nncq_t const volatile *q)
{
    return nxt_atomic_load(&q->tail);
}


static inline void
nxt_nncq_tail_cmp_inc(nxt_nncq_t volatile *q, nxt_nncq_atomic_t t)
{
    nxt_atomic_cmp_set(&q->tail, t, t + 1);
}


static inline nxt_nncq_atomic_t
nxt_nncq_index(nxt_nncq_t const volatile *q, nxt_nncq_atomic_t i)
{
    return i % NXT_NNCQ_SIZE;
}


static inline nxt_nncq_atomic_t
nxt_nncq_map(nxt_nncq_t const volatile *q, nxt_nncq_atomic_t i)
{
    return i % NXT_NNCQ_SIZE;
}


static inline nxt_nncq_cycle_t
nxt_nncq_cycle(nxt_nncq_t const volatile *q, nxt_nncq_atomic_t i)
{
    return i / NXT_NNCQ_SIZE;
}


static inline nxt_nncq_cycle_t
nxt_nncq_next_cycle(nxt_nncq_t const volatile *q, nxt_nncq_cycle_t i)
{
    return i + 1;
}


static inline nxt_nncq_atomic_t
nxt_nncq_new_entry(nxt_nncq_t const volatile *q, nxt_nncq_cycle_t cycle,
    nxt_nncq_atomic_t i)
{
    return (nxt_nncq_atomic_t) cycle * NXT_NNCQ_SIZE + (i % NXT_NNCQ_SIZE);
}


static inline nxt_nncq_atomic_t
nxt_nncq_empty(nxt_nncq_t const volatile *q)
{
    return NXT_NNCQ_SIZE;
}


static inline void
nxt_nncq_init(nxt_nncq_t volatile *q)
{
    q->head = NXT_NNCQ_SIZE;
    nxt_memzero((void *) q->entries, NXT_NNCQ_SIZE * sizeof(nxt_nncq_atomic_t));
    q->tail = NXT_NNCQ_SIZE;
}


/*
 * Puts "val" at the tail and stores in "*pos" the tail value that the item
 * took.  The head passes "*pos" when the item is taken.
 */
static inline nxt_int_t
nxt_nncq_enqueue_pos(nxt_nncq_t volatile *q, nxt_nncq_atomic_t val,
    nxt_nncq_atomic_t *pos)
{
    nxt_nncq_cycle_t   e_cycle, t_cycle;
    nxt_nncq_atomic_t  n, t, e, j, r;

    for (r = 0; r < NXT_NNCQ_RETRIES; r++) {
        t = nxt_nncq_tail(q);
        j = nxt_nncq_map(q, t);
        e = nxt_atomic_load_acquire(&q->entries[j]);

        e_cycle = nxt_nncq_cycle(q, e);
        t_cycle = nxt_nncq_cycle(q, t);

        if (e_cycle == t_cycle) {
            nxt_nncq_tail_cmp_inc(q, t);
            continue;
        }

        if (nxt_nncq_next_cycle(q, e_cycle) != t_cycle) {
            continue;
        }

        n = nxt_nncq_new_entry(q, t_cycle, val);

        if (nxt_atomic_cmp_set(&q->entries[j], e, n)) {
            nxt_nncq_tail_cmp_inc(q, t);

            *pos = t;

            return NXT_OK;
        }
    }

    return NXT_ERROR;
}


static inline nxt_int_t
nxt_nncq_enqueue(nxt_nncq_t volatile *q, nxt_nncq_atomic_t val)
{
    nxt_nncq_atomic_t  pos;

    return nxt_nncq_enqueue_pos(q, val, &pos);
}


static inline nxt_nncq_atomic_t
nxt_nncq_dequeue(nxt_nncq_t volatile *q)
{
    nxt_nncq_cycle_t   e_cycle, h_cycle;
    nxt_nncq_atomic_t  h, j, e, r;

    for (r = 0; r < NXT_NNCQ_RETRIES; r++) {
        h = nxt_nncq_head(q);
        j = nxt_nncq_map(q, h);
        e = nxt_atomic_load_acquire(&q->entries[j]);

        e_cycle = nxt_nncq_cycle(q, e);
        h_cycle = nxt_nncq_cycle(q, h);

        if (e_cycle != h_cycle) {
            if (nxt_nncq_next_cycle(q, e_cycle) == h_cycle) {
                return nxt_nncq_empty(q);
            }

            continue;
        }

        if (nxt_atomic_cmp_set(&q->head, h, h + 1)) {
            return nxt_nncq_index(q, e);
        }
    }

    return nxt_nncq_empty(q);
}


#endif /* _NXT_NNCQ_H_INCLUDED_ */
