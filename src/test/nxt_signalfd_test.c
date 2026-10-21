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
    { -1, 0, 0 },
    { 0, 0, 0 },
    { 1, 0, 0 },
    { 0, NXT_EAGAIN, 0 },
    { 1, NXT_EAGAIN, 0 },
};


typedef struct {
    ssize_t     size;    /* siginfo bytes in the pipe; -1: fd -1 */
    const char  *alert;
    nxt_uint_t  dispatches;
    uint32_t    signo;   /* the last signal dispatched */
} nxt_signalfd_handler_case_t;


static const nxt_signalfd_handler_case_t  nxt_signalfd_handler_cases[] = {
    { 0, NULL, 0, 0 },                      /* EAGAIN: a spurious wakeup */
    { -1, "failed", 0, 0 },                 /* EBADF */
    { 2, "returned 2", 0, 0 },              /* a short read */
    { sizeof(struct signalfd_siginfo), NULL, 1, SIGHUP },
    /* Two different signals pending: one call reads both. */
    { 2 * sizeof(struct signalfd_siginfo), NULL, 2, SIGCHLD },
};


static nxt_uint_t  nxt_signalfd_alerts;
static u_char      nxt_signalfd_alert[128];
static nxt_uint_t  nxt_signalfd_dispatches;
static uint32_t    nxt_signalfd_signo;


static void nxt_cdecl
nxt_signalfd_log_handler(nxt_uint_t level, nxt_log_t *log, const char *fmt, ...)
{
    u_char   *p;
    va_list  args;

    if (level == NXT_LOG_ALERT) {
        nxt_signalfd_alerts++;

        va_start(args, fmt);
        p = nxt_vsprintf(nxt_signalfd_alert,
                         nxt_signalfd_alert + sizeof(nxt_signalfd_alert) - 1,
                         fmt, args);
        va_end(args);

        *p = '\0';
    }
}


static void
nxt_signalfd_dispatch(nxt_task_t *task, void *obj, void *data)
{
    nxt_signalfd_dispatches++;
    nxt_signalfd_signo = (uintptr_t) obj;
}


static nxt_int_t
nxt_signalfd_handler_test(nxt_thread_t *thr, nxt_fd_t *fds)
{
    nxt_uint_t                         i;
    nxt_log_t                          log;
    nxt_task_t                         task;
    nxt_fd_event_t                     ev;
    const nxt_signalfd_handler_case_t  *c;

    static const struct signalfd_siginfo  si[2] = {
        { .ssi_signo = SIGHUP }, { .ssi_signo = SIGCHLD }
    };

    NXT_TEST_CHECK(thr->log, fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0,
                   "fcntl() failed %E", nxt_errno);

    nxt_memzero(&log, sizeof(nxt_log_t));
    log.level = NXT_LOG_INFO;
    log.handler = nxt_signalfd_log_handler;

    nxt_memzero(&task, sizeof(nxt_task_t));
    task.log = &log;

    nxt_memzero(&ev, sizeof(nxt_fd_event_t));

    for (i = 0; i < nxt_nitems(nxt_signalfd_handler_cases); i++) {
        c = &nxt_signalfd_handler_cases[i];

        NXT_TEST_CHECK(thr->log,
                       c->size <= 0 || write(fds[1], si, c->size) == c->size,
                       "write() failed %E", nxt_errno);

        ev.fd = (c->size == -1) ? -1 : fds[0];

        nxt_signalfd_alerts = 0;
        nxt_signalfd_alert[0] = '\0';
        nxt_signalfd_dispatches = 0;
        nxt_signalfd_signo = 0;

        nxt_epoll_test_signalfd_handler(&task, &ev, nxt_signalfd_dispatch);

        NXT_TEST_CHECK(thr->log,
                       nxt_signalfd_alerts == (c->alert != NULL)
                       && (c->alert == NULL
                           || strstr((char *) nxt_signalfd_alert, c->alert))
                       && nxt_signalfd_dispatches == c->dispatches
                       && nxt_signalfd_signo == c->signo,
                       "signalfd handler case %ui: %ui alerts \"%s\", "
                       "%ui dispatches of signal %uD",
                       i, nxt_signalfd_alerts, nxt_signalfd_alert,
                       nxt_signalfd_dispatches, nxt_signalfd_signo);
    }

    return NXT_OK;
}


nxt_int_t
nxt_signalfd_test(nxt_thread_t *thr)
{
    nxt_fd_t    fds[2];
    nxt_int_t   ret;
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

    NXT_TEST_CHECK(thr->log, pipe(fds) == 0, "pipe() failed %E", nxt_errno);

    ret = nxt_signalfd_handler_test(thr, fds);

    close(fds[0]);
    close(fds[1]);

    if (ret != NXT_OK) {
        return NXT_ERROR;
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
