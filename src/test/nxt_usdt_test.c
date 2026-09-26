/*
 * Copyright (C) FreeUnit contributors.
 */

#include <nxt_main.h>
#include <nxt_usdt.h>
#include "nxt_tests.h"


static nxt_uint_t  nxt_usdt_test_evaluated;


/*
 * The contract of NXT_USDT(): without --usdt a probe is nothing, so its
 * arguments are not evaluated at all; with --usdt they are, once.
 */
nxt_int_t
nxt_usdt_test(nxt_thread_t *thr)
{
    nxt_uint_t  expected;

    nxt_usdt_test_evaluated = 0;

    NXT_USDT(test__probe, ++nxt_usdt_test_evaluated);

#if (NXT_HAVE_USDT)
    expected = 1;
#else
    expected = 0;
#endif

    NXT_TEST_CHECK(thr->log, nxt_usdt_test_evaluated == expected,
                   "NXT_USDT() evaluated its argument %ui times, not %ui",
                   nxt_usdt_test_evaluated, expected);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "usdt test passed");

    return NXT_OK;
}
