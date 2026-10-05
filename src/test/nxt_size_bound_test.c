/*
 * Copyright (C) FreeUnit Community
 */

/*
 * Each allocator adds a header to the size it is given.  A size near
 * SIZE_MAX must make the allocation fail.  It must not wrap the sum and
 * return a small block.
 *
 * With a 32-bit size_t, nxt_mp_alloc_large() let the sizes 0xFFFFFFE5 to
 * 0xFFFFFFFE through its 4 GiB check.  Then the aligned size, or the aligned
 * size plus the block header, wrapped.  The cases below are written relative
 * to SIZE_MAX, so with a 32-bit size_t they are exactly the sizes around
 * that window.  With a 64-bit size_t the 4 GiB check refuses them, and the
 * cases run as a regression check.  The nxt_buf_t cases wrap with both
 * widths.
 *
 * Each allocation runs in a child: a wrapped size corrupts the heap of the
 * process that uses the block.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include "nxt_tests.h"


typedef enum {
    NXT_SIZE_BOUND_MP,
    NXT_SIZE_BOUND_BUF_MEM,
    NXT_SIZE_BOUND_BUF_MEM_TS,
    NXT_SIZE_BOUND_BUF_FILE,
    NXT_SIZE_BOUND_ENGINE_BUF,
    NXT_SIZE_BOUND_BODY,
} nxt_size_bound_test_alloc_t;


typedef struct {
    const char                   *name;
    nxt_size_bound_test_alloc_t  alloc;
    size_t                       size;
} nxt_size_bound_test_case_t;


static const nxt_size_bound_test_case_t  nxt_size_bound_test_cases[] = {
    /* 0xFFFFFFE4 with a 32-bit size_t: the header sum fits. */
    { "nxt_mp_alloc()", NXT_SIZE_BOUND_MP, SIZE_MAX - 27 },
    /* 0xFFFFFFE5: the smallest size whose header sum wraps. */
    { "nxt_mp_alloc()", NXT_SIZE_BOUND_MP, SIZE_MAX - 26 },
    /* 0xFFFFFFFC: the largest aligned size. */
    { "nxt_mp_alloc()", NXT_SIZE_BOUND_MP, SIZE_MAX - 3 },
    /* 0xFFFFFFFE: the alignment itself wraps to 0. */
    { "nxt_mp_alloc()", NXT_SIZE_BOUND_MP, SIZE_MAX - 1 },

    { "nxt_buf_mem_alloc()", NXT_SIZE_BOUND_BUF_MEM,
      SIZE_MAX - NXT_BUF_MEM_SIZE + 1 },
    /* The thread-safe part is private to nxt_buf.c: any size near SIZE_MAX. */
    { "nxt_buf_mem_ts_alloc()", NXT_SIZE_BOUND_BUF_MEM_TS, SIZE_MAX },
    { "nxt_buf_file_alloc()", NXT_SIZE_BOUND_BUF_FILE,
      SIZE_MAX - NXT_BUF_FILE_SIZE + 1 },
    { "nxt_event_engine_buf_mem_alloc()", NXT_SIZE_BOUND_ENGINE_BUF,
      SIZE_MAX - NXT_BUF_MEM_SIZE + 1 },

    /* A "body_buffer_size" near SIZE_MAX and a longer body. */
    { "nxt_http_request_body_alloc()", NXT_SIZE_BOUND_BODY, SIZE_MAX - 8 },
};


static nxt_thread_t  *nxt_size_bound_test_thr;


static int
nxt_size_bound_test_child(void *data)
{
    void                     *p;
    nxt_mp_t                 *mp;
    nxt_int_t                ret;
    nxt_socket_conf_t        skcf;
    nxt_http_request_t       r;
    nxt_event_engine_t       engine;
    nxt_socket_conf_joint_t  joint;

    const nxt_size_bound_test_case_t  *tc = data;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return 2;
    }

    p = NULL;

    switch (tc->alloc) {

    case NXT_SIZE_BOUND_MP:
        p = nxt_mp_alloc(mp, tc->size);
        break;

    case NXT_SIZE_BOUND_BUF_MEM:
        p = nxt_buf_mem_alloc(mp, tc->size, 0);
        break;

    case NXT_SIZE_BOUND_BUF_MEM_TS:
        p = nxt_buf_mem_ts_alloc(nxt_size_bound_test_thr->task, mp, tc->size);
        break;

    case NXT_SIZE_BOUND_BUF_FILE:
        p = nxt_buf_file_alloc(mp, tc->size, 0);
        break;

    case NXT_SIZE_BOUND_ENGINE_BUF:
        nxt_memzero(&engine, sizeof(nxt_event_engine_t));
        engine.mem_pool = mp;

        p = nxt_event_engine_buf_mem_alloc(&engine, tc->size);
        break;

    case NXT_SIZE_BOUND_BODY:
        nxt_memzero(&skcf, sizeof(nxt_socket_conf_t));
        nxt_memzero(&joint, sizeof(nxt_socket_conf_joint_t));
        nxt_memzero(&r, sizeof(nxt_http_request_t));

        skcf.body_buffer_size = tc->size;
        skcf.body_temp_path.start = (u_char *) "/tmp";
        skcf.body_temp_path.length = nxt_length("/tmp");

        joint.socket_conf = &skcf;

        r.mem_pool = mp;
        r.conf = &joint;

        ret = nxt_http_request_body_alloc(nxt_size_bound_test_thr->task, &r,
                                          SIZE_MAX);

        p = (ret == NXT_OK) ? (void *) r.body : NULL;
        break;
    }

    if (p != NULL) {
        nxt_log_alert(nxt_size_bound_test_thr->log, "size bound test: "
                      "%s with size %uz returned %p, not NULL",
                      tc->name, tc->size, p);
        return 1;
    }

    return 0;
}


nxt_int_t
nxt_size_bound_test(nxt_thread_t *thr)
{
    int         rc;
    nxt_uint_t  i;

    const nxt_size_bound_test_case_t  *tc;

    nxt_thread_time_update(thr);

    nxt_size_bound_test_thr = thr;

    for (i = 0; i < nxt_nitems(nxt_size_bound_test_cases); i++) {
        tc = &nxt_size_bound_test_cases[i];

        rc = nxt_test_in_child(thr, tc->name, nxt_size_bound_test_child,
                               (void *) tc);

        NXT_TEST_CHECK(thr->log, rc == 0, "size bound test: %s with size %uz "
                       "did not fail (child status %d)", tc->name, tc->size,
                       rc);
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "size bound test passed");

    return NXT_OK;
}
