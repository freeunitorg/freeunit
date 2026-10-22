
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include "nxt_tests.h"


/*
 * nxt_mp_get() and nxt_mp_zget() promise memory aligned suitably for
 * structures.  They are served by a bump allocator that has no
 * per-allocation alignment step, so the promise only holds if the
 * requested size is rounded up to NXT_MAX_ALIGNMENT: a single odd-sized
 * get() used to leave the page cursor misaligned and every later get()
 * from that page returned a misaligned pointer.  A struct copy or a 64-bit
 * field access through such a pointer faults with SIGBUS on armv7; this
 * test catches the regression on any architecture, no strict-alignment
 * hardware needed.
 */

nxt_int_t
nxt_mp_get_align_test(nxt_thread_t *thr)
{
    void        *p;
    nxt_mp_t    *mp;
    nxt_bool_t  valid;
    nxt_uint_t  i, n;

    /*
     * Sizes deliberately not multiples of NXT_MAX_ALIGNMENT.  On 32-bit
     * platforms 12 is sizeof(nxt_http_static_mtype_t) and 20 is both
     * sizeof(nxt_tstr_t) and sizeof(nxt_list_t) - the sizes of the
     * allocations that actually faulted on armv7.
     */
    static const size_t  sizes[] = {
        1, 2, 3, 5, 7, 9, 12, 17, 20, 23, 31, 33, 45, 63, 100
    };

    const size_t  min_chunk_size = 16;
    const size_t  page_size = 128;
    const size_t  page_alignment = 128;
    const size_t  cluster_size = page_size * 8;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "mem pool get alignment test "
                  "started, alignment:%uz", (size_t) NXT_MAX_ALIGNMENT);

    valid = nxt_mp_test_sizes(cluster_size, page_alignment, page_size,
                              min_chunk_size);
    if (!valid) {
        return NXT_ERROR;
    }

    mp = nxt_mp_create(cluster_size, page_alignment, page_size, min_chunk_size);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    /*
     * Several passes over the size table so that allocations both stay
     * within a page and spill over into freshly allocated ones.
     */

    for (i = 0; i < 64; i++) {

        for (n = 0; n < nxt_nitems(sizes); n++) {

            p = (i & 1) ? nxt_mp_get(mp, sizes[n])
                        : nxt_mp_zget(mp, sizes[n]);

            if (p == NULL) {
                nxt_log_error(NXT_LOG_NOTICE, thr->log,
                              "mem pool get alignment test failed: "
                              "get(%uz) returned NULL", sizes[n]);
                return NXT_ERROR;
            }

            if (((uintptr_t) p & (NXT_MAX_ALIGNMENT - 1)) != 0) {
                nxt_log_error(NXT_LOG_NOTICE, thr->log,
                              "mem pool get alignment test failed: "
                              "get(%uz) returned %p, not %uz-aligned",
                              sizes[n], p, (size_t) NXT_MAX_ALIGNMENT);
                return NXT_ERROR;
            }
        }

        /* A large allocation takes the nxt_mp_alloc_large() path. */

        p = nxt_mp_get(mp, page_size + 1);

        if (p == NULL || ((uintptr_t) p & (NXT_MAX_ALIGNMENT - 1)) != 0) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "mem pool get alignment test failed: "
                          "large get(%uz) returned %p",
                          page_size + 1, p);
            return NXT_ERROR;
        }
    }

    nxt_mp_destroy(mp);

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "mem pool get alignment test passed");

    return NXT_OK;
}


nxt_int_t
nxt_mp_test(nxt_thread_t *thr, nxt_uint_t runs, nxt_uint_t nblocks,
    size_t max_size)
{
    void          **blocks;
    size_t        total;
    uint32_t      value, size;
    nxt_mp_t      *mp;
    nxt_bool_t    valid;
    nxt_uint_t    i, n;

    const size_t  min_chunk_size = 16;
    const size_t  page_size = 128;
    const size_t  page_alignment = 128;
    const size_t  cluster_size = page_size * 8;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "mem pool test started, max:%uz", max_size);

    blocks = nxt_malloc(nblocks * sizeof(void *));
    if (blocks == NULL) {
        return NXT_ERROR;
    }

    valid = nxt_mp_test_sizes(cluster_size, page_alignment, page_size,
                              min_chunk_size);
    if (!valid) {
        return NXT_ERROR;
    }

    mp = nxt_mp_create(cluster_size, page_alignment, page_size, min_chunk_size);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    value = 0;

    for (i = 0; i < runs; i++) {

        total = 0;

        for (n = 0; n < nblocks; n++) {
            value = nxt_murmur_hash2(&value, sizeof(uint32_t));

            size = value & max_size;

            if (size == 0) {
                size++;
            }

            total += size;
            blocks[n] = nxt_mp_alloc(mp, size);

            if (blocks[n] == NULL) {
                nxt_log_error(NXT_LOG_NOTICE, thr->log,
                              "mem pool test failed: %uz", total);
                return NXT_ERROR;
            }
        }

        for (n = 0; n < nblocks; n++) {
            nxt_mp_free(mp, blocks[n]);
        }
    }

    if (!nxt_mp_is_empty(mp)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "mem pool is not empty");
        return NXT_ERROR;
    }

    nxt_mp_destroy(mp);

    nxt_free(blocks);

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "mem pool test passed");

    return NXT_OK;
}


/*
 * The cluster cache: a thread that destroys a pool keeps the pool's clusters
 * and gives them to its next pools.  The pool geometry is the one of the
 * router's request pool.  The count of nxt_memalign() calls for cluster
 * memory is read through an NXT_TESTS hook in src/nxt_mp.c.
 *
 * The test pins counts, not the safety property.  A reused cluster must never
 * be handed out while a pool that owns it is alive.  That holds because
 * nxt_mp_destroy() runs only when the pool has no live allocation, and this
 * test cannot see a break of that rule.  The AddressSanitizer legs of the CI
 * can see a use of a destroyed pool, because the cache is off there.  They
 * do not run the cache code.  No CI leg runs the other sanitizers that turn
 * the cache off.
 */

#if !(NXT_DEBUG_MEMORY)

#define NXT_MP_CC_POOLS  1000
#define NXT_MP_CC_GUARD  100
#define NXT_MP_CC_LIVE   40

/* NXT_MP_CLUSTER_CACHE_SAME in src/nxt_mp.c. */
#define NXT_MP_CC_SAME   16


static nxt_mp_t *
nxt_mp_cc_pool(size_t cluster_size, size_t page_size)
{
    u_char    *p;
    nxt_mp_t  *mp;

    mp = nxt_mp_create(cluster_size, 128, page_size, 32);
    if (mp == NULL) {
        return NULL;
    }

    /* A small allocation takes a cluster. */

    p = nxt_mp_alloc(mp, 64);
    if (p == NULL) {
        nxt_mp_destroy(mp);
        return NULL;
    }

    nxt_memset(p, 0xA5, 64);

    return mp;
}


/*
 * The guard of the cache.  Under a sanitizer that tracks freed memory, the
 * cache must be off, or the sanitizer misses a use of a destroyed pool.  The
 * test finds such a sanitizer with its own copy of the compiler checks, not
 * with NXT_HEAP_SANITIZER, so a guard that is removed or broken in nxt_mp.h
 * or nxt_clang.h fails here.  Without a sanitizer and with __thread, the
 * cache must be on, or the cache test below is skipped everywhere.
 * c-tests-asan in build-test.yml runs this test with gcc and with clang.
 */

#if (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_HWADDRESS__)        \
     || defined(__SANITIZE_THREAD__))

#define NXT_MP_CC_SANITIZER  1

#elif (defined(__clang__) && defined(__has_feature))

#if (__has_feature(address_sanitizer)                                         \
     || __has_feature(hwaddress_sanitizer)                                    \
     || __has_feature(memory_sanitizer)                                       \
     || __has_feature(thread_sanitizer))
#define NXT_MP_CC_SANITIZER  1
#endif

#endif

#ifndef NXT_MP_CC_SANITIZER
#define NXT_MP_CC_SANITIZER  0
#endif

#if (NXT_HAVE_THREAD_STORAGE_CLASS && !(NXT_MP_CC_SANITIZER)                  \
     && !(NXT_MP_NO_CLUSTER_CACHE))
#define NXT_MP_CC_ON  1
#else
#define NXT_MP_CC_ON  0
#endif


static int
nxt_mp_cluster_cache_guard_child(void *data)
{
    nxt_mp_t      *mp;
    nxt_uint_t    i, n, start, expect;
    nxt_thread_t  *thr;

    thr = data;

    nxt_mp_cluster_cache_drain();

    start = nxt_mp_test_cluster_alloc_count();

    for (i = 0; i < NXT_MP_CC_GUARD; i++) {
        mp = nxt_mp_cc_pool(4096, 512);
        NXT_TEST_CHECK(thr->log, mp != NULL,
                       "mp cluster cache guard test: pool %ui failed", i);
        nxt_mp_destroy(mp);
    }

    n = nxt_mp_test_cluster_alloc_count() - start;

    expect = NXT_MP_CC_ON ? 1 : NXT_MP_CC_GUARD;

    NXT_TEST_CHECK(thr->log, n == expect,
                   "mp cluster cache guard test: the cache must be %s, but "
                   "%ui pools allocated %ui clusters, not %ui",
                   NXT_MP_CC_ON ? "on" : "off", (nxt_uint_t) NXT_MP_CC_GUARD,
                   n, expect);

    nxt_mp_cluster_cache_drain();

    return 0;
}

#endif


#if (NXT_MP_CLUSTER_CACHE)

/*
 * Creates "count" pools at once, then destroys them.  Sets "allocs" to the
 * number of clusters that the pools allocated.
 */

static nxt_int_t
nxt_mp_cc_churn(size_t cluster_size, nxt_uint_t count, nxt_uint_t *allocs)
{
    nxt_mp_t    *live[NXT_MP_CC_LIVE];
    nxt_uint_t  i, n, start;

    start = nxt_mp_test_cluster_alloc_count();

    for (n = 0; n < count && n < NXT_MP_CC_LIVE; n++) {
        live[n] = nxt_mp_cc_pool(cluster_size, (cluster_size == 4096) ? 512
                                                                     : 256);
        if (live[n] == NULL) {
            break;
        }
    }

    *allocs = nxt_mp_test_cluster_alloc_count() - start;

    for (i = 0; i < n; i++) {
        nxt_mp_destroy(live[i]);
    }

    return (n == count) ? NXT_OK : NXT_ERROR;
}


static nxt_atomic_t  nxt_mp_cc_thread_ready;

/* The clusters in the cache of the thread when it starts. */
static nxt_atomic_t  nxt_mp_cc_thread_cached;


/*
 * A thread that destroys a pool and then waits to be canceled.  The cluster
 * of the pool stays in the cache of the thread.
 */

static void
nxt_mp_cc_cancel_thread(void *data)
{
    nxt_mp_t  *mp;

    (void) nxt_atomic_fetch_add(&nxt_mp_cc_thread_cached,
                                nxt_mp_test_cluster_cache_count());

    mp = nxt_mp_cc_pool(4096, 512);
    if (mp != NULL) {
        nxt_mp_destroy(mp);
    }

    (void) nxt_atomic_fetch_add(&nxt_mp_cc_thread_ready, 1);

    for ( ;; ) {
        pthread_testcancel();
        nxt_nanosleep(1000000);
    }
}


static int
nxt_mp_cluster_cache_test_child(void *data)
{
    void                 *p;
    nxt_mp_t             *mp, *mp2;
    nxt_uint_t           i, n, start;
    nxt_thread_t         *thr;
    nxt_thread_link_t    *link;
    nxt_thread_handle_t  handle;

    thr = data;

    nxt_mp_cluster_cache_drain();

    /* 1. Pools created and destroyed one after another. */

    start = nxt_mp_test_cluster_alloc_count();

    for (i = 0; i < NXT_MP_CC_POOLS; i++) {
        mp = nxt_mp_cc_pool(4096, 512);
        NXT_TEST_CHECK(thr->log, mp != NULL,
                       "mp cluster cache test: pool %ui failed", i);
        nxt_mp_destroy(mp);
    }

    n = nxt_mp_test_cluster_alloc_count() - start;

    NXT_TEST_CHECK(thr->log, n == 1,
                   "mp cluster cache test: %ui pools allocated %ui clusters, "
                   "not 1", (nxt_uint_t) NXT_MP_CC_POOLS, n);

    /* 2. A cluster that becomes empty in nxt_mp_free() is kept too. */

    mp = nxt_mp_create(4096, 128, 512, 32);
    NXT_TEST_CHECK(thr->log, mp != NULL, "mp cluster cache test: create");

    start = nxt_mp_test_cluster_alloc_count();

    for (i = 0; i < 100; i++) {
        p = nxt_mp_alloc(mp, 64);
        NXT_TEST_CHECK(thr->log, p != NULL, "mp cluster cache test: alloc");
        nxt_mp_free(mp, p);
    }

    n = nxt_mp_test_cluster_alloc_count() - start;

    nxt_mp_destroy(mp);

    NXT_TEST_CHECK(thr->log, n == 0,
                   "mp cluster cache test: 100 alloc and free pairs "
                   "allocated %ui clusters, not 0", n);

    /* 3. The cache keeps at most 16 clusters of one size. */

    NXT_TEST_CHECK(thr->log, nxt_mp_cc_churn(4096, 40, &n) == NXT_OK,
                   "mp cluster cache test: churn failed");
    NXT_TEST_CHECK(thr->log, nxt_mp_cc_churn(4096, 40, &n) == NXT_OK,
                   "mp cluster cache test: churn failed");

    NXT_TEST_CHECK(thr->log, n == 40 - NXT_MP_CC_SAME,
                   "mp cluster cache test: 40 pools after 40 destroyed "
                   "allocated %ui clusters, not %ui", n,
                   (nxt_uint_t) (40 - NXT_MP_CC_SAME));

    /*
     * 4. Clusters of one size do not take all the room.  A burst of closed
     *    connections leaves many 1024-byte clusters; a request pool still
     *    gets a 4096-byte one back.
     */

    nxt_mp_cluster_cache_drain();

    NXT_TEST_CHECK(thr->log, nxt_mp_cc_churn(1024, 40, &n) == NXT_OK,
                   "mp cluster cache test: churn failed");

    start = nxt_mp_test_cluster_alloc_count();

    for (i = 0; i < 100; i++) {
        mp = nxt_mp_cc_pool(4096, 512);
        NXT_TEST_CHECK(thr->log, mp != NULL,
                       "mp cluster cache test: pool %ui failed", i);
        nxt_mp_destroy(mp);
    }

    n = nxt_mp_test_cluster_alloc_count() - start;

    NXT_TEST_CHECK(thr->log, n == 1,
                   "mp cluster cache test: 100 pools after 40 1024-byte "
                   "clusters allocated %ui clusters, not 1", n);

    /*
     * 5. The cache keeps at most 32 clusters in all: 16 of 1024 bytes and 16
     *    of 2048 bytes fill it, and no 4096-byte cluster finds room.
     */

    nxt_mp_cluster_cache_drain();

    NXT_TEST_CHECK(thr->log,
                   nxt_mp_cc_churn(1024, NXT_MP_CC_SAME, &n) == NXT_OK
                   && nxt_mp_cc_churn(2048, NXT_MP_CC_SAME, &n) == NXT_OK
                   && nxt_mp_cc_churn(4096, NXT_MP_CC_SAME, &n) == NXT_OK
                   && nxt_mp_cc_churn(4096, NXT_MP_CC_SAME, &n) == NXT_OK,
                   "mp cluster cache test: churn failed");

    NXT_TEST_CHECK(thr->log, n == NXT_MP_CC_SAME,
                   "mp cluster cache test: %ui pools with a full cache "
                   "allocated %ui clusters, not %ui",
                   (nxt_uint_t) NXT_MP_CC_SAME, n,
                   (nxt_uint_t) NXT_MP_CC_SAME);

    /* 6. A cluster over 4096 bytes is not kept, even with room left. */

    nxt_mp_cluster_cache_drain();

    start = nxt_mp_test_cluster_alloc_count();

    for (i = 0; i < 10; i++) {
        mp = nxt_mp_cc_pool(8192, 1024);
        NXT_TEST_CHECK(thr->log, mp != NULL,
                       "mp cluster cache test: 8192 pool failed");
        nxt_mp_destroy(mp);
    }

    n = nxt_mp_test_cluster_alloc_count() - start;

    NXT_TEST_CHECK(thr->log, n == 10,
                   "mp cluster cache test: 10 pools with 8192-byte clusters "
                   "allocated %ui clusters, not 10", n);

    /* 7. nxt_mp_cluster_cache_drain() empties the cache. */

    mp = nxt_mp_cc_pool(4096, 512);
    NXT_TEST_CHECK(thr->log, mp != NULL, "mp cluster cache test: create");
    nxt_mp_destroy(mp);

    nxt_mp_cluster_cache_drain();

    start = nxt_mp_test_cluster_alloc_count();

    mp = nxt_mp_cc_pool(4096, 512);
    NXT_TEST_CHECK(thr->log, mp != NULL, "mp cluster cache test: create");
    nxt_mp_destroy(mp);

    n = nxt_mp_test_cluster_alloc_count() - start;

    NXT_TEST_CHECK(thr->log, n == 1,
                   "mp cluster cache test: a pool after the drain allocated "
                   "%ui clusters, not 1", n);

    /*
     * 8. The key is the size and the alignment.  A 1024-byte cluster is too
     *    small for a 4096-byte pool.  A cluster with 128-byte alignment is
     *    not aligned enough for a pool with 256-byte alignment.  The first
     *    allocation of a pool is at the start of its cluster.
     */

    nxt_mp_cluster_cache_drain();

    mp = nxt_mp_cc_pool(1024, 256);
    NXT_TEST_CHECK(thr->log, mp != NULL, "mp cluster cache test: create");
    nxt_mp_destroy(mp);

    start = nxt_mp_test_cluster_alloc_count();

    mp = nxt_mp_cc_pool(4096, 512);
    NXT_TEST_CHECK(thr->log, mp != NULL, "mp cluster cache test: create");

    n = nxt_mp_test_cluster_alloc_count() - start;

    NXT_TEST_CHECK(thr->log, n == 1,
                   "mp cluster cache test: a 4096-byte pool after a "
                   "1024-byte cluster allocated %ui clusters, not 1", n);

    nxt_mp_destroy(mp);

    nxt_mp_cluster_cache_drain();

    mp = nxt_mp_cc_pool(4096, 512);
    NXT_TEST_CHECK(thr->log, mp != NULL, "mp cluster cache test: create");
    nxt_mp_destroy(mp);

    start = nxt_mp_test_cluster_alloc_count();

    mp = nxt_mp_create(4096, 256, 512, 32);
    NXT_TEST_CHECK(thr->log, mp != NULL, "mp cluster cache test: create");

    p = nxt_mp_alloc(mp, 64);
    NXT_TEST_CHECK(thr->log, p != NULL, "mp cluster cache test: alloc");

    n = nxt_mp_test_cluster_alloc_count() - start;

    NXT_TEST_CHECK(thr->log, n == 1,
                   "mp cluster cache test: a pool with 256-byte alignment "
                   "after a 128-byte aligned cluster allocated %ui clusters, "
                   "not 1", n);

    NXT_TEST_CHECK(thr->log, ((uintptr_t) p & 255) == 0,
                   "mp cluster cache test: a pool with 256-byte alignment "
                   "got the cluster %p", p);

    nxt_mp_destroy(mp);

    /*
     * 9. A cluster that is freed twice stays in the cache once.  Otherwise
     *    two pools would share it.  The alert in the log is expected.
     */

    nxt_mp_cluster_cache_drain();

    mp = nxt_mp_create(4096, 128, 512, 32);
    NXT_TEST_CHECK(thr->log, mp != NULL, "mp cluster cache test: create");

    p = nxt_memalign(128, 4096);
    NXT_TEST_CHECK(thr->log, p != NULL, "mp cluster cache test: memalign");

    nxt_mp_test_cluster_put_twice(mp, p);
    nxt_mp_destroy(mp);

    start = nxt_mp_test_cluster_alloc_count();

    mp = nxt_mp_cc_pool(4096, 512);
    mp2 = nxt_mp_cc_pool(4096, 512);
    NXT_TEST_CHECK(thr->log, mp != NULL && mp2 != NULL,
                   "mp cluster cache test: create");

    n = nxt_mp_test_cluster_alloc_count() - start;

    NXT_TEST_CHECK(thr->log, n == 1,
                   "mp cluster cache test: two pools after a cluster freed "
                   "twice allocated %ui clusters, not 1", n);

    nxt_mp_destroy(mp);
    nxt_mp_destroy(mp2);

    /*
     * 10. A canceled thread does not call nxt_thread_exit().  Its cleanup
     *     handler frees its cache.
     */

    start = nxt_mp_test_cluster_drain_count();

    link = nxt_zalloc(sizeof(nxt_thread_link_t));
    NXT_TEST_CHECK(thr->log, link != NULL, "mp cluster cache test: link");

    link->start = nxt_mp_cc_cancel_thread;

    NXT_TEST_CHECK(thr->log, nxt_thread_create(&handle, link) == NXT_OK,
                   "mp cluster cache test: thread create");

    /* The thread writes the flag with an atomic add, so read it as one. */
    for (i = 0; nxt_atomic_fetch_add(&nxt_mp_cc_thread_ready, 0) == 0; i++) {
        NXT_TEST_CHECK(thr->log, i < 10000,
                       "mp cluster cache test: the thread did not start");
        nxt_nanosleep(1000000);
    }

    n = nxt_mp_cc_thread_cached;

    NXT_TEST_CHECK(thr->log, n == 0,
                   "mp cluster cache test: a new thread started with %ui "
                   "cached clusters, not 0", n);

    nxt_thread_cancel(handle);
    nxt_thread_wait(handle);

    n = nxt_mp_test_cluster_drain_count() - start;

    NXT_TEST_CHECK(thr->log, n == 1,
                   "mp cluster cache test: a canceled thread freed %ui "
                   "cached clusters, not 1", n);

    nxt_mp_cluster_cache_drain();

    return 0;
}

#endif


nxt_int_t
nxt_mp_cluster_cache_test(nxt_thread_t *thr)
{
#if !(NXT_DEBUG_MEMORY)
    int  rc;

    rc = nxt_test_in_child(thr, "mp cluster cache guard test",
                           nxt_mp_cluster_cache_guard_child, thr);

    NXT_TEST_CHECK(thr->log, rc == 0,
                   "mp cluster cache guard test failed (child status %d)", rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "mp cluster cache guard test passed: the cache is %s",
                  NXT_MP_CC_ON ? "on" : "off");
#else
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "mp cluster cache guard test skipped: the pools of "
                  "NXT_DEBUG_MEMORY have no clusters");
#endif

#if (NXT_MP_CLUSTER_CACHE)
    rc = nxt_test_in_child(thr, "mp cluster cache test",
                           nxt_mp_cluster_cache_test_child, thr);

    NXT_TEST_CHECK(thr->log, rc == 0,
                   "mp cluster cache test failed (child status %d)", rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "mp cluster cache test passed");

#else
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "mp cluster cache test skipped: the cache is compiled out");
#endif

    return NXT_OK;
}
