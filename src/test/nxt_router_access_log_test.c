/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * The access log record write after a short write or a failed write.
 *
 * The record goes to a pipe.  The fault hooks make the next writes fail with
 * no data or write half of the buffer.  After each step the test reads the
 * pipe and compares the bytes.  It checks that:
 *
 *   - a short write is not resumed, the record is counted, and the next
 *     record starts with "\n" and the marker;
 *   - a record is dropped and counted while the log ends inside a torn
 *     record, also while another thread writes the marker;
 *   - a reopened log does not start with "\n";
 *   - the counters do not wrap at 2^32 on a 32-bit build.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include "nxt_tests.h"


/* "reopen" reopens the log before the record is written. */

typedef struct {
    nxt_uint_t  failed;
    nxt_uint_t  shorts;
    nxt_uint_t  reopen;
    const char  *record;
    const char  *expect;
} nxt_router_access_log_test_step_t;


typedef struct {
    const char                         *name;
    uint64_t                           lost;
    nxt_router_access_log_test_step_t  step[4];
    uint64_t                           expect_lost;
} nxt_router_access_log_test_case_t;


static const nxt_router_access_log_test_case_t
    nxt_router_access_log_cases[] =
{
    { "a short write", 0, {
        { 0, 1, 0, "first record\n", "first " },
        { 0, 0, 0, "second\n", "\n#freeunit: lost 1 record(s)\nsecond\n" },
        { 0, 0, 0, "third\n", "third\n" },
        { 0, 0, 0, NULL, NULL } }, 1 },

    { "a failed marker after a short write", 0, {
        { 0, 1, 0, "first record\n", "first " },
        { 1, 0, 0, "second\n", "" },
        { 0, 0, 0, "third\n", "\n#freeunit: lost 2 record(s)\nthird\n" },
        { 0, 0, 0, NULL, NULL } }, 2 },

    { "a reopen after a short write", 0, {
        { 0, 1, 0, "first record\n", "first " },
        { 0, 0, 1, "second\n", "#freeunit: lost 1 record(s)\nsecond\n" },
        { 0, 0, 0, NULL, NULL } }, 1 },

    { "a failed write", 0, {
        { 1, 0, 0, "first\n", "" },
        { 0, 0, 0, "second\n", "#freeunit: lost 1 record(s)\nsecond\n" },
        { 0, 0, 0, NULL, NULL } }, 1 },

    { "a count past 2^32", 0xffffffff, {
        { 2, 0, 0, "first\n", "" },
        { 0, 0, 0, "second\n",
          "#freeunit: lost 4294967296 record(s)\nsecond\n" },
        { 0, 0, 0, NULL, NULL } }, 0x100000000ULL },
};


static nxt_int_t
nxt_router_access_log_test_read(nxt_thread_t *thr, nxt_fd_t fd,
    const char *name, const char *expect)
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
                   "access log test \"%s\": the log has \"%*s\", "
                   "expected \"%s\"", name, (size_t) nxt_max(n, 0), buf,
                   expect);

    return NXT_OK;
}


nxt_int_t
nxt_router_access_log_test(nxt_thread_t *thr)
{
    int                                      fds[2];
    uint64_t                                 lost;
    nxt_uint_t                               failed;
    nxt_log_t                                log;
    nxt_uint_t                               i, j;
    nxt_task_t                               task;
    nxt_router_access_log_t                  al;
    const nxt_router_access_log_test_step_t  *step;
    const nxt_router_access_log_test_case_t  *c;

    if (pipe(fds) != 0 || fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0) {
        nxt_log_alert(thr->log, "access log test: pipe() failed %E",
                      nxt_errno);
        return NXT_ERROR;
    }

    /* The loss warning is not logged, so no moderation timer is set. */
    log = *thr->log;
    log.level = NXT_LOG_ERR;

    task = *thr->task;
    task.log = &log;

    failed = 0;

    for (i = 0; i < nxt_nitems(nxt_router_access_log_cases); i++) {
        c = &nxt_router_access_log_cases[i];

        nxt_memzero(&al, sizeof(nxt_router_access_log_t));
        al.fd = fds[1];
        nxt_str_set(&al.path, "test");

        if (c->lost != 0) {
            al.lost = c->lost;
            al.pending = c->lost;
            nxt_atomic_store(&al.gap, 1);
        }

        for (j = 0; c->step[j].record != NULL; j++) {
            step = &c->step[j];

            nxt_router_access_log_test_failed_writes = step->failed;
            nxt_router_access_log_test_short_writes = step->shorts;

            if (step->reopen) {
                nxt_router_access_log_test_reopened(&al);
            }

            nxt_router_access_log_test_record(&task, &al,
                                              (u_char *) step->record,
                                              nxt_strlen(step->record));

            if (nxt_router_access_log_test_read(thr, fds[0], c->name,
                                                step->expect)
                != NXT_OK)
            {
                failed++;
            }
        }

        lost = nxt_router_access_log_lost_count(&al);

        if (lost != c->expect_lost) {
            nxt_log_alert(thr->log, "access log test \"%s\": lost %uL, "
                          "expected %uL", c->name, lost, c->expect_lost);
            failed++;
        }
    }

    /*
     * Another thread writes the marker.  A record waits for no lock: it is
     * dropped when the log ends inside a torn record, and written otherwise.
     */

    for (i = 0; i < 2; i++) {
        nxt_memzero(&al, sizeof(nxt_router_access_log_t));
        al.fd = fds[1];
        nxt_str_set(&al.path, "test");
        nxt_atomic_store(&al.gap, 1);
        al.marking = 1;
        al.marking_torn = (i == 0);

        nxt_router_access_log_test_record(&task, &al, (u_char *) "r\n", 2);

        if (nxt_router_access_log_test_read(thr, fds[0], "marking",
                                            (i == 0) ? "" : "r\n")
            != NXT_OK)
        {
            failed++;
        }

        lost = nxt_router_access_log_lost_count(&al);

        if (lost != (uint64_t) (i == 0)) {
            nxt_log_alert(thr->log, "access log test \"marking\": lost %uL",
                          lost);
            failed++;
        }
    }

    if (failed == 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "access log test passed");
    }

    nxt_router_access_log_test_failed_writes = 0;
    nxt_router_access_log_test_short_writes = 0;

    (void) close(fds[0]);
    (void) close(fds[1]);

    return (failed == 0) ? NXT_OK : NXT_ERROR;
}
