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
#include <nxt_conf.h>
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


/*
 * NXT_CONF_MAP_SIZE refuses a number out of the range of ssize_t.  A
 * negative number in the range is mapped: a stored configuration from an
 * earlier version can have -1, and it gives SIZE_MAX as before.  With a
 * 32-bit size_t the JSON parser accepts 2^31.  With a 64-bit size_t it
 * refuses 2^63 itself: a number has at most 14 characters and must be within
 * the range of int64_t.  The largest number that it accepts is mapped.
 */

typedef struct {
    const char  *number;
    nxt_int_t   expect;
    size_t      size;
} nxt_size_bound_test_map_t;


static const nxt_size_bound_test_map_t  nxt_size_bound_test_maps[] = {
    { "0",              NXT_OK,    0 },
    { "2147483647",     NXT_OK,    2147483647 },
    { "-1",             NXT_OK,    SIZE_MAX },
#if (NXT_SIZE_T_SIZE == 4)
    { "-2147483648",    NXT_OK,    2147483648U },
    { "2147483648",     NXT_ERROR, 7 },
    { "-2147483649",    NXT_ERROR, 7 },
#else
    { "9.22337203e18",  NXT_OK,    9223372030000000000ULL },
    { "-9.22337203e18", NXT_OK,    (size_t) -9223372030000000000LL },
#endif
};


static nxt_int_t
nxt_size_bound_test_map(nxt_thread_t *thr, nxt_mp_t *mp,
    const nxt_size_bound_test_map_t *tm)
{
    u_char            buf[64], *end;
    size_t            size;
    nxt_int_t         ret;
    nxt_conf_value_t  *cv;

    static const nxt_conf_map_t  map[] = {
        { nxt_string("size"), NXT_CONF_MAP_SIZE, 0 },
    };

    end = nxt_sprintf(buf, buf + sizeof(buf), "{\"size\": %s}", tm->number);

    cv = nxt_conf_json_parse(mp, buf, end, NULL);
    NXT_TEST_CHECK(thr->log, cv != NULL,
                   "size bound test: %s did not parse", tm->number);

    size = 7;
    ret = nxt_conf_map_object(mp, cv, map, nxt_nitems(map), &size);

    NXT_TEST_CHECK(thr->log, ret == tm->expect && size == tm->size,
                   "size bound test: NXT_CONF_MAP_SIZE of %s returned %i, "
                   "size %uz, expected %i, size %uz", tm->number, ret, size,
                   tm->expect, tm->size);

    return NXT_OK;
}


nxt_int_t
nxt_size_bound_test(nxt_thread_t *thr)
{
    int         rc;
    nxt_mp_t    *mp;
    nxt_int_t   ret;
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

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    ret = NXT_OK;

    for (i = 0; i < nxt_nitems(nxt_size_bound_test_maps); i++) {
        ret = nxt_size_bound_test_map(thr, mp, &nxt_size_bound_test_maps[i]);
        if (ret != NXT_OK) {
            break;
        }
    }

    nxt_mp_destroy(mp);

    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "size bound test passed");

    return NXT_OK;
}
