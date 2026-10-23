
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include "nxt_tests.h"


/*
 * A child sets its RLIMIT_NOFILE to "soft" and "hard", calls
 * nxt_process_nofile_raise() and compares the result and the new soft
 * limit with "ret" and "out".  A non-zero "err" makes setrlimit() fail.
 * "skip" allows the child to skip the case when it cannot set the
 * limits, because only a privileged process can raise its hard limit.
 * The child also skips the case when the kernel sets a lower hard limit
 * than the case asks for.
 * Linux refuses a hard limit above fs.nr_open, so RLIM_INFINITY is tested
 * only through nxt_process_nofile_target().
 */

typedef struct {
    nxt_thread_t  *thr;
    const char    *name;
    rlim_t        soft;
    rlim_t        hard;
    nxt_err_t     err;
    nxt_int_t     ret;
    rlim_t        out;
    nxt_bool_t    skip;
} nxt_process_nofile_case_t;


#define NXT_NOFILE_SKIPPED  77


/* nxt_process_nofile_target() does not go above OPEN_MAX on macOS. */

#if (NXT_MACOSX) && defined(OPEN_MAX)
#define NXT_NOFILE_CAPPED(n)                                                 \
    ((rlim_t) (n) < (rlim_t) OPEN_MAX ? (rlim_t) (n) : (rlim_t) OPEN_MAX)
#else
#define NXT_NOFILE_CAPPED(n)  ((rlim_t) (n))
#endif


static int nxt_process_nofile_child(void *data);


nxt_int_t
nxt_process_nofile_test(nxt_thread_t *thr)
{
    int         status;
    rlim_t      r;
    nxt_uint_t  i;

    static const struct {
        rlim_t  cur;
        rlim_t  max;
        rlim_t  out;
    } targets[] = {
        { 1024, 524288, NXT_NOFILE_CAPPED(524288) },
        { 1024, RLIM_INFINITY, NXT_NOFILE_CAPPED(1048576) },
        { 1024, 2097152, NXT_NOFILE_CAPPED(1048576) },
        { 1024, 1048576, NXT_NOFILE_CAPPED(1048576) },
        { 1024, 1024, 1024 },
        { 4096, 4096, 4096 },
        { 0, 1024, 1024 },
        { 2097152, RLIM_INFINITY, 2097152 },
        { 2097152, 4194304, 2097152 },
        { RLIM_INFINITY, RLIM_INFINITY, RLIM_INFINITY },
    };

    nxt_process_nofile_case_t  cases[] = {
        { NULL, "soft below hard", 64, 512, 0, NXT_OK, 512, 0 },
        { NULL, "soft equals hard", 512, 512, 0, NXT_DECLINED, 512, 0 },
        { NULL, "setrlimit() fails", 64, 512, NXT_EPERM, NXT_ERROR, 64, 0 },
        { NULL, "hard above the cap", 64, 2097152, 0, NXT_OK,
          NXT_NOFILE_CAPPED(1048576), 1 },
    };

    for (i = 0; i < nxt_nitems(targets); i++) {
        r = nxt_process_nofile_target(targets[i].cur, targets[i].max);

        NXT_TEST_CHECK(thr->log, r == targets[i].out,
                       "nofile target: soft %uL, hard %uL gave %uL, not %uL",
                       (uint64_t) targets[i].cur, (uint64_t) targets[i].max,
                       (uint64_t) r, (uint64_t) targets[i].out);
    }

    for (i = 0; i < nxt_nitems(cases); i++) {
        cases[i].thr = thr;

        status = nxt_test_in_child(thr, cases[i].name,
                                   nxt_process_nofile_child, &cases[i]);

        if (status == NXT_NOFILE_SKIPPED) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "nofile raise \"%s\" skipped: the hard limit "
                          "cannot be set", cases[i].name);
            continue;
        }

        NXT_TEST_CHECK(thr->log, status == 0,
                       "nofile raise \"%s\" failed: child status %d",
                       cases[i].name, status);
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "process nofile test passed");

    return NXT_OK;
}


static int
nxt_process_nofile_child(void *data)
{
    nxt_int_t                  ret;
    struct rlimit              rlim;
    nxt_process_nofile_case_t  *test;

    test = data;

    rlim.rlim_cur = test->soft;
    rlim.rlim_max = test->hard;

    if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        if (test->skip) {
            return NXT_NOFILE_SKIPPED;
        }

        nxt_log_alert(test->thr->log, "nofile raise \"%s\": setrlimit(%uL, "
                      "%uL) failed %E", test->name, (uint64_t) test->soft,
                      (uint64_t) test->hard, nxt_errno);
        return 1;
    }

    /*
     * FreeBSD and OpenBSD do not refuse a hard limit above
     * kern.maxfilesperproc.  They set the hard limit to that value.  Then
     * the case does not test what it asks for, so the child skips it.
     */

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        nxt_log_alert(test->thr->log, "nofile raise \"%s\": getrlimit() "
                      "failed %E", test->name, nxt_errno);
        return 1;
    }

    if (test->skip && rlim.rlim_max != test->hard) {
        return NXT_NOFILE_SKIPPED;
    }

    nxt_process_test_nofile_failure(test->err);

    ret = nxt_process_nofile_raise(test->thr->task);

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        nxt_log_alert(test->thr->log, "nofile raise \"%s\": getrlimit() "
                      "failed %E", test->name, nxt_errno);
        return 1;
    }

    if (ret != test->ret || rlim.rlim_cur != test->out) {
        nxt_log_alert(test->thr->log, "nofile raise \"%s\": returned %i, "
                      "soft limit %uL, expected %i and %uL", test->name, ret,
                      (uint64_t) rlim.rlim_cur, test->ret,
                      (uint64_t) test->out);
        return 1;
    }

    return 0;
}
