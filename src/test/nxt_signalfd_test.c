/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_main.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


#if (NXT_HAVE_SIGNALFD)

typedef struct {
    ssize_t     n;
    int         err;
    nxt_bool_t  spurious;
} nxt_signalfd_case_t;


static const nxt_signalfd_case_t  nxt_signalfd_cases[] = {
    { -1, NXT_EAGAIN, 1 },
    { -1, NXT_EINTR, 0 },
    { 0, 0, 0 },
    { 1, 0, 0 },
};


nxt_int_t
nxt_signalfd_test(nxt_thread_t *thr)
{
    nxt_uint_t  i;

    for (i = 0; i < nxt_nitems(nxt_signalfd_cases); i++) {
        NXT_TEST_CHECK(thr->log,
                       nxt_epoll_signalfd_eagain(nxt_signalfd_cases[i].n,
                                                 nxt_signalfd_cases[i].err)
                       == nxt_signalfd_cases[i].spurious,
                       "signalfd read (%d, %d) classified as %d, expected %d",
                       (int) nxt_signalfd_cases[i].n,
                       nxt_signalfd_cases[i].err,
                       (int) nxt_epoll_signalfd_eagain(
                                 nxt_signalfd_cases[i].n,
                                 nxt_signalfd_cases[i].err),
                       (int) nxt_signalfd_cases[i].spurious);
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "signalfd test passed");

    return NXT_OK;
}

#else

nxt_int_t
nxt_signalfd_test(nxt_thread_t *thr)
{
    return NXT_OK;
}

#endif
