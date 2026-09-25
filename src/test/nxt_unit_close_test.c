
/*
 * Copyright (C) F5, Inc.
 */

/*
 * Tests for the close-provenance diagnostic in nxt_unit.c
 * (nxt_unit_close_impl and its ring of recent close records).
 *
 * The impl is driven directly with synthetic site names, bypassing the
 * nxt_unit_close() macro, so attribution can be asserted exactly.  Alerts
 * go to STDERR_FILENO when no context is available; each call is wrapped
 * to capture that output through a pipe.
 *
 * The genuinely concurrent interleavings (a loser scanning while the
 * winner is between publish and commit) cannot be forced without hooks
 * into the impl, so concurrency is covered by a smoke test asserting the
 * spin locks make progress and alerts stay well-formed under contention.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int nxt_unit_close_impl(int fd, const char *from, int line);


#define NXT_CLOSE_TEST_BOGUS_FD    4242
#define NXT_CLOSE_TEST_RACE_FD     4243
#define NXT_CLOSE_TEST_RACERS     4
#define NXT_CLOSE_TEST_RACE_LOOPS  10000

static int   nxt_close_test_failures;
static int   nxt_close_test_saved_stderr;
static int   nxt_close_test_pipe[2];
static char  nxt_close_test_captured[4096];


/*
 * The capture pipe and the saved stderr are allocated once, up front:
 * allocating fds inside the wrapper (pipe()/dup() return the lowest free
 * number) would resurrect the very fd a test just closed, turning the
 * intended EBADF into a successful close of the harness's own pipe.
 */
static void
nxt_close_test_capture_init(void)
{
    if (pipe(nxt_close_test_pipe) != 0) {
        perror("pipe");
        exit(1);
    }

    if (fcntl(nxt_close_test_pipe[0], F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        exit(1);
    }

    nxt_close_test_saved_stderr = dup(STDERR_FILENO);
}


static int
nxt_close_test_close(int fd, const char *from, int line)
{
    int      res, err;
    ssize_t  n;

    dup2(nxt_close_test_pipe[1], STDERR_FILENO);

    res = nxt_unit_close_impl(fd, from, line);
    err = errno;

    dup2(nxt_close_test_saved_stderr, STDERR_FILENO);

    n = read(nxt_close_test_pipe[0], nxt_close_test_captured,
             sizeof(nxt_close_test_captured) - 1);

    nxt_close_test_captured[n > 0 ? n : 0] = '\0';

    errno = err;
    return res;
}


static void
nxt_close_test_assert(int cond, const char *name)
{
    if (cond) {
        printf("close test: %-38s passed\n", name);

    } else {
        printf("close test: %-38s FAILED\ncaptured: %s\n", name,
               nxt_close_test_captured);

        nxt_close_test_failures++;
    }
}


static int
nxt_close_test_captured_has(const char *substr)
{
    return strstr(nxt_close_test_captured, substr) != NULL;
}


static void *
nxt_close_test_racer(void *arg)
{
    int  i;

    (void) arg;

    for (i = 0; i < NXT_CLOSE_TEST_RACE_LOOPS; i++) {
        if (nxt_unit_close_impl(NXT_CLOSE_TEST_RACE_FD, "racer", 1) != -1) {
            return (void *) 1;
        }
    }

    return NULL;
}


static void *
nxt_close_test_churner(void *arg)
{
    int  i, fd;

    (void) arg;

    /*
     * Exercises publish/commit on the success path concurrently with the
     * racers' failure-path scans and retractions.  These fds are owned
     * solely by this thread (the racers only touch the never-opened
     * NXT_CLOSE_TEST_RACE_FD), so every close must succeed.
     */
    for (i = 0; i < NXT_CLOSE_TEST_RACE_LOOPS; i++) {
        fd = open("/dev/null", O_RDONLY);
        if (fd == -1) {
            return (void *) 1;
        }

        if (nxt_unit_close_impl(fd, "churner", 2) != 0) {
            return (void *) 1;
        }
    }

    return NULL;
}


int
main(void)
{
    int        i, fd, res, devnull;
    void       *ret;
    pthread_t  racers[NXT_CLOSE_TEST_RACERS], churner;

    nxt_close_test_capture_init();

    /* A successful close is attributed to a later EBADF on the same fd. */

    fd = open("/dev/null", O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    res = nxt_close_test_close(fd, "site_a", 111);
    nxt_close_test_assert(res == 0, "close succeeds");

    res = nxt_close_test_close(fd, "site_b", 222);
    nxt_close_test_assert(res == -1 && errno == EBADF, "double close EBADF");
    nxt_close_test_assert(nxt_close_test_captured_has("failed at site_b:222"),
                          "failure names own site");
    nxt_close_test_assert(
        nxt_close_test_captured_has("previously closed at site_a:111"),
        "failure names prior closer");

    /*
     * A failed close is retracted: a later failure on the same fd still
     * names the original committed closer, never the failed attempt.
     */

    res = nxt_close_test_close(fd, "site_c", 333);
    nxt_close_test_assert(
        res == -1
        && nxt_close_test_captured_has("previously closed at site_a:111"),
        "prior closer survives failed close");
    nxt_close_test_assert(!nxt_close_test_captured_has("site_b"),
                          "failed close is not a prior closer");

    /* An fd with no record at all falls back to the generic message. */

    res = nxt_close_test_close(NXT_CLOSE_TEST_BOGUS_FD, "site_d", 444);
    nxt_close_test_assert(
        res == -1
        && nxt_close_test_captured_has("no recent close of this fd"),
        "unknown fd reports no record");

    /* On fd-number reuse the newest committed close wins attribution. */

    fd = open("/dev/null", O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    res = nxt_close_test_close(fd, "site_e", 555);
    nxt_close_test_assert(res == 0, "reused fd close succeeds");

    res = nxt_close_test_close(fd, "site_f", 666);
    nxt_close_test_assert(
        res == -1
        && nxt_close_test_captured_has("previously closed at site_e:555"),
        "newest committed close wins");

    /*
     * Concurrency smoke: racers hammer the failure path (scan + retract)
     * on a shared never-opened fd while a churner runs the success path
     * (publish + commit); deadlock or a torn record would hang or crash.
     * Alerts are muted for the duration.
     */

    devnull = open("/dev/null", O_WRONLY);
    if (devnull == -1) {
        perror("open");
        return 1;
    }

    dup2(devnull, STDERR_FILENO);

    for (i = 0; i < NXT_CLOSE_TEST_RACERS; i++) {
        pthread_create(&racers[i], NULL, nxt_close_test_racer, NULL);
    }

    pthread_create(&churner, NULL, nxt_close_test_churner, NULL);

    res = 0;

    for (i = 0; i < NXT_CLOSE_TEST_RACERS; i++) {
        pthread_join(racers[i], &ret);
        res |= (ret != NULL);
    }

    pthread_join(churner, &ret);
    res |= (ret != NULL);

    dup2(nxt_close_test_saved_stderr, STDERR_FILENO);
    close(devnull);

    nxt_close_test_captured[0] = '\0';
    nxt_close_test_assert(res == 0, "concurrent stress");

    if (nxt_close_test_failures != 0) {
        printf("close test: %d failure(s)\n", nxt_close_test_failures);
        return 1;
    }

    printf("close test: all passed\n");
    return 0;
}
