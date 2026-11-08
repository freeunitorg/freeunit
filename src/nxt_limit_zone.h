/*
 * Copyright (C) FreeUnit contributors
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_LIMIT_ZONE_H_INCLUDED_
#define _NXT_LIMIT_ZONE_H_INCLUDED_


/*
 * A rate-limit zone: one bucket per key, all state in one preallocated arena
 * so the hit path never allocates.  Slice 1 of the limit_req work: the object
 * only.  Nothing calls it yet; the route action and the 429 come in slice 2.
 *
 * The zone is created in the router (one process, N threads), so the lock is a
 * thread spinlock, not SHM.  Counters live inside the zone and are read with
 * the zone, so a reload that keeps the zone by name keeps its numbers.
 */

#define NXT_LIMIT_KEY_MAX       16     /* 4-byte v4 or 16-byte v6 address */
#define NXT_LIMIT_TOKEN_SCALE   1000000  /* micro-tokens per request */
#define NXT_LIMIT_BURST_MAX     65535
#define NXT_LIMIT_RATE_MAX      1000000
#define NXT_LIMIT_KEYS_MAX      1048576


typedef struct {
    uint64_t    last_us;
    uint64_t    tokens;                /* micro-tokens, <= burst * SCALE */
    uint32_t    next;                  /* 1-based: hash chain or free list */
    /*
     * 1-based, 0 at the ends: the list of the entries in use from the most
     * to the least recently used, so the eviction takes the tail and the
     * arena is never scanned.
     */
    uint32_t    lru_prev;
    uint32_t    lru_next;
    uint8_t     key_len;
    uint8_t     key[NXT_LIMIT_KEY_MAX];
} nxt_limit_entry_t;


typedef struct {
    nxt_thread_spinlock_t   lock;

    nxt_limit_entry_t      *entries;
    uint32_t               *buckets;   /* 1-based entry index, 0 is empty */
    uint32_t                buckets_mask;

    uint32_t                keys_max;
    uint32_t                keys_used;
    uint32_t                free_head; /* 0: free list is empty */
    uint32_t                lru_head;  /* 0: no entry is in use */
    uint32_t                lru_tail;

    uint32_t                rate;      /* requests per second */
    uint32_t                burst;     /* bucket depth, in requests */

    /*
     * The bucket of the keys that find the zone full, with no idle entry
     * to take: they share it, so a full zone does not give a fresh bucket
     * to each new key.  Not in the hash and not in the list.
     */
    nxt_limit_entry_t       overflow;

    uint64_t                passed;
    uint64_t                rejected;
    /* Idle entries given to a new key. */
    uint64_t                evicted;
    /* Requests counted in the overflow bucket. */
    uint64_t                overflowed;
} nxt_limit_zone_t;


nxt_limit_zone_t *nxt_limit_zone_create(nxt_mp_t *mp, uint32_t keys_max,
    uint32_t rate, uint32_t burst);
nxt_bool_t nxt_limit_zone_check(nxt_limit_zone_t *zone, const uint8_t *key,
    size_t key_len, uint64_t now_us, uint64_t *wait_us);


#endif /* _NXT_LIMIT_ZONE_H_INCLUDED_ */
