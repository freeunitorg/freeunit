/*
 * Copyright (C) FreeUnit contributors
 */

#include <nxt_main.h>

#include <nxt_limit_zone.h>

#include "nxt_tests.h"


/*
 * The zone object from slice 1: the token bucket, the burst cap, per-key
 * isolation, the overflow bucket and the eviction of an idle entry when
 * the arena is full, and the counters.  The clock is
 * a parameter, so every case is deterministic and needs no sleeps.
 */

static const u_char  nxt_limit_key_a[4] = { 10, 0, 0, 1 };
static const u_char  nxt_limit_key_b[4] = { 10, 0, 0, 2 };
static const u_char  nxt_limit_key_c[4] = { 10, 0, 0, 3 };
static const u_char  nxt_limit_key_d[4] = { 10, 0, 0, 4 };


static nxt_bool_t
nxt_limit_zone_check_key(nxt_limit_zone_t *zone, const u_char *key,
    uint64_t now_us)
{
    return nxt_limit_zone_check(zone, key, 4, now_us, NULL);
}


nxt_int_t
nxt_limit_zone_test(nxt_thread_t *thr)
{
    nxt_mp_t            *mp;
    nxt_limit_zone_t    *zone, *small, *capz, *fast;
    nxt_uint_t           i, passed;
    u_char               long_key[17];
    uint64_t             now, t;

    /* cluster must be at least a page: 8 pages, 4 KiB pages. */
    mp = nxt_mp_create(32768, 128, 4096, 64);

    NXT_TEST_CHECK(thr->log, mp != NULL, "nxt_mp_create() failed");

    /* Parameters are validated, not clamped. */

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_create(mp, 0, 1, 1) == NULL,
                   "a zone with keys_max 0 was accepted");

    NXT_TEST_CHECK(thr->log,
                   nxt_limit_zone_create(mp, NXT_LIMIT_KEYS_MAX + 1, 1, 1)
                   == NULL,
                   "a zone above NXT_LIMIT_KEYS_MAX was accepted");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_create(mp, 4, 0, 1) == NULL,
                   "a zone with rate 0 was accepted");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_create(mp, 4, 1, 0) == NULL,
                   "a zone with burst 0 was accepted");

    NXT_TEST_CHECK(thr->log,
                   nxt_limit_zone_create(mp, 4, 1, NXT_LIMIT_BURST_MAX + 1)
                   == NULL,
                   "a zone above NXT_LIMIT_BURST_MAX was accepted");

    /* rate 10/s, burst 3: three requests pass, the fourth does not. */

    zone = nxt_limit_zone_create(mp, 16, 10, 3);

    NXT_TEST_CHECK(thr->log, zone != NULL, "zone creation failed");

    now = 1000000;

    for (i = 0; i < 3; i++) {
        NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(zone,
                           nxt_limit_key_a, now) == 1,
                       "request %ui inside the burst was rejected", i + 1);
    }

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(zone, nxt_limit_key_a,
                       now) == 0,
                   "the request past the burst was allowed");

    NXT_TEST_CHECK(thr->log, zone->passed == 3 && zone->rejected == 1,
                   "counters after the burst: passed %uL, rejected %uL",
                   zone->passed, zone->rejected);

    /*
     * 10 tokens per second is one token per 100 ms: at 99.999 ms there is
     * still no token, at 100 ms there is exactly one.  The time is in
     * microseconds.
     */

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(zone, nxt_limit_key_a,
                       now + 99999) == 0,
                   "a request passed before a whole token accrued");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(zone, nxt_limit_key_a,
                       now + 100000) == 1,
                   "a request was rejected at a whole token");

    /* A second key has its own bucket. */

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(zone, nxt_limit_key_b,
                       now) == 1,
                   "a fresh key was rejected");

    /*
     * Tokens never exceed the burst, however long the idle period: after an
     * hour at one token per second a burst-2 zone still passes two requests,
     * not three thousand six hundred.
     */

    capz = nxt_limit_zone_create(mp, 4, 1, 2);

    NXT_TEST_CHECK(thr->log, capz != NULL, "cap zone creation failed");

    for (i = 0; i < 2; i++) {
        NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(capz,
                           nxt_limit_key_c, 0) == 1,
                       "request %ui inside the cap burst was rejected", i + 1);
    }

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(capz, nxt_limit_key_c,
                       3600 * 1000000ULL) == 1,
                   "the first request after an idle hour was rejected");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(capz, nxt_limit_key_c,
                       3600 * 1000000ULL) == 1,
                   "the second request after an idle hour was rejected");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(capz, nxt_limit_key_c,
                       3600 * 1000000ULL) == 0,
                   "an idle hour accrued more than the burst");

    /*
     * Above 1000 per second: 5000 per second with a burst of 1 is one
     * request per 200 us.  A clock in milliseconds passed one request
     * per millisecond at most, 1000 per second.
     */

    fast = nxt_limit_zone_create(mp, 4, 5000, 1);

    NXT_TEST_CHECK(thr->log, fast != NULL, "fast zone creation failed");

    now = 1000000;

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(fast, nxt_limit_key_a,
                       now) == 1,
                   "the first request at 5000/s was rejected");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(fast, nxt_limit_key_a,
                       now + 199) == 0,
                   "a request at 5000/s passed after 199 us");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(fast, nxt_limit_key_a,
                       now + 200) == 1,
                   "a request at 5000/s was rejected after 200 us");

    /* One second, a request every 50 us: 5000 pass, one each 200 us. */

    passed = 0;

    for (t = now + 250; t <= now + 200 + 1000000; t += 50) {
        passed += nxt_limit_zone_check_key(fast, nxt_limit_key_a, t);
    }

    NXT_TEST_CHECK(thr->log, passed == 5000,
                   "%ui requests passed in one second at 5000/s", passed);

    /* An oversized key is not limited: it cannot be stored, so it passes. */

    nxt_memzero(long_key, sizeof(long_key));

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check(zone, long_key,
                       sizeof(long_key), now, NULL) == 1,
                   "a 17-byte key was limited instead of passed");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check(zone, (u_char *) "", 0, now,
                                                  NULL)
                   == 1,
                   "an empty key was limited instead of passed");

    /*
     * A full zone: two slots, four keys, 1 per second, a burst of 1.  The
     * entries are in use, so the third and the fourth key share the
     * overflow bucket: the third one empties it, the fourth one is
     * rejected.  The arena never grows past keys_max.
     */

    small = nxt_limit_zone_create(mp, 2, 1, 1);

    NXT_TEST_CHECK(thr->log, small != NULL, "small zone creation failed");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(small, nxt_limit_key_a,
                       0) == 1,
                   "first key in a fresh small zone was rejected");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(small, nxt_limit_key_b,
                       10000) == 1,
                   "second key in a fresh small zone was rejected");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(small, nxt_limit_key_c,
                       20000) == 1,
                   "the third key was rejected by a full overflow bucket");

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(small, nxt_limit_key_d,
                       30000) == 0,
                   "the fourth key got a fresh bucket in a full zone");

    NXT_TEST_CHECK(thr->log, small->keys_used == 2 && small->evicted == 0
                             && small->overflowed == 2,
                   "full zone: keys_used %uD, evicted %uL, overflowed %uL",
                   small->keys_used, small->evicted, small->overflowed);

    /* The first key keeps its empty bucket. */

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(small, nxt_limit_key_a,
                       40000) == 0,
                   "a key in a full zone got a fresh bucket");

    /*
     * One second later the bucket of the least recently used key, b, is
     * full again: the entry is idle, and the new key takes it.
     */

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(small, nxt_limit_key_c,
                       1010000) == 1,
                   "the new key did not take the idle entry");

    NXT_TEST_CHECK(thr->log, small->keys_used == 2 && small->evicted == 1
                             && small->overflowed == 2,
                   "idle entry: keys_used %uD, evicted %uL, overflowed %uL",
                   small->keys_used, small->evicted, small->overflowed);

    /* The key that took the entry has its own bucket now. */

    NXT_TEST_CHECK(thr->log, nxt_limit_zone_check_key(small, nxt_limit_key_c,
                       1010001) == 0,
                   "the key of an idle entry was not limited");

    nxt_mp_destroy(mp);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_limit_zone test passed");

    return NXT_OK;
}

#undef ZONE_CAP_CHECK
