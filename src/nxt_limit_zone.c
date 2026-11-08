/*
 * Copyright (C) FreeUnit contributors
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>

#include "nxt_limit_zone.h"


static uint32_t nxt_limit_hash(nxt_limit_zone_t *zone, const uint8_t *key,
    size_t len);
static nxt_limit_entry_t *nxt_limit_lookup(nxt_limit_zone_t *zone,
    const uint8_t *key, size_t len, uint32_t hash, uint32_t *bucket);
static nxt_limit_entry_t *nxt_limit_claim(nxt_limit_zone_t *zone,
    const uint8_t *key, size_t len, uint32_t hash, uint64_t now_us);
static void nxt_limit_unlink(nxt_limit_zone_t *zone, uint32_t idx,
    uint32_t bucket);
static void nxt_limit_lru_unlink(nxt_limit_zone_t *zone, uint32_t idx);
static void nxt_limit_lru_link(nxt_limit_zone_t *zone, uint32_t idx);
static uint32_t nxt_limit_evict_idle(nxt_limit_zone_t *zone, uint64_t now_us);
static void nxt_limit_refill(nxt_limit_zone_t *zone, nxt_limit_entry_t *entry,
    uint64_t now_us);


/*
 * SipHash-2-4 with a random key for each zone.  The keys are client
 * addresses, which a client can often choose (its own /64, or a trusted
 * "client_ip"), and every lookup walks a chain under the zone lock.  With
 * an unkeyed hash a client can send addresses that all go to one chain;
 * without the key it cannot know which addresses collide.
 */

#define NXT_LIMIT_ROTL(x, b)  (((x) << (b)) | ((x) >> (64 - (b))))

#define NXT_LIMIT_SIPROUND(v0, v1, v2, v3)                                    \
    do {                                                                      \
        v0 += v1; v1 = NXT_LIMIT_ROTL(v1, 13); v1 ^= v0;                      \
        v0 = NXT_LIMIT_ROTL(v0, 32);                                          \
        v2 += v3; v3 = NXT_LIMIT_ROTL(v3, 16); v3 ^= v2;                      \
        v0 += v3; v3 = NXT_LIMIT_ROTL(v3, 21); v3 ^= v0;                      \
        v2 += v1; v1 = NXT_LIMIT_ROTL(v1, 17); v1 ^= v2;                      \
        v2 = NXT_LIMIT_ROTL(v2, 32);                                          \
    } while (0)


uint64_t
nxt_limit_siphash(const uint64_t key[2], const uint8_t *data, size_t len)
{
    size_t    i, j, end;
    uint64_t  v0, v1, v2, v3, m;

    v0 = key[0] ^ 0x736f6d6570736575ULL;
    v1 = key[1] ^ 0x646f72616e646f6dULL;
    v2 = key[0] ^ 0x6c7967656e657261ULL;
    v3 = key[1] ^ 0x7465646279746573ULL;

    end = len & ~(size_t) 7;

    for (i = 0; i < end; i += 8) {
        m = 0;

        for (j = 0; j < 8; j++) {
            m |= (uint64_t) data[i + j] << (8 * j);
        }

        v3 ^= m;
        NXT_LIMIT_SIPROUND(v0, v1, v2, v3);
        NXT_LIMIT_SIPROUND(v0, v1, v2, v3);
        v0 ^= m;
    }

    m = (uint64_t) len << 56;

    for (j = 0; i + j < len; j++) {
        m |= (uint64_t) data[i + j] << (8 * j);
    }

    v3 ^= m;
    NXT_LIMIT_SIPROUND(v0, v1, v2, v3);
    NXT_LIMIT_SIPROUND(v0, v1, v2, v3);
    v0 ^= m;

    v2 ^= 0xff;
    NXT_LIMIT_SIPROUND(v0, v1, v2, v3);
    NXT_LIMIT_SIPROUND(v0, v1, v2, v3);
    NXT_LIMIT_SIPROUND(v0, v1, v2, v3);
    NXT_LIMIT_SIPROUND(v0, v1, v2, v3);

    return v0 ^ v1 ^ v2 ^ v3;
}


static uint32_t
nxt_limit_hash(nxt_limit_zone_t *zone, const uint8_t *key, size_t len)
{
    return (uint32_t) nxt_limit_siphash(zone->hash_key, key, len);
}


nxt_limit_zone_t *
nxt_limit_zone_create(nxt_mp_t *mp, uint32_t keys_max, uint32_t rate,
    uint32_t burst)
{
    uint32_t            i, buckets;
    uint32_t            *heads;
    nxt_thread_t        *thr;
    nxt_limit_zone_t    *zone;
    nxt_limit_entry_t   *entry;

    if (keys_max == 0 || keys_max > NXT_LIMIT_KEYS_MAX
        || rate == 0 || rate > NXT_LIMIT_RATE_MAX
        || burst == 0 || burst > NXT_LIMIT_BURST_MAX)
    {
        return NULL;
    }

    buckets = 1;

    while (buckets < keys_max) {
        buckets <<= 1;
    }

    zone = nxt_mp_get(mp, sizeof(nxt_limit_zone_t));

    if (zone == NULL) {
        return NULL;
    }

    /*
     * One arena for the entries and one for the bucket heads: the hit path
     * takes no allocation, and both arrays die with the pool that owns the
     * zone, so the reload path only has to refcount the zone itself.
     */

    entry = nxt_mp_get(mp, keys_max * sizeof(nxt_limit_entry_t));

    if (entry == NULL) {
        return NULL;
    }

    heads = nxt_mp_get(mp, buckets * sizeof(uint32_t));

    if (heads == NULL) {
        return NULL;
    }

    nxt_memzero(zone, sizeof(nxt_limit_zone_t));
    nxt_memzero(entry, keys_max * sizeof(nxt_limit_entry_t));
    nxt_memzero(heads, buckets * sizeof(uint32_t));

    thr = nxt_thread();

    for (i = 0; i < 2; i++) {
        zone->hash_key[i] = (uint64_t) nxt_random(&thr->random) << 32
                            | nxt_random(&thr->random);
    }

    zone->entries = entry;
    zone->buckets = heads;
    zone->buckets_mask = buckets - 1;
    zone->keys_max = keys_max;
    zone->free_head = 1;

    for (i = 0; i < keys_max; i++) {
        /* 1-based links: 0 means "no next", so index 0 is stored as 1. */
        entry[i].next = (i + 1 < keys_max) ? i + 2 : 0;
    }

    zone->rate = rate;
    zone->burst = burst;

    zone->overflow.tokens = (uint64_t) burst * NXT_LIMIT_TOKEN_SCALE;

    return zone;
}


/*
 * The time is in microseconds: a bucket gets "rate" micro-tokens in each
 * one, so every "rate" up to NXT_LIMIT_RATE_MAX is exact with a "burst" of
 * one.  A clock in milliseconds limited a bucket to "burst" requests per
 * millisecond.
 *
 * When the request is rejected and wait_us is given, it is set to the
 * microseconds until one request fits the bucket at the rate: what a
 * "Retry-After" is made of.  It is 0 when the request passes.
 */

nxt_bool_t
nxt_limit_zone_check(nxt_limit_zone_t *zone, const uint8_t *key,
    size_t key_len, uint64_t now_us, uint64_t *wait_us)
{
    uint32_t            hash, bucket;
    uint64_t            need;
    nxt_bool_t          allow;
    nxt_limit_entry_t   *entry;

    if (wait_us != NULL) {
        *wait_us = 0;
    }

    if (key_len == 0 || key_len > NXT_LIMIT_KEY_MAX) {
        return 1;
    }

    hash = nxt_limit_hash(zone, key, key_len);

    nxt_thread_spin_lock(&zone->lock);

    entry = nxt_limit_lookup(zone, key, key_len, hash, &bucket);

    if (entry != NULL) {
        /* Used now: to the head of the list, away from the eviction. */
        nxt_limit_lru_unlink(zone, entry - zone->entries);
        nxt_limit_lru_link(zone, entry - zone->entries);

    } else {
        entry = nxt_limit_claim(zone, key, key_len, hash, now_us);

        if (entry == NULL) {
            /* Full, and no entry is idle: the shared bucket counts it. */
            entry = &zone->overflow;
            zone->overflowed++;
        }
    }

    nxt_limit_refill(zone, entry, now_us);

    allow = (entry->tokens >= NXT_LIMIT_TOKEN_SCALE);

    if (allow) {
        entry->tokens -= NXT_LIMIT_TOKEN_SCALE;
        zone->passed++;

    } else {
        zone->rejected++;

        if (wait_us != NULL) {
            need = NXT_LIMIT_TOKEN_SCALE - entry->tokens;
            *wait_us = (need + zone->rate - 1) / zone->rate;
        }
    }

    nxt_thread_spin_unlock(&zone->lock);

    return allow;
}


static nxt_limit_entry_t *
nxt_limit_lookup(nxt_limit_zone_t *zone, const uint8_t *key, size_t len,
    uint32_t hash, uint32_t *bucket)
{
    uint32_t            link;
    nxt_limit_entry_t   *entry;

    *bucket = hash & zone->buckets_mask;
    link = zone->buckets[*bucket];

    while (link != 0) {
        entry = &zone->entries[link - 1];

        if ((size_t) entry->key_len == len
            && (len == 0 || memcmp(entry->key, key, len) == 0))
        {
            return entry;
        }

        link = entry->next;
    }

    return NULL;
}


/*
 * A free entry, or the least recently used one when it is idle.  NULL when
 * the zone is full and that entry is in use.
 */

static nxt_limit_entry_t *
nxt_limit_claim(nxt_limit_zone_t *zone, const uint8_t *key, size_t len,
    uint32_t hash, uint64_t now_us)
{
    uint32_t            bucket, idx;
    nxt_limit_entry_t   *entry;

    if (zone->free_head != 0) {
        idx = zone->free_head - 1;
        entry = &zone->entries[idx];
        zone->free_head = entry->next;
        zone->keys_used++;

    } else {
        idx = nxt_limit_evict_idle(zone, now_us);

        if (idx == (uint32_t) -1) {
            return NULL;
        }

        entry = &zone->entries[idx];
    }

    entry->key_len = (uint8_t) len;
    nxt_memcpy(entry->key, key, len);
    entry->last_us = 0;
    entry->tokens = (uint64_t) zone->burst * NXT_LIMIT_TOKEN_SCALE;
    entry->next = 0;

    bucket = hash & zone->buckets_mask;
    entry->next = zone->buckets[bucket];
    zone->buckets[bucket] = idx + 1;

    nxt_limit_lru_link(zone, idx);

    return entry;
}


static void
nxt_limit_unlink(nxt_limit_zone_t *zone, uint32_t idx, uint32_t bucket)
{
    uint32_t  link;

    link = zone->buckets[bucket];

    if (link == idx + 1) {
        zone->buckets[bucket] = zone->entries[idx].next;
        return;
    }

    while (link != 0) {
        if (zone->entries[link - 1].next == idx + 1) {
            zone->entries[link - 1].next = zone->entries[idx].next;
            return;
        }

        link = zone->entries[link - 1].next;
    }
}


static void
nxt_limit_lru_unlink(nxt_limit_zone_t *zone, uint32_t idx)
{
    nxt_limit_entry_t  *entry;

    entry = &zone->entries[idx];

    if (entry->lru_prev != 0) {
        zone->entries[entry->lru_prev - 1].lru_next = entry->lru_next;

    } else {
        zone->lru_head = entry->lru_next;
    }

    if (entry->lru_next != 0) {
        zone->entries[entry->lru_next - 1].lru_prev = entry->lru_prev;

    } else {
        zone->lru_tail = entry->lru_prev;
    }
}


/* At the head: the entry is the most recently used one. */

static void
nxt_limit_lru_link(nxt_limit_zone_t *zone, uint32_t idx)
{
    nxt_limit_entry_t  *entry;

    entry = &zone->entries[idx];

    entry->lru_prev = 0;
    entry->lru_next = zone->lru_head;

    if (zone->lru_head != 0) {
        zone->entries[zone->lru_head - 1].lru_prev = idx + 1;

    } else {
        zone->lru_tail = idx + 1;
    }

    zone->lru_head = idx + 1;
}


/*
 * The least recently used entry is the tail of the list: the one with the
 * oldest last_us, found without a scan of the arena.  A scan cost 28 us
 * per request at 16384 keys, under the lock, for every new key once the
 * arena was full.
 *
 * It is given to the new key only when it is idle: its bucket is full at
 * now_us, as a new bucket is, so its key loses nothing.  An entry in use
 * is kept: else a client that uses more keys than the zone holds gets a
 * fresh bucket for every request.  Only the tail is looked at, so an idle
 * entry elsewhere is not found; the new key then takes the overflow
 * bucket, which is the safe side.
 */

static uint32_t
nxt_limit_evict_idle(nxt_limit_zone_t *zone, uint64_t now_us)
{
    uint32_t            oldest, bucket, hash;
    nxt_limit_entry_t   *entry;

    if (zone->lru_tail == 0) {
        return (uint32_t) -1;
    }

    oldest = zone->lru_tail - 1;
    entry = &zone->entries[oldest];

    nxt_limit_refill(zone, entry, now_us);

    if (entry->tokens < (uint64_t) zone->burst * NXT_LIMIT_TOKEN_SCALE) {
        return (uint32_t) -1;
    }

    nxt_limit_lru_unlink(zone, oldest);

    hash = nxt_limit_hash(zone, entry->key, entry->key_len);
    bucket = hash & zone->buckets_mask;

    nxt_limit_unlink(zone, oldest, bucket);

    /* The key is dropped, not carried over: the caller sets the new key. */

    entry->key_len = 0;
    zone->evicted++;

    return oldest;
}


static void
nxt_limit_refill(nxt_limit_zone_t *zone, nxt_limit_entry_t *entry,
    uint64_t now_us)
{
    uint64_t  elapsed, max;

    if (now_us <= entry->last_us) {
        return;
    }

    elapsed = now_us - entry->last_us;
    max = (uint64_t) zone->burst * NXT_LIMIT_TOKEN_SCALE;

    /*
     * Micro-tokens added: elapsed microseconds times rate per second.  The
     * rate is at least 1, so "max" microseconds fill any bucket, and below
     * that the product is under 2^56: no overflow.
     */

    if (elapsed >= max || entry->tokens + elapsed * zone->rate >= max) {
        entry->tokens = max;

    } else {
        entry->tokens += elapsed * zone->rate;
    }

    entry->last_us = now_us;
}
