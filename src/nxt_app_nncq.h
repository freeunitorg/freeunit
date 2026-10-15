
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_APP_NNCQ_H_INCLUDED_
#define _NXT_APP_NNCQ_H_INCLUDED_


/* Appilcation Numeric Naive Circular Queue */

#define NXT_APP_NNCQ_SIZE  131072

/* The retry budget of the loops below, see nxt_nncq.h. */
#define NXT_APP_NNCQ_RETRIES  NXT_APP_NNCQ_SIZE

typedef uint32_t nxt_app_nncq_atomic_t;
typedef uint16_t nxt_app_nncq_cycle_t;

/*
 * head, tail and the entries count in nxt_app_nncq_atomic_t and wrap at its
 * width.  The cycle of an index i is i / NXT_APP_NNCQ_SIZE, so there are
 * NXT_APP_NNCQ_CYCLES cycles, and after the last one the cycle is 0 again.
 * The size must divide 2^32, or i % NXT_APP_NNCQ_SIZE would jump at the
 * wrap.  The largest cycle must fit nxt_app_nncq_cycle_t.
 */
#define NXT_APP_NNCQ_CYCLES                                                   \
    ((nxt_app_nncq_atomic_t) -1 / NXT_APP_NNCQ_SIZE + 1)

nxt_static_assert((nxt_app_nncq_atomic_t) -1 % NXT_APP_NNCQ_SIZE
                  == NXT_APP_NNCQ_SIZE - 1,
                  "NXT_APP_NNCQ_SIZE must divide the range of the index");
nxt_static_assert(NXT_APP_NNCQ_CYCLES - 1 <= (nxt_app_nncq_cycle_t) -1,
                  "nxt_app_nncq_cycle_t must hold every cycle");

typedef struct {
    nxt_app_nncq_atomic_t  head;
    nxt_app_nncq_atomic_t  entries[NXT_APP_NNCQ_SIZE];
    nxt_app_nncq_atomic_t  tail;
} nxt_app_nncq_t;


static inline nxt_app_nncq_atomic_t
nxt_app_nncq_head(nxt_app_nncq_t const volatile *q)
{
    return q->head;
}


static inline nxt_app_nncq_atomic_t
nxt_app_nncq_tail(nxt_app_nncq_t const volatile *q)
{
    return q->tail;
}


static inline void
nxt_app_nncq_tail_cmp_inc(nxt_app_nncq_t volatile *q, nxt_app_nncq_atomic_t t)
{
    nxt_atomic_cmp_set(&q->tail, t, t + 1);
}


static inline nxt_app_nncq_atomic_t
nxt_app_nncq_index(nxt_app_nncq_t const volatile *q, nxt_app_nncq_atomic_t i)
{
    return i % NXT_APP_NNCQ_SIZE;
}


static inline nxt_app_nncq_atomic_t
nxt_app_nncq_map(nxt_app_nncq_t const volatile *q, nxt_app_nncq_atomic_t i)
{
    return i % NXT_APP_NNCQ_SIZE;
}


static inline nxt_app_nncq_cycle_t
nxt_app_nncq_cycle(nxt_app_nncq_t const volatile *q, nxt_app_nncq_atomic_t i)
{
    return i / NXT_APP_NNCQ_SIZE;
}


static inline nxt_app_nncq_cycle_t
nxt_app_nncq_next_cycle(nxt_app_nncq_t const volatile *q,
    nxt_app_nncq_cycle_t i)
{
    return (i + 1) % NXT_APP_NNCQ_CYCLES;
}


static inline nxt_app_nncq_atomic_t
nxt_app_nncq_new_entry(nxt_app_nncq_t const volatile *q,
    nxt_app_nncq_cycle_t cycle,
    nxt_app_nncq_atomic_t i)
{
    return (nxt_app_nncq_atomic_t) cycle * NXT_APP_NNCQ_SIZE
           + (i % NXT_APP_NNCQ_SIZE);
}


static inline nxt_app_nncq_atomic_t
nxt_app_nncq_empty(nxt_app_nncq_t const volatile *q)
{
    return NXT_APP_NNCQ_SIZE;
}


static void
nxt_app_nncq_init(nxt_app_nncq_t volatile *q)
{
    q->head = NXT_APP_NNCQ_SIZE;
    nxt_memzero((void *) q->entries,
                NXT_APP_NNCQ_SIZE * sizeof(nxt_app_nncq_atomic_t));
    q->tail = NXT_APP_NNCQ_SIZE;
}


/*
 * Enqueue an item and optionally report its assigned slot position 't'.
 * 'pos' is written only when NXT_OK is returned.
 */
static nxt_int_t
nxt_app_nncq_enqueue_pos(nxt_app_nncq_t volatile *q, nxt_app_nncq_atomic_t val,
    nxt_app_nncq_atomic_t *pos)
{
    nxt_app_nncq_cycle_t   e_cycle, t_cycle;
    nxt_app_nncq_atomic_t  n, t, e, j, r;

    for (r = 0; r < NXT_APP_NNCQ_RETRIES; r++) {
        t = nxt_app_nncq_tail(q);
        j = nxt_app_nncq_map(q, t);
        e = q->entries[j];

        e_cycle = nxt_app_nncq_cycle(q, e);
        t_cycle = nxt_app_nncq_cycle(q, t);

        if (e_cycle == t_cycle) {
            nxt_app_nncq_tail_cmp_inc(q, t);
            continue;
        }

        if (nxt_app_nncq_next_cycle(q, e_cycle) != t_cycle) {
            continue;
        }

        n = nxt_app_nncq_new_entry(q, t_cycle, val);

        if (nxt_atomic_cmp_set(&q->entries[j], e, n)) {
            nxt_app_nncq_tail_cmp_inc(q, t);

            if (pos != NULL) {
                *pos = t;
            }

            return NXT_OK;
        }
    }

    return NXT_ERROR;
}


static inline nxt_int_t
nxt_app_nncq_enqueue(nxt_app_nncq_t volatile *q, nxt_app_nncq_atomic_t val)
{
    return nxt_app_nncq_enqueue_pos(q, val, NULL);
}


static nxt_app_nncq_atomic_t
nxt_app_nncq_dequeue(nxt_app_nncq_t volatile *q)
{
    nxt_app_nncq_cycle_t   e_cycle, h_cycle;
    nxt_app_nncq_atomic_t  h, j, e, r;

    for (r = 0; r < NXT_APP_NNCQ_RETRIES; r++) {
        h = nxt_app_nncq_head(q);
        j = nxt_app_nncq_map(q, h);
        e = q->entries[j];

        e_cycle = nxt_app_nncq_cycle(q, e);
        h_cycle = nxt_app_nncq_cycle(q, h);

        if (e_cycle != h_cycle) {
            if (nxt_app_nncq_next_cycle(q, e_cycle) == h_cycle) {
                return nxt_app_nncq_empty(q);
            }

            continue;
        }

        if (nxt_atomic_cmp_set(&q->head, h, h + 1)) {
            return nxt_app_nncq_index(q, e);
        }
    }

    return nxt_app_nncq_empty(q);
}


#endif /* _NXT_APP_NNCQ_H_INCLUDED_ */
