/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * How many idle contexts one READ_QUEUE datagram on the shared port wakes
 * (nxt_unit_wait_set_wait() in src/nxt_unit.c).
 *
 * Each of NXT_WAKE_TEST_THREADS threads plays an idle context: it has a
 * read port of its own, and it waits on that port and on one shared port,
 * as nxt_unit_read_buf() does.  A woken thread does not read the datagram,
 * as a context that took the request from the queue does not.
 *
 *   1. With poll(), one datagram wakes every thread, and the datagram stays
 *      in the socket.
 *   2. A woken thread that left the datagram in the socket gets the event
 *      again at its next wait.
 *   3. A message on the read port of a context is reported alone.
 */

#include "nxt_main.h"
#include "nxt_unit.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#if (NXT_LINUX)
#include <sys/syscall.h>
#endif


#define NXT_WAKE_TEST_THREADS  4


typedef struct {
    pthread_t  thread;
    pid_t      tid;
    int        read[2];
    void       *ws;
    int        n;
    short      revents[2];
} nxt_wake_test_thread_t;


static int                     nxt_wake_test_failures;
static int                     nxt_wake_test_shared[2] = { -1, -1 };
static nxt_atomic_t            nxt_wake_test_ready;
static nxt_atomic_t            nxt_wake_test_woken;
static nxt_wake_test_thread_t  nxt_wake_test_threads[NXT_WAKE_TEST_THREADS];


static void
nxt_wake_test_assert(int cond, const char *name, long value)
{
    if (cond) {
        printf("wake test: %-50s passed (%ld)\n", name, value);

    } else {
        printf("wake test: %-50s FAILED (%ld)\n", name, value);
        nxt_wake_test_failures++;
    }
}


static void *
nxt_wake_test_waiter(void *data)
{
    nxt_wake_test_thread_t  *t;

    t = data;

#if (NXT_LINUX)
    t->tid = (pid_t) syscall(SYS_gettid);
#endif

    /* The first wait is like the first wait of a context; nothing is ready. */
    (void) nxt_unit_test_wait(t->ws, t->read[0], nxt_wake_test_shared[0], 0,
                              t->revents);

    (void) nxt_atomic_fetch_add(&nxt_wake_test_ready, 1);

    t->n = nxt_unit_test_wait(t->ws, t->read[0], nxt_wake_test_shared[0],
                              5000, t->revents);

    if (t->n > 0 && (t->revents[1] & POLLIN) != 0) {
        (void) nxt_atomic_fetch_add(&nxt_wake_test_woken, 1);
    }

    return NULL;
}


/*
 * A thread waits when its state is "S" (sleeping).  Without the state of a
 * thread, a thread that is ready is taken as one that waits.
 */

static int
nxt_wake_test_sleeping(pid_t tid)
{
#if (NXT_LINUX)
    int      fd;
    char     path[64], buf[256], *p;
    ssize_t  n;

    (void) snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int) tid);

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        return 0;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    (void) close(fd);

    if (n <= 0) {
        return 0;
    }

    buf[n] = '\0';

    p = strrchr(buf, ')');

    return (p != NULL && p[1] == ' ' && p[2] == 'S');

#else

    return 1;

#endif
}


static void
nxt_wake_test_settle(int ms)
{
    struct timespec  ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;

    (void) nanosleep(&ts, NULL);
}


static void
nxt_wake_test_ring(void)
{
    char  b;

    b = 'Q';

    if (send(nxt_wake_test_shared[1], &b, 1, 0) != 1) {
        perror("send");
        nxt_wake_test_failures++;
    }
}


/* Starts the threads, and returns when each one sleeps in its wait. */

static int
nxt_wake_test_start(void)
{
    int                     i, k, all;
    nxt_wake_test_thread_t  *t;

    nxt_atomic_store(&nxt_wake_test_ready, 0);
    nxt_atomic_store(&nxt_wake_test_woken, 0);

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        t = &nxt_wake_test_threads[i];

        t->ws = nxt_unit_test_wait_set_new();
        if (t->ws == NULL) {
            return -1;
        }

        if (pthread_create(&t->thread, NULL, nxt_wake_test_waiter, t) != 0) {
            perror("pthread_create");
            return -1;
        }
    }

    /* Wait until every thread sleeps in its wait. */

    all = 0;

    for (k = 0; k < 500; k++) {
        all = (nxt_atomic_load(&nxt_wake_test_ready) == NXT_WAKE_TEST_THREADS);

        for (i = 0; all && i < NXT_WAKE_TEST_THREADS; i++) {
            all = nxt_wake_test_sleeping(nxt_wake_test_threads[i].tid);
        }

        if (all) {
            break;
        }

        nxt_wake_test_settle(10);
    }

    nxt_wake_test_assert(all, "all threads wait", k);

    nxt_wake_test_settle(100);

    return 0;
}


static void
nxt_wake_test_join(void)
{
    int  i;

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        (void) pthread_join(nxt_wake_test_threads[i].thread, NULL);
    }
}


/* Counts and reads the datagrams in the shared socket. */

static int
nxt_wake_test_drain(void)
{
    int   k;
    char  b;

    k = 0;

    while (recv(nxt_wake_test_shared[0], &b, 1, MSG_DONTWAIT) == 1) {
        k++;
    }

    return k;
}


static void
nxt_wake_test_free_sets(void)
{
    int  i;

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        nxt_unit_test_wait_set_free(nxt_wake_test_threads[i].ws);
    }
}


/* 1. One datagram wakes every thread.  No thread reads it. */

static void
nxt_wake_test_all(const char *name)
{
    int  n;

    nxt_wake_test_ring();

    /*
     * Every wait ends after a bound.  A thread that the datagram woke has
     * returned by then, and one that it did not wake has timed out, so the
     * count does not depend on how fast the threads run.
     */
    nxt_wake_test_join();

    n = (int) nxt_atomic_load(&nxt_wake_test_woken);
    nxt_wake_test_assert(n == NXT_WAKE_TEST_THREADS, name, n);
}


/*
 * 2. Nobody read the "sent" datagrams.  The wait of a woken context reports
 * the shared port again, at once, and the context reads them all.
 */

static void
nxt_wake_test_unread(int sent)
{
    int                     n, k;
    short                   revents[2];
    nxt_wake_test_thread_t  *t;

    t = &nxt_wake_test_threads[0];

    n = nxt_unit_test_wait(t->ws, t->read[0], nxt_wake_test_shared[0], 0,
                           revents);
    nxt_wake_test_assert(n == 1 && (revents[1] & POLLIN) != 0,
                         "a woken context sees the unread datagram again", n);

    k = nxt_wake_test_drain();

    nxt_wake_test_assert(k == sent, "the datagrams were left in the socket",
                         k);

    n = nxt_unit_test_wait(t->ws, t->read[0], nxt_wake_test_shared[0], 0,
                           revents);
    nxt_wake_test_assert(n == 0, "the drained socket is not reported", n);
}


/* 3. The read port of a context is reported as poll() reports it. */

static int
nxt_wake_test_read_port(void)
{
    int                     n;
    char                    b;
    void                    *ws;
    short                   revents[2];
    nxt_wake_test_thread_t  *t;

    t = &nxt_wake_test_threads[0];

    ws = nxt_unit_test_wait_set_new();

    if (ws == NULL || send(t->read[1], "m", 1, 0) != 1) {
        return 1;
    }

    n = nxt_unit_test_wait(ws, t->read[0], nxt_wake_test_shared[0], 1000,
                           revents);
    nxt_wake_test_assert(n == 1 && revents[0] == POLLIN && revents[1] == 0,
                         "the read port is reported alone", n);

    nxt_unit_test_wait_set_free(ws);

    (void) recv(t->read[0], &b, 1, MSG_DONTWAIT);

    return 0;
}


int
main(void)
{
    int                     i;
    nxt_wake_test_thread_t  *t;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, nxt_wake_test_shared) == -1) {
        perror("socketpair");
        return 1;
    }

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        t = &nxt_wake_test_threads[i];

        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, t->read) == -1) {
            perror("socketpair");
            return 1;
        }
    }

    if (nxt_wake_test_start() != 0) {
        return 1;
    }

    nxt_wake_test_all("with poll(), one READ_QUEUE wakes every context");

    nxt_wake_test_unread(1);

    if (nxt_wake_test_read_port() != 0) {
        return 1;
    }

    nxt_wake_test_free_sets();

    printf("wake test: %s\n", nxt_wake_test_failures ? "FAILED" : "passed");

    return nxt_wake_test_failures ? 1 : 0;
}
