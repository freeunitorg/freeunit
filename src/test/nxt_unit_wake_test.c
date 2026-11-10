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
 *      in the socket.  The wait uses poll() on systems without
 *      EPOLLEXCLUSIVE, on kernel releases that do not use a set (7), and
 *      when the epoll set cannot be made (EINVAL, 4).
 *   2. A woken thread that left the datagram in the socket gets the event
 *      again at its next wait.
 *   3. A message on the read port of a context is reported alone.
 *   4. When the epoll set cannot be made on this system (EINVAL), the
 *      thread waits in poll(), and it does not try to make the set again.
 *   5. With an epoll set, one datagram wakes one thread, and each further
 *      datagram wakes one more thread, so no request waits while a context
 *      is idle.
 *   6. When the set was not made for an error that can go away (EMFILE),
 *      the next wait makes it, and one datagram wakes one thread again.
 *   7. The kernel releases that use a set are 5.7 and later, 5.6.13 and
 *      later, and 5.4.41 and later.  On other releases, each thread waits
 *      in poll().
 *   8. Two datagrams sent back to back wake two threads, or all of them
 *      with poll().  Linux before 5.7 sends the second one to the set of
 *      the first woken thread, if that thread did not run yet.
 *   9. When the process uses half of its RLIMIT_NOFILE or more, the set is
 *      not kept, and the thread does not try again.
 *  10. When the application can have fewer than 4 processes, each thread
 *      waits in poll().  With 4, or when the number is not known (0), each
 *      thread uses a set.
 *  11. NXT_UNIT_INIT gives the number of processes.
 */

#include "nxt_main.h"
#include "nxt_unit.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/utsname.h>

#if (NXT_LINUX)
#include <sys/syscall.h>
#endif


#define NXT_WAKE_TEST_THREADS  4


typedef struct {
    pthread_t  thread;
    pid_t      tid;
    int        read[2];
    void       *ws;
    int        epoll;
    int        n;
    short      revents[2];
} nxt_wake_test_thread_t;


static int                     nxt_wake_test_failures;
static uint32_t                nxt_wake_test_processes;
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

    t->epoll = nxt_unit_test_wait_set_epoll(t->ws);

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


/*
 * Starts the threads, and returns when each one sleeps in its wait.  With
 * "err" not 0, the first wait does not make the epoll set, as if
 * epoll_create1() failed with errno "err", and it must wait as poll()
 * does.  The waits of the thread come after it.
 */

static int
nxt_wake_test_start(int err)
{
    int                     i, k, all, bad;
    nxt_wake_test_thread_t  *t;

    nxt_atomic_store(&nxt_wake_test_ready, 0);
    nxt_atomic_store(&nxt_wake_test_woken, 0);

    bad = 0;

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        t = &nxt_wake_test_threads[i];

        t->ws = nxt_unit_test_wait_set_new(nxt_wake_test_processes);
        if (t->ws == NULL) {
            return -1;
        }

#if (NXT_HAVE_EPOLL) && defined(EPOLLEXCLUSIVE)
        if (err != 0) {
            int    n;
            short  revents[2];

            nxt_unit_test_wait_set_failures(1, err);

            /* Nothing is ready, so poll() returns 0. */
            n = nxt_unit_test_wait(t->ws, t->read[0], nxt_wake_test_shared[0],
                                   0, revents);
            if (n != 0) {
                bad = n;
            }
        }
#endif

        if (pthread_create(&t->thread, NULL, nxt_wake_test_waiter, t) != 0) {
            perror("pthread_create");
            return -1;
        }
    }

    if (err != 0) {
        nxt_wake_test_assert(bad == 0,
                             "a failed epoll set waits as poll() does", bad);
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


/* The number of threads that use an epoll set. */

static int
nxt_wake_test_epoll_sets(void)
{
    int  i, k;

    k = 0;

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        k += (nxt_wake_test_threads[i].epoll != 0);
    }

    return k;
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

    ws = nxt_unit_test_wait_set_new(0);

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


/*
 * 1, 2 and 3 when each context waits in poll().  "err" is 0 when poll() is
 * the wait of this system, or the errno of the epoll set that is not made
 * (4).
 */

static int
nxt_wake_test_poll(int err)
{
    int  k;

    if (nxt_wake_test_start(err) != 0) {
        return 1;
    }

    k = nxt_wake_test_epoll_sets();
    nxt_wake_test_assert(k == 0, (err != 0) ? "after EINVAL, no context uses "
                                              "an epoll set"
                                            : "no context uses an epoll set",
                         k);

    nxt_wake_test_all("with poll(), one READ_QUEUE wakes every context");

    nxt_wake_test_unread(1);

    if (nxt_wake_test_read_port() != 0) {
        return 1;
    }

    nxt_wake_test_free_sets();

    return 0;
}


/*
 * 8. Two datagrams back to back wake "expect" threads.  Often, the first
 * woken thread does not run before the second send.  When the threads use
 * epoll sets on Linux 4.15 or 5.4.0 in a VM, the two datagrams woke one
 * thread in 11 of 12 runs.
 */

static int
nxt_wake_test_back_to_back(int expect)
{
    int  i, n, k;

    if (nxt_wake_test_start(0) != 0) {
        return 1;
    }

    nxt_wake_test_ring();
    nxt_wake_test_ring();

    nxt_wake_test_settle(300);

    n = (int) nxt_atomic_load(&nxt_wake_test_woken);
    nxt_wake_test_assert(n == expect,
                         "two READ_QUEUE back to back wake two or more", n);

    /* Wake the others. */

    for (i = 2; i < NXT_WAKE_TEST_THREADS; i++) {
        nxt_wake_test_ring();
    }

    nxt_wake_test_join();

    k = nxt_wake_test_drain();
    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "back to back, no context read a datagram", k);

    nxt_wake_test_free_sets();

    return 0;
}


#if (NXT_HAVE_EPOLL) && defined(EPOLLEXCLUSIVE)

/*
 * 5. One datagram wakes one thread, and each further datagram wakes one
 * more.  No thread reads a datagram.
 */

static void
nxt_wake_test_one_each(const char *one, const char *each)
{
    int  i, n;

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


/*
 * 7. The kernel releases that use a set, and the release that the gate of
 * this process reads.
 */

static void
nxt_wake_test_releases(void)
{
    int             failures, gate;
    nxt_uint_t      i;
    struct utsname  u;

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

    nxt_wake_test_assert(failures == 0, "kernel releases that use a set",
                         (long) i);

    if (uname(&u) != 0) {
        perror("uname");
        nxt_wake_test_failures++;
        return;
    }

    gate = nxt_unit_test_epoll_release(NULL);

    printf("wake test: kernel \"%s\", epoll sets %s\n", u.release,
           gate ? "used" : "not used");

    nxt_wake_test_assert(gate == nxt_unit_test_epoll_release(u.release),
                         "the gate reads the release of this kernel", gate);
}


/*
 * 9. The set is not kept when its descriptor is at half of the soft
 * RLIMIT_NOFILE or above.  Next to that limit, it is kept.
 */

static int
nxt_wake_test_crowded(int read_fd)
{
    int            fd, n, epoll;
    void           *ws;
    short          revents[2];
    struct rlimit  rlim, low;

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        perror("getrlimit");
        return 1;
    }

    /* The lowest free descriptor: the set takes it. */

    fd = fcntl(read_fd, F_DUPFD, 0);
    if (fd == -1) {
        perror("fcntl");
        return 1;
    }

    (void) close(fd);

    low = rlim;
    low.rlim_cur = 2 * (rlim_t) fd;

    ws = nxt_unit_test_wait_set_new(0);

    if (ws == NULL || setrlimit(RLIMIT_NOFILE, &low) != 0) {
        perror("setrlimit");
        return 1;
    }

    n = nxt_unit_test_wait(ws, read_fd, nxt_wake_test_shared[0], 0, revents);
    epoll = nxt_unit_test_wait_set_epoll(ws);

    (void) setrlimit(RLIMIT_NOFILE, &rlim);

    nxt_wake_test_assert(n == 0 && !epoll,
                         "at half of RLIMIT_NOFILE, the set is not kept", fd);

    (void) nxt_unit_test_wait(ws, read_fd, nxt_wake_test_shared[0], 0,
                              revents);

    nxt_wake_test_assert(!nxt_unit_test_wait_set_epoll(ws),
                         "then the context does not try again", fd);

    nxt_unit_test_wait_set_free(ws);

    low.rlim_cur = 2 * (rlim_t) fd + 2;

    ws = nxt_unit_test_wait_set_new(0);

    if (ws == NULL || setrlimit(RLIMIT_NOFILE, &low) != 0) {
        perror("setrlimit");
        return 1;
    }

    n = nxt_unit_test_wait(ws, read_fd, nxt_wake_test_shared[0], 0, revents);
    epoll = nxt_unit_test_wait_set_epoll(ws);

    (void) setrlimit(RLIMIT_NOFILE, &rlim);

    nxt_wake_test_assert(n == 0 && epoll,
                         "below half of RLIMIT_NOFILE, the set is kept", fd);

    nxt_unit_test_wait_set_free(ws);

    return 0;
}


/*
 * 10. With 2 processes, no thread uses a set, and one datagram wakes every
 * thread.  With 4, one datagram wakes one thread.  0 (not known) is case 5.
 */

static int
nxt_wake_test_processes_gate(void)
{
    int  k;

    nxt_wake_test_processes = 2;

    if (nxt_wake_test_start(0) != 0) {
        return 1;
    }

    k = nxt_wake_test_epoll_sets();
    nxt_wake_test_assert(k == 0, "with 2 processes, no context uses a set", k);

    nxt_wake_test_all("with 2 processes, one READ_QUEUE wakes every context");

    k = nxt_wake_test_drain();
    nxt_wake_test_assert(k == 1, "with 2 processes, the datagram stays", k);

    nxt_wake_test_free_sets();

    nxt_wake_test_processes = 4;

    if (nxt_wake_test_start(0) != 0) {
        return 1;
    }

    k = nxt_wake_test_epoll_sets();
    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "with 4 processes, every context uses its set", k);

    nxt_wake_test_one_each("with 4 processes, one READ_QUEUE wakes one",
                           "with 4 processes, each READ_QUEUE wakes one more");

    k = nxt_wake_test_drain();
    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "with 4 processes, no context read a datagram", k);

    nxt_wake_test_free_sets();

    nxt_wake_test_processes = 0;

    return 0;
}


/* 4, 5 and 6, then 8 and 9, on a kernel that uses epoll sets. */

static int
nxt_wake_test_epoll(void)
{
    int  k;

    /*
     * 4. The epoll sets cannot be made on this system.  Each context waits
     * in poll(), as before.  The waits of the threads do not try to make
     * the set again: no failure is armed for them, and a try would make it.
     */

    if (nxt_wake_test_poll(EINVAL) != 0) {
        return 1;
    }

    /* 5. */

    if (nxt_wake_test_start(0) != 0) {
        return 1;
    }

    k = nxt_wake_test_epoll_sets();
    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "every context uses its epoll set", k);

    nxt_wake_test_one_each("one READ_QUEUE wakes one context",
                           "each READ_QUEUE wakes one more context");

    nxt_wake_test_unread(NXT_WAKE_TEST_THREADS);

    nxt_wake_test_free_sets();

    /*
     * 6. The first wait of each context does not make the set, for EMFILE.
     * That limit can go away, so the next wait makes the set, and one
     * datagram wakes one context again.
     */

    if (nxt_wake_test_start(EMFILE) != 0) {
        return 1;
    }

    k = nxt_wake_test_epoll_sets();
    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "after EMFILE, the next wait makes the set", k);

    nxt_wake_test_one_each("after EMFILE, one READ_QUEUE wakes one context",
                           "after EMFILE, each READ_QUEUE wakes one more");

    k = nxt_wake_test_drain();
    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "after EMFILE, no context read a datagram", k);

    nxt_wake_test_free_sets();

    if (nxt_wake_test_back_to_back(2) != 0
        || nxt_wake_test_crowded(nxt_wake_test_threads[0].read[0]) != 0
        || nxt_wake_test_processes_gate() != 0)
    {
        return 1;
    }

    return 0;
}

#endif


/*
 * 11. The number of processes is the last field of NXT_UNIT_INIT
 * (src/nxt_external.c).  A variable without it is not valid.
 */

static void
nxt_wake_test_env(void)
{
    int64_t  n;

    static const char  with[] = NXT_VERSION ";1;1,0,-1;1,0,-1;1,0,-1,-1;"
                                "-1,-1;2,1048576,0,6";
    static const char  without[] = NXT_VERSION ";1;1,0,-1;1,0,-1;1,0,-1,-1;"
                                   "-1,-1;2,1048576,0,";

    if (setenv(NXT_UNIT_INIT_ENV, with, 1) != 0) {
        perror("setenv");
        nxt_wake_test_failures++;
        return;
    }

    n = nxt_unit_test_read_env_processes();
    nxt_wake_test_assert(n == 6, "NXT_UNIT_INIT gives 6 processes", (long) n);

    if (setenv(NXT_UNIT_INIT_ENV, without, 1) != 0) {
        perror("setenv");
        nxt_wake_test_failures++;
        return;
    }

    n = nxt_unit_test_read_env_processes();
    nxt_wake_test_assert(n == -1, "NXT_UNIT_INIT without processes fails",
                         (long) n);

    (void) unsetenv(NXT_UNIT_INIT_ENV);
}


static int
nxt_wake_test_end(int rc)
{
    if (rc != 0) {
        return 1;
    }

    nxt_wake_test_env();

    printf("wake test: %s\n", nxt_wake_test_failures ? "FAILED" : "passed");

    return nxt_wake_test_failures ? 1 : 0;
}


int
main(void)
{
    int                     i, rc;
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

#if (NXT_HAVE_EPOLL) && defined(EPOLLEXCLUSIVE)
    nxt_wake_test_releases();

    if (nxt_unit_test_epoll_release(NULL)) {
        return nxt_wake_test_end(nxt_wake_test_epoll());
    }
#else
    printf("wake test: no EPOLLEXCLUSIVE, the epoll cases are skipped\n");
#endif

    /* 1, 2, 3 and 8 with poll(). */

    rc = nxt_wake_test_poll(0);

    if (rc == 0) {
        rc = nxt_wake_test_back_to_back(NXT_WAKE_TEST_THREADS);
    }

    return nxt_wake_test_end(rc);
}
