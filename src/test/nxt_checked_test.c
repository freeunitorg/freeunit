/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_main.h>
#include <nxt_checked.h>
#include <nxt_span.h>
#include "nxt_tests.h"


typedef struct {
    size_t  a;
    size_t  b;
    size_t  result;
    int     overflow;
} nxt_checked_case_t;


static const nxt_checked_case_t  nxt_checked_add_cases[] = {
    { 0, 0, 0, 0 },
    { 2, 3, 5, 0 },
    { SIZE_MAX, 0, SIZE_MAX, 0 },
    { 0, SIZE_MAX, SIZE_MAX, 0 },
    { SIZE_MAX - 1, 1, SIZE_MAX, 0 },
    { SIZE_MAX, 1, 0, 1 },
    { 1, SIZE_MAX, 0, 1 },
    { SIZE_MAX, SIZE_MAX, 0, 1 },
};


static const nxt_checked_case_t  nxt_checked_mul_cases[] = {
    { 0, 0, 0, 0 },
    { 0, SIZE_MAX, 0, 0 },
    { SIZE_MAX, 0, 0, 0 },
    { 6, 7, 42, 0 },
    { SIZE_MAX, 1, SIZE_MAX, 0 },
    { 1, SIZE_MAX, SIZE_MAX, 0 },
    { SIZE_MAX / 2, 2, SIZE_MAX - 1, 0 },
    { SIZE_MAX / 2 + 1, 2, 0, 1 },
    { SIZE_MAX, 2, 0, 1 },
    { 2, SIZE_MAX, 0, 1 },
    { SIZE_MAX, SIZE_MAX, 0, 1 },
};


/*
 * Runs one add or mul function over its table.  The builtin and the
 * portable functions must agree on every row, so both are run over the
 * same table.
 */

static nxt_int_t
nxt_checked_run_table(nxt_thread_t *thr, const char *name,
    int (*fn)(size_t, size_t, size_t *), const nxt_checked_case_t *cases,
    nxt_uint_t n)
{
    size_t      r;
    nxt_uint_t  i;

    for (i = 0; i < n; i++) {
        r = 0xAB;

        if (cases[i].overflow) {
            NXT_TEST_CHECK(thr->log, fn(cases[i].a, cases[i].b, &r) != 0,
                           "%s(%uz, %uz) did not overflow",
                           name, cases[i].a, cases[i].b);
            continue;
        }

        NXT_TEST_CHECK(thr->log, fn(cases[i].a, cases[i].b, &r) == 0
                       && r == cases[i].result,
                       "%s(%uz, %uz) failed: %uz, expected %uz",
                       name, cases[i].a, cases[i].b, r, cases[i].result);
    }

    return NXT_OK;
}


nxt_int_t
nxt_checked_test(nxt_thread_t *thr)
{
    size_t         extra;
    u_char         dst[4];
    nxt_span_t     span;
    const u_char   *p;

    static const u_char  buf[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

    if (nxt_checked_run_table(thr, "nxt_size_add", nxt_size_add,
                              nxt_checked_add_cases,
                              nxt_nitems(nxt_checked_add_cases)) != NXT_OK
        || nxt_checked_run_table(thr, "nxt_size_add_portable",
                                 nxt_size_add_portable, nxt_checked_add_cases,
                                 nxt_nitems(nxt_checked_add_cases)) != NXT_OK
        || nxt_checked_run_table(thr, "nxt_size_mul", nxt_size_mul,
                                 nxt_checked_mul_cases,
                                 nxt_nitems(nxt_checked_mul_cases)) != NXT_OK
        || nxt_checked_run_table(thr, "nxt_size_mul_portable",
                                 nxt_size_mul_portable, nxt_checked_mul_cases,
                                 nxt_nitems(nxt_checked_mul_cases)) != NXT_OK)
    {
        return NXT_ERROR;
    }

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

    /* A copy of 0 bytes succeeds and changes neither the span nor dst. */

    NXT_TEST_CHECK(thr->log, nxt_span_copy(&span, dst, 0) == 0
                   && span.pos == buf + 4 && span.end == buf + 6
                   && dst[0] == 0xAA,
                   "nxt_span_copy() of 0 bytes failed on a non-empty span");

    /* The same on an empty span with pos == end == NULL. */

    nxt_span_init(&span, NULL, NULL);

    NXT_TEST_CHECK(thr->log, nxt_span_copy(&span, dst, 0) == 0
                   && span.pos == NULL && span.end == NULL
                   && dst[0] == 0xAA,
                   "nxt_span_copy() of 0 bytes failed on an empty span");

    NXT_TEST_CHECK(thr->log, nxt_span_copy(&span, dst, 1) != 0
                   && span.pos == NULL && dst[0] == 0xAA,
                   "nxt_span_copy() accepted a byte from an empty span");

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_checked test passed");
    return NXT_OK;
}
