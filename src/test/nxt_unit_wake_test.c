/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * How many idle contexts one READ_QUEUE datagram on the shared port wakes
 * (nxt_unit_wait() in src/nxt_unit.c).
 *
 * Each of NXT_WAKE_TEST_THREADS threads plays an idle context: it has a
 * read port of its own, and it waits on that port and on one shared port,
 * as nxt_unit_read_buf() does.  A woken thread does not read the datagram,
 * as a context that took the request from the queue does not.
 *
 *   1. The release check accepts Linux 5.7 and later, 5.6.13 and later,
 *      and 5.4.41 and later.
 *   2. On a release that the check refuses, each thread waits in poll(),
 *      and one datagram wakes all of them.  The thread does not try to make
 *      the epoll set again.
 *   3. One datagram wakes one thread.  Each further datagram wakes one more
 *      thread, so no request waits while a context is idle.
 *   4. A woken thread that left its datagram in the socket gets the event
 *      again at its next wait, and reads the datagram then.
 *   5. When the set was not made for an error that can go away (EMFILE),
 *      the next wait makes it, and one datagram wakes one thread again.
 *   6. When the descriptor of the set is at half of the soft RLIMIT_NOFILE
 *      or above, the set is not kept, and the thread does not try again.
 *
 * Cases 3 to 6 run only on a kernel release that the check accepts.
 */

#include "nxt_main.h"
#include "nxt_unit.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>


#if (NXT_HAVE_EPOLL) && defined(EPOLLEXCLUSIVE)

#define NXT_WAKE_TEST_THREADS  4

/* How the first wait of each thread fails, for nxt_wake_test_start(). */
#define NXT_WAKE_TEST_OLD_KERNEL  1
#define NXT_WAKE_TEST_EMFILE      2


typedef struct {
    pthread_t  thread;
    pid_t      tid;
    int        read[2];
    int        ep_fd;
    int        n;
    short      revents[2];
} nxt_wake_test_thread_t;


static int                     nxt_wake_test_failures;
static int                     nxt_wake_test_shared[2] = { -1, -1 };
static struct utsname          nxt_wake_test_uname;
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
    t->tid = (pid_t) syscall(SYS_gettid);

    (void) nxt_atomic_fetch_add(&nxt_wake_test_ready, 1);

    t->n = nxt_unit_test_wait(&t->ep_fd, t->read[0], nxt_wake_test_shared[0],
                              5000, t->revents);

    if (t->n > 0 && (t->revents[1] & POLLIN) != 0) {
        (void) nxt_atomic_fetch_add(&nxt_wake_test_woken, 1);
    }

    return NULL;
}


/* A thread waits when its state is "S" (sleeping). */

static int
nxt_wake_test_sleeping(pid_t tid)
{
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


/* The descriptor that the next epoll_create1() gets. */

static int
nxt_wake_test_next_fd(void)
{
    int  fd;

    fd = dup(nxt_wake_test_shared[0]);
    if (fd != -1) {
        (void) close(fd);
    }

    return fd;
}


/* Sets the soft RLIMIT_NOFILE to "cur", and the hard one to "saved". */

static int
nxt_wake_test_nofile(rlim_t cur, const struct rlimit *saved)
{
    struct rlimit  rlim;

    rlim = *saved;
    rlim.rlim_cur = cur;

    if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        perror("setrlimit");
        return -1;
    }

    return 0;
}


/*
 * Starts the threads, and returns when each one sleeps in its wait.  The
 * first wait of each thread runs here, with a timeout of 0, and makes the
 * epoll set.  With NXT_WAKE_TEST_OLD_KERNEL or NXT_WAKE_TEST_EMFILE, that
 * wait does not make the set, and it must wait as poll() does.  The cause
 * is gone when the threads start.
 */

static int
nxt_wake_test_start(int mode)
{
    int                     i, k, all, n, bad;
    short                   revents[2];
    struct rlimit           rlim;
    nxt_wake_test_thread_t  *t;

    nxt_atomic_store(&nxt_wake_test_ready, 0);
    nxt_atomic_store(&nxt_wake_test_woken, 0);

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        perror("getrlimit");
        return -1;
    }

    if (mode == NXT_WAKE_TEST_OLD_KERNEL) {
        (void) nxt_unit_test_epoll_release("5.4.40");
    }

    /* No descriptor is free. */

    if (mode == NXT_WAKE_TEST_EMFILE
        && nxt_wake_test_nofile((rlim_t) nxt_wake_test_next_fd(), &rlim) != 0)
    {
        return -1;
    }

    bad = 0;

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        t = &nxt_wake_test_threads[i];

        t->ep_fd = -1;

        /* Nothing is ready, so the wait returns 0. */
        n = nxt_unit_test_wait(&t->ep_fd, t->read[0], nxt_wake_test_shared[0],
                               0, revents);
        if (n != 0) {
            bad = n;
        }
    }

    (void) nxt_unit_test_epoll_release(nxt_wake_test_uname.release);

    if (mode == NXT_WAKE_TEST_EMFILE
        && nxt_wake_test_nofile(rlim.rlim_cur, &rlim) != 0)
    {
        return -1;
    }

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        t = &nxt_wake_test_threads[i];

        if (pthread_create(&t->thread, NULL, nxt_wake_test_waiter, t) != 0) {
            perror("pthread_create");
            return -1;
        }
    }

    nxt_wake_test_assert(bad == 0, "the first wait returns as poll() does",
                         bad);

    /* Wait until every thread sleeps in its wait. */

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


/* Returns the number of threads with an epoll set, and closes the sets. */

static int
nxt_wake_test_sets(int close_them)
{
    int  i, k;

    k = 0;

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        if (nxt_wake_test_threads[i].ep_fd >= 0) {
            k++;

            if (close_them) {
                (void) close(nxt_wake_test_threads[i].ep_fd);
                nxt_wake_test_threads[i].ep_fd = -1;
            }
        }
    }

    return k;
}


/*
 * 3.  One datagram wakes one thread, and each further datagram wakes one
 * more.  No thread reads a datagram.
 */

static void
nxt_wake_test_one_each(const char *one, const char *each)
{
    int  i, n;

    nxt_wake_test_settle(100);

    nxt_wake_test_ring();
    nxt_wake_test_settle(300);

    n = (int) nxt_atomic_load(&nxt_wake_test_woken);
    nxt_wake_test_assert(n == 1, one, n);

    for (i = 1; i < NXT_WAKE_TEST_THREADS; i++) {
        nxt_wake_test_ring();
        nxt_wake_test_settle(100);
    }

    nxt_wake_test_settle(200);

    n = (int) nxt_atomic_load(&nxt_wake_test_woken);
    nxt_wake_test_assert(n == NXT_WAKE_TEST_THREADS, each, n);

    nxt_wake_test_join();
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


/* 1.  The release check. */

static void
nxt_wake_test_releases(void)
{
    int         failures;
    nxt_uint_t  i;

    static const struct {
        const char  *release;
        int         epoll;
    } tests[] = {
        { "5.7", 1 },
        { "5.7.0", 1 },
        { "5.6.12", 0 },
        { "5.6.13", 1 },
        { "5.5.19", 0 },
        { "5.4.40", 0 },
        { "5.4.41", 1 },
        { "5.4.41-rt24", 1 },
        { "5.4.0-150-generic", 0 },
        { "4.19.325", 0 },
        { "4.18.0-553.el8_10.x86_64", 0 },
        { "5.10.0-35-amd64", 1 },
        { "5.15.0-91-generic", 1 },
        { "6.1.0", 1 },
        { "10.0.1", 1 },
        { "6", 0 },
        { "6.", 0 },
        { "", 0 },
        { "x.1", 0 },
        { "99999999999.1", 0 },
    };

    failures = 0;

    for (i = 0; i < nxt_nitems(tests); i++) {
        if (nxt_unit_test_epoll_release(tests[i].release) != tests[i].epoll) {
            printf("wake test: release \"%s\" FAILED (expected %d)\n",
                   tests[i].release, tests[i].epoll);
            failures++;
        }
    }

    nxt_wake_test_assert(failures == 0, "the release check", failures);
}


/*
 * 6.  The set is not kept when its descriptor is at half of the soft
 * RLIMIT_NOFILE or above, and the next wait does not try again.  Below that
 * limit, the set is kept.
 */

static int
nxt_wake_test_crowded(int read_fd)
{
    int            fd, n, ep_fd;
    short          revents[2];
    struct rlimit  rlim;

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        perror("getrlimit");
        return -1;
    }

    fd = nxt_wake_test_next_fd();
    ep_fd = -1;

    if (nxt_wake_test_nofile(2 * (rlim_t) fd, &rlim) != 0) {
        return -1;
    }

    n = nxt_unit_test_wait(&ep_fd, read_fd, nxt_wake_test_shared[0], 0,
                           revents);

    if (nxt_wake_test_nofile(rlim.rlim_cur, &rlim) != 0) {
        return -1;
    }

    nxt_wake_test_assert(n == 0 && ep_fd < 0,
                         "at half of RLIMIT_NOFILE, the set is not kept", fd);

    (void) nxt_unit_test_wait(&ep_fd, read_fd, nxt_wake_test_shared[0], 0,
                              revents);

    nxt_wake_test_assert(ep_fd < 0, "then the context does not try again",
                         ep_fd);

    ep_fd = -1;

    if (nxt_wake_test_nofile(2 * (rlim_t) fd + 2, &rlim) != 0) {
        return -1;
    }

    n = nxt_unit_test_wait(&ep_fd, read_fd, nxt_wake_test_shared[0], 0,
                           revents);

    if (nxt_wake_test_nofile(rlim.rlim_cur, &rlim) != 0) {
        return -1;
    }

    nxt_wake_test_assert(n == 0 && ep_fd == fd,
                         "below half of RLIMIT_NOFILE, the set is kept", ep_fd);

    if (ep_fd >= 0) {
        (void) close(ep_fd);
    }

    return 0;
}


int
main(void)
{
    int                     i, k, n, ep_fd;
    char                    b;
    short                   revents[2];
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

    if (uname(&nxt_wake_test_uname) != 0) {
        perror("uname");
        return 1;
    }

    nxt_wake_test_releases();

    /*
     * 2.  On a refused release, one datagram wakes every thread.  The waits
     * in the threads run with the release of this kernel.  If the context
     * tried to make the set again there, a kernel that the check accepts
     * would give it the set.
     */

    if (nxt_wake_test_start(NXT_WAKE_TEST_OLD_KERNEL) != 0) {
        return 1;
    }

    nxt_wake_test_settle(100);

    nxt_wake_test_ring();
    nxt_wake_test_settle(300);

    n = (int) nxt_atomic_load(&nxt_wake_test_woken);
    nxt_wake_test_assert(n == NXT_WAKE_TEST_THREADS,
                         "on a refused release, one READ_QUEUE wakes all", n);

    nxt_wake_test_join();

    n = nxt_wake_test_sets(1);
    nxt_wake_test_assert(n == 0, "on a refused release, no context makes a set",
                         n);

    k = nxt_wake_test_drain();
    nxt_wake_test_assert(k == 1, "no context read the datagram", k);

    if (!nxt_unit_test_epoll_release(nxt_wake_test_uname.release)) {
        printf("wake test: the check refuses Linux %s, cases 3 to 6 skipped\n",
               nxt_wake_test_uname.release);
        goto done;
    }

    /* 3. */

    if (nxt_wake_test_start(0) != 0) {
        return 1;
    }

    n = nxt_wake_test_sets(0);
    nxt_wake_test_assert(n == NXT_WAKE_TEST_THREADS,
                         "every context uses its epoll set", n);

    nxt_wake_test_one_each("one READ_QUEUE wakes one context",
                           "each READ_QUEUE wakes one more context");

    /*
     * 4.  Nobody read the datagrams.  The set of a woken context reports
     * the shared port again, at once, and the context reads them all.
     */

    t = &nxt_wake_test_threads[0];

    n = nxt_unit_test_wait(&t->ep_fd, t->read[0], nxt_wake_test_shared[0], 0,
                           revents);
    nxt_wake_test_assert(n == 1 && (revents[1] & POLLIN) != 0,
                         "a woken context sees the unread datagram again", n);

    k = nxt_wake_test_drain();
    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "the datagrams were left in the socket", k);

    n = nxt_unit_test_wait(&t->ep_fd, t->read[0], nxt_wake_test_shared[0], 0,
                           revents);
    nxt_wake_test_assert(n == 0, "the drained socket is not reported", n);

    (void) nxt_wake_test_sets(1);

    /* The read port of a context is reported as poll() reports it. */

    if (send(t->read[1], "m", 1, 0) != 1) {
        perror("send");
        return 1;
    }

    ep_fd = -1;

    n = nxt_unit_test_wait(&ep_fd, t->read[0], nxt_wake_test_shared[0], 1000,
                           revents);
    nxt_wake_test_assert(n == 1 && revents[0] == POLLIN && revents[1] == 0,
                         "the read port is reported alone", n);

    if (ep_fd >= 0) {
        (void) close(ep_fd);
    }

    (void) recv(t->read[0], &b, 1, MSG_DONTWAIT);

    /*
     * 5.  The first wait of each context does not make the set, for EMFILE.
     * That limit can go away, so the next wait makes the set, and one
     * datagram wakes one context again.
     */

    if (nxt_wake_test_start(NXT_WAKE_TEST_EMFILE) != 0) {
        return 1;
    }

    nxt_wake_test_one_each("after EMFILE, one READ_QUEUE wakes one context",
                           "after EMFILE, each READ_QUEUE wakes one more");

    n = nxt_wake_test_sets(1);
    nxt_wake_test_assert(n == NXT_WAKE_TEST_THREADS,
                         "after EMFILE, the next wait makes the set", n);

    k = nxt_wake_test_drain();
    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "after EMFILE, no context read a datagram", k);

    /* 6. */

    if (nxt_wake_test_crowded(t->read[0]) != 0) {
        return 1;
    }

done:

    printf("wake test: %s\n", nxt_wake_test_failures ? "FAILED" : "passed");

    return nxt_wake_test_failures ? 1 : 0;
}

#else

int
main(void)
{
    printf("wake test: skipped, no EPOLLEXCLUSIVE\n");

    return 0;
}

#endif
