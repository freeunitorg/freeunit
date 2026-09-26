/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_main.h>
#include <nxt_checked.h>
#include <nxt_span.h>
#include "nxt_tests.h"


nxt_int_t
nxt_checked_test(nxt_thread_t *thr)
{
    size_t         r, extra;
    u_char         dst[4];
    nxt_span_t     span;
    const u_char   *p;

    static const u_char  buf[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

    NXT_TEST_CHECK(thr->log, nxt_size_add(2, 3, &r) == 0 && r == 5,
                   "nxt_size_add(2, 3) failed");
    NXT_TEST_CHECK(thr->log, nxt_size_mul(6, 7, &r) == 0 && r == 42,
                   "nxt_size_mul(6, 7) failed");
    NXT_TEST_CHECK(thr->log, nxt_size_add(SIZE_MAX, 1, &r) != 0,
                   "nxt_size_add(SIZE_MAX, 1) did not overflow");
    NXT_TEST_CHECK(thr->log, nxt_size_mul(SIZE_MAX, 2, &r) != 0,
                   "nxt_size_mul(SIZE_MAX, 2) did not overflow");

    /* Two whole 4-byte records, then a partial tail of 0..3 bytes. */

    for (extra = 0; extra <= 3; extra++) {
        nxt_span_init(&span, buf, buf + 8 + extra);

        NXT_TEST_CHECK(thr->log, nxt_span_take(&span, 4, &p) == 0 && p == buf
                       && nxt_span_take(&span, 4, &p) == 0 && p == buf + 4
                       && nxt_span_len(&span) == extra,
                       "nxt_span_take() rejected a whole record");
        NXT_TEST_CHECK(thr->log, nxt_span_take(&span, 4, &p) != 0,
                       "nxt_span_take() accepted a %uz-byte tail", extra);
    }

    nxt_span_init(&span, buf, buf + 6);

    NXT_TEST_CHECK(thr->log, nxt_span_copy(&span, dst, 4) == 0
                   && memcmp(dst, buf, 4) == 0,
                   "nxt_span_copy() failed on a whole record");

    dst[0] = 0xAA;

    NXT_TEST_CHECK(thr->log, nxt_span_copy(&span, dst, 4) != 0
                   && dst[0] == 0xAA,
                   "nxt_span_copy() accepted or wrote a partial tail");

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_checked test passed");
    return NXT_OK;
}
