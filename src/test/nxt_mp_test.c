
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
