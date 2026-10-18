/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * The access log record write after a short write or a failed write
 * (src/nxt_router_access_log.c).
 *
 * The log is a pipe.  The fault hooks set the results of the next write()
 * calls: a call fails before any byte, or writes at most n bytes.  After each
 * step the test reads the pipe and compares the bytes, and after each case
 * it compares the counters.  It checks that:
 *
 *   - a short write is completed by the next writes, within three write()
 *     calls; a call interrupted by a signal is not one of them;
 *   - a record still incomplete after the third write() is torn and counted,
 *     also when every call takes one byte;
 *   - a record that ends inside its line is counted, and the next record
 *     writes the missing "\n", then the marker, then itself;
 *   - a write that fails before its first byte leaves no "\n" behind;
 *   - the marker comes before the record that follows the gap, also after a
 *     record that could not be built;
 *   - a failed or a torn marker is written again with the next record, and
 *     the record it should have preceded is counted;
 *   - the reopen ends a torn record in the old file, and the new file starts
 *     with the marker and the next record; a failed repair there is an alert;
 *   - the counters do not wrap at 2^32 on a 32-bit build;
 *   - a path of a log that an old configuration still uses gets that log,
 *     not a second one with its own mutex; a reopen reply holds its log.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include "nxt_tests.h"


/* A fault: the write() call fails with ENOSPC before any byte. */
#define F  (-1)

/* A fault: the write() call fails with EINTR before any byte. */
#define INTR  (-2)

/* A fault that lets the call write the whole buffer of any record here. */
#define P  1024


enum {
    NXT_ACCESS_LOG_TEST_RECORD = 0,
    NXT_ACCESS_LOG_TEST_LOST,
    NXT_ACCESS_LOG_TEST_REOPEN,
};


/*
 * One step: write a record, count a record that could not be built, or
 * reopen the log on the second pipe.  log[0] and log[1] are the bytes that
 * the first and the second pipe get from the step.
 */

typedef struct {
    nxt_uint_t  action;
    const char  *record;
    nxt_uint_t  nfaults;
    ssize_t     fault[16];
    const char  *log[2];
} nxt_router_access_log_test_step_t;


typedef struct {
    const char                         *name;
    uint64_t                           lost;
    nxt_router_access_log_test_step_t  step[4];
    uint64_t                           expect_lost;
    const char                         *expect_alert;
} nxt_router_access_log_test_case_t;


#define R  NXT_ACCESS_LOG_TEST_RECORD
#define L  NXT_ACCESS_LOG_TEST_LOST
#define O  NXT_ACCESS_LOG_TEST_REOPEN


static const nxt_router_access_log_test_case_t
    nxt_router_access_log_cases[] =
{
    { "a short write is completed", 0, {
        { R, "first record\n", 1, { 6 }, { "first record\n", "" } },
        { R, "second\n", 0, { 0 }, { "second\n", "" } } }, 0, NULL },

    { "two short writes are completed by the third write", 0, {
        { R, "first record\n", 2, { 1, 1 }, { "first record\n", "" } } },
      0, NULL },

    { "an interrupted write is not counted", 0, {
        { R, "first record\n", 5, { 1, INTR, 1, INTR, INTR },
          { "first record\n", "" } } }, 0, NULL },

    { "a write of one byte per call is torn after three writes", 0, {
        { R, "first record\n", 3, { 1, 1, 1 }, { "fir", "" } },
        { R, "second\n", 0, { 0 },
          { "\n#freeunit: lost 1 record(s)\nsecond\n", "" } } }, 1, NULL },

    { "a short write and a write of no byte", 0, {
        { R, "first record\n", 2, { 3, 0 }, { "fir", "" } },
        { R, "second\n", 0, { 0 },
          { "\n#freeunit: lost 1 record(s)\nsecond\n", "" } } }, 1, NULL },

    { "a short write and a failed retry", 0, {
        { R, "first record\n", 2, { 6, F }, { "first ", "" } },
        { R, "second\n", 0, { 0 },
          { "\n#freeunit: lost 1 record(s)\nsecond\n", "" } },
        { R, "third\n", 0, { 0 }, { "third\n", "" } } }, 1, NULL },

    { "a failed repair", 0, {
        { R, "first record\n", 2, { 6, F }, { "first ", "" } },
        { R, "second\n", 1, { F }, { "", "" } },
        { R, "third\n", 0, { 0 },
          { "\n#freeunit: lost 2 record(s)\nthird\n", "" } } }, 2, NULL },

    { "a failed marker", 0, {
        { R, "first record\n", 2, { 6, F }, { "first ", "" } },
        { R, "second\n", 2, { P, F }, { "\n", "" } },
        { R, "third\n", 0, { 0 },
          { "#freeunit: lost 2 record(s)\nthird\n", "" } } }, 2, NULL },

    { "a torn marker", 0, {
        { R, "first\n", 1, { F }, { "", "" } },
        { R, "second\n", 2, { 10, F }, { "#freeunit:", "" } },
        { R, "third\n", 0, { 0 },
          { "\n#freeunit: lost 2 record(s)\nthird\n", "" } } }, 2, NULL },

    { "a write that fails before its first byte", 0, {
        { R, "first\n", 1, { F }, { "", "" } },
        { R, "second\n", 0, { 0 },
          { "#freeunit: lost 1 record(s)\nsecond\n", "" } } }, 1, NULL },

    { "a write of no byte", 0, {
        { R, "first\n", 1, { 0 }, { "", "" } },
        { R, "second\n", 0, { 0 },
          { "#freeunit: lost 1 record(s)\nsecond\n", "" } } }, 1, NULL },

    { "a record that could not be built", 0, {
        { L, NULL, 0, { 0 }, { "", "" } },
        { R, "second\n", 0, { 0 },
          { "#freeunit: lost 1 record(s)\nsecond\n", "" } } }, 1, NULL },

    { "a reopen after a torn record", 0, {
        { R, "first record\n", 2, { 6, F }, { "first ", "" } },
        { O, NULL, 0, { 0 }, { "\n", "" } },
        { R, "second\n", 0, { 0 },
          { "", "#freeunit: lost 1 record(s)\nsecond\n" } } }, 1, NULL },

    { "a reopen whose repair fails", 0, {
        { R, "first record\n", 2, { 6, F }, { "first ", "" } },
        { O, NULL, 1, { F }, { "", "" } },
        { R, "second\n", 0, { 0 },
          { "", "#freeunit: lost 1 record(s)\nsecond\n" } } }, 1,
      "access log \"test\": the torn line was not repaired before the "
      "reopen" },

    { "a reopen after a failed write", 0, {
        { R, "first\n", 1, { F }, { "", "" } },
        { O, NULL, 0, { 0 }, { "", "" } },
        { R, "second\n", 0, { 0 },
          { "", "#freeunit: lost 1 record(s)\nsecond\n" } } }, 1, NULL },

    { "a count past 2^32", 0xffffffff, {
        { R, "first\n", 1, { F }, { "", "" } },
        { R, "second\n", 0, { 0 },
          { "#freeunit: lost 4294967296 record(s)\nsecond\n", "" } } },
      0x100000000ULL, NULL },
};


#undef R
#undef L
#undef O


/* The alerts that the writer logs, for the case that expects one. */
static nxt_uint_t  nxt_router_access_log_test_alerts;
static u_char      nxt_router_access_log_test_alert[256];
static size_t      nxt_router_access_log_test_alert_length;


static void nxt_cdecl
nxt_router_access_log_test_log_handler(nxt_uint_t level, nxt_log_t *log,
    const char *fmt, ...)
{
    u_char   *p, *end;
    va_list  args;

    if (level != NXT_LOG_ALERT) {
        return;
    }

    nxt_router_access_log_test_alerts++;

    p = nxt_router_access_log_test_alert;
    end = p + sizeof(nxt_router_access_log_test_alert);

    va_start(args, fmt);
    p = nxt_vsprintf(p, end, fmt, args);
    va_end(args);

    nxt_router_access_log_test_alert_length = p
                                          - nxt_router_access_log_test_alert;
}


static nxt_int_t
nxt_router_access_log_test_read(nxt_thread_t *thr, nxt_fd_t fd,
    const char *name, nxt_uint_t step, const char *expect)
{
    u_char   buf[256];
    size_t   length;
    ssize_t  n;

    n = read(fd, buf, sizeof(buf));

    if (n == -1 && nxt_errno == NXT_EAGAIN) {
        n = 0;
    }

    length = nxt_strlen(expect);

    NXT_TEST_CHECK(thr->log, n >= 0 && (size_t) n == length
                             && memcmp(buf, expect, length) == 0,
                   "access log test \"%s\", step %ui: the log has \"%*s\", "
                   "expected \"%s\"", name, step, (size_t) nxt_max(n, 0),
                   buf, expect);

    return NXT_OK;
}


static nxt_int_t
nxt_router_access_log_test_case(nxt_thread_t *thr, nxt_task_t *task,
    const nxt_router_access_log_test_case_t *c, int fds[2][2])
{
    nxt_int_t                                ret;
    nxt_uint_t                               i, j;
    nxt_router_access_log_t                  al;
    const nxt_router_access_log_test_step_t  *step;

    nxt_memzero(&al, sizeof(nxt_router_access_log_t));
    nxt_str_set(&al.path, "test");

    /* The reopen holds the log for its reply; the case holds it as well. */
    al.count = 1;

    al.lost = c->lost;
    al.pending = c->lost;

    if (nxt_thread_mutex_create(&al.mutex) != NXT_OK) {
        nxt_log_alert(thr->log, "access log test: mutex create failed");
        return NXT_ERROR;
    }

    /* The reopen replaces al.fd, so it is a descriptor of its own. */
    al.fd = dup(fds[0][1]);

    if (al.fd == -1) {
        nxt_log_alert(thr->log, "access log test: dup() failed %E",
                      nxt_errno);
        nxt_thread_mutex_destroy(&al.mutex);
        return NXT_ERROR;
    }

    nxt_router->access_log = &al;

    nxt_router_access_log_test_alerts = 0;
    nxt_router_access_log_test_alert_length = 0;

    ret = NXT_OK;

    for (i = 0; i < nxt_nitems(c->step); i++) {
        step = &c->step[i];

        if (step->record == NULL && step->action == NXT_ACCESS_LOG_TEST_RECORD)
        {
            break;
        }

        nxt_router_access_log_test_fault = step->fault;
        nxt_router_access_log_test_faults = step->nfaults;

        switch (step->action) {

        case NXT_ACCESS_LOG_TEST_RECORD:
            nxt_router_access_log_test_record(task, &al,
                                              (u_char *) step->record,
                                              nxt_strlen(step->record));
            break;

        case NXT_ACCESS_LOG_TEST_LOST:
            nxt_router_access_log_test_lost(task, &al);
            break;

        default: /* NXT_ACCESS_LOG_TEST_REOPEN */
            if (nxt_router_access_log_test_reopen_ready(task, &al,
                                                        dup(fds[1][1]))
                != NXT_OK)
            {
                nxt_log_alert(thr->log, "access log test \"%s\": "
                              "the reopen failed", c->name);
                ret = NXT_ERROR;
            }

            break;
        }

        /* Every fault was used: the step made the writes it should. */
        if (nxt_router_access_log_test_faults != 0) {
            nxt_log_alert(thr->log, "access log test \"%s\", step %ui: "
                          "%ui faults left", c->name, i,
                          nxt_router_access_log_test_faults);
            ret = NXT_ERROR;
        }

        nxt_router_access_log_test_faults = 0;

        for (j = 0; j < 2; j++) {
            if (nxt_router_access_log_test_read(thr, fds[j][0], c->name, i,
                                                step->log[j])
                != NXT_OK)
            {
                ret = NXT_ERROR;
            }
        }
    }

    if (al.lost != c->expect_lost || al.pending != 0 || al.torn) {
        nxt_log_alert(thr->log, "access log test \"%s\": lost %uL, pending "
                      "%uL, torn %d, expected lost %uL, pending 0, torn 0",
                      c->name, al.lost, al.pending, (int) al.torn,
                      c->expect_lost);
        ret = NXT_ERROR;
    }

    if (c->expect_alert == NULL) {
        if (nxt_router_access_log_test_alerts != 0) {
            nxt_log_alert(thr->log, "access log test \"%s\": %ui alert(s), "
                          "the last \"%*s\", expected none", c->name,
                          nxt_router_access_log_test_alerts,
                          nxt_router_access_log_test_alert_length,
                          nxt_router_access_log_test_alert);
            ret = NXT_ERROR;
        }

    } else if (nxt_router_access_log_test_alerts != 1
               || nxt_router_access_log_test_alert_length
                  != nxt_strlen(c->expect_alert)
               || memcmp(nxt_router_access_log_test_alert, c->expect_alert,
                         nxt_router_access_log_test_alert_length)
                  != 0)
    {
        nxt_log_alert(thr->log, "access log test \"%s\": %ui alert(s), "
                      "the last \"%*s\", expected one \"%s\"", c->name,
                      nxt_router_access_log_test_alerts,
                      nxt_router_access_log_test_alert_length,
                      nxt_router_access_log_test_alert, c->expect_alert);
        ret = NXT_ERROR;
    }

    nxt_router->access_log = NULL;

    (void) close(al.fd);
    nxt_thread_mutex_destroy(&al.mutex);

    return ret;
}


/*
 * The paths go X, then Y, then X while the first X is still used.  The
 * second X is the first object, and a log is not found after its release.
 */

static nxt_int_t
nxt_router_access_log_test_live(nxt_thread_t *thr, nxt_task_t *task,
    nxt_router_t *router)
{
    nxt_fd_t                 fd;
    nxt_int_t                ret;
    nxt_str_t                x, y;
    nxt_uint_t               n;
    nxt_queue_link_t         *lnk;
    nxt_router_access_log_t  *x1, *y1, *x2;

    nxt_str_set(&x, "x.log");
    nxt_str_set(&y, "y.log");

    x1 = nxt_router_access_log_get(task, router, &x);
    y1 = nxt_router_access_log_get(task, router, &y);

    /* Y is the current log; the old configuration still holds X. */
    router->access_log = y1;

    x2 = nxt_router_access_log_get(task, router, &x);

    ret = NXT_OK;

    if (x1 == NULL || y1 == NULL || x2 != x1 || x1->count != 2) {
        nxt_log_alert(thr->log, "access log test \"a live log is reused\": "
                      "the second X is %p, expected %p, count %uD",
                      x2, x1, (x1 != NULL) ? x1->count : 0);
        ret = NXT_ERROR;
    }

    /*
     * A reopen reply for the old X while Y is current: it is not applied,
     * and the hold the reopen took on X is released.
     */
    if (ret == NXT_OK) {
        fd = open("/dev/null", O_WRONLY);

        if (fd == -1
            || nxt_router_access_log_test_reopen_ready(task, x1, fd) != NXT_OK
            || x1->count != 2 || x1->fd != -1)
        {
            nxt_log_alert(thr->log, "access log test \"a reopen reply holds "
                          "its log\": count %uD, fd %FD, expected 2, -1",
                          x1->count, x1->fd);
            ret = NXT_ERROR;
        }
    }

    if (x2 != NULL && x2 != x1) {
        nxt_router_access_log_release(task, &router->lock, x2);
    }

    nxt_router_access_log_release(task, &router->lock, x1);
    nxt_router_access_log_release(task, &router->lock, x1);
    nxt_router_access_log_release(task, &router->lock, y1);

    router->access_log = NULL;

    n = 0;

    for (lnk = nxt_queue_first(&router->access_logs);
         lnk != nxt_queue_tail(&router->access_logs);
         lnk = nxt_queue_next(lnk))
    {
        n++;
    }

    if (n != 0) {
        nxt_log_alert(thr->log, "access log test \"a released log is not "
                      "found\": %ui live logs, expected 0", n);
        ret = NXT_ERROR;
    }

    return ret;
}


nxt_int_t
nxt_router_access_log_test(nxt_thread_t *thr)
{
    int           fds[2][2];
    nxt_int_t     ret;
    nxt_log_t     log;
    nxt_uint_t    i, failed;
    nxt_task_t    task;
    nxt_router_t  router, *saved_router;

    for (i = 0; i < 2; i++) {
        if (pipe(fds[i]) != 0 || fcntl(fds[i][0], F_SETFL, O_NONBLOCK) != 0) {
            nxt_log_alert(thr->log, "access log test: pipe() failed %E",
                          nxt_errno);
            return NXT_ERROR;
        }
    }

    /*
     * The loss warning is not logged, so no moderation timer is set.  The
     * alerts of the writer are kept for the case to check.
     */
    log = *thr->log;
    log.level = NXT_LOG_ERR;
    log.handler = nxt_router_access_log_test_log_handler;

    task = *thr->task;
    task.log = &log;

    /* The reopen reply applies to the log of the current router. */
    saved_router = nxt_router;
    nxt_memzero(&router, sizeof(nxt_router_t));
    nxt_queue_init(&router.access_logs);
    nxt_router = &router;

    failed = 0;

    for (i = 0; i < nxt_nitems(nxt_router_access_log_cases); i++) {
        ret = nxt_router_access_log_test_case(thr, &task,
                                              &nxt_router_access_log_cases[i],
                                              fds);
        if (ret != NXT_OK) {
            failed++;
        }
    }

    if (nxt_router_access_log_test_live(thr, &task, &router) != NXT_OK) {
        failed++;
    }

    nxt_router = saved_router;

    nxt_router_access_log_test_fault = NULL;
    nxt_router_access_log_test_faults = 0;

    for (i = 0; i < 2; i++) {
        (void) close(fds[i][0]);
        (void) close(fds[i][1]);
    }

    if (failed != 0) {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "access log test passed");

    return NXT_OK;
}
