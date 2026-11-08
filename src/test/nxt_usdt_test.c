/*
 * Copyright (C) FreeUnit contributors.
 */

#include <nxt_main.h>
#include <nxt_usdt.h>
#include "nxt_tests.h"


/*
 * Without --usdt, NXT_USDT() must not evaluate its arguments.  With it,
 * each argument is evaluated once.  The probes in the source use one and
 * two arguments, so the test fires a probe of each.
 */

nxt_int_t
nxt_usdt_test(nxt_thread_t *thr)
{
    nxt_uint_t  n, m, expected;

    n = 0;
    m = 0;
    NXT_USDT(test__probe, n++);
    NXT_USDT(test__probe2, n++, m++);

#if (NXT_HAVE_USDT)
    expected = 1;
#else
    expected = 0;
#endif

    NXT_TEST_CHECK(thr->log, n == 2 * expected,
                   "NXT_USDT() evaluated its first argument %ui times, "
                   "not %ui", n, 2 * expected);

    NXT_TEST_CHECK(thr->log, m == expected,
                   "NXT_USDT() evaluated its second argument %ui times, "
                   "not %ui", m, expected);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "usdt test passed");

    return NXT_OK;
}
