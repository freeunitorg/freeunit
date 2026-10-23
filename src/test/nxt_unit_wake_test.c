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
 *   1. One datagram wakes one thread.  With poll(), it woke all of them.
 *   2. Each further datagram wakes one more thread, so no request waits
 *      while a context is idle.
 *   3. A woken thread that left its datagram in the socket gets the event
 *      again at its next wait, and reads the datagram then.
 *   4. When the epoll set cannot be made on this system (EINVAL), the
 *      thread waits in poll(), and one datagram wakes all of them.  The
 *      thread does not try to make the set again.
 *   5. When the set was not made for an error that can go away (EMFILE),
 *      the next wait makes it, and one datagram wakes one thread again.
 *   6. After NXT_UNIT_EPOLL_REORDER wakes for the shared port, the next
 *      wait adds the port to the set again, and the set still reports it.
 *   7. With fewer CPUs than the minimum, the set is not made, and the
 *      thread waits in poll().
 *   8. The CPUs are the CPUs of the affinity mask, not the online CPUs: a
 *      process that runs on 2 CPUs of a larger system counts 2.
 *
 * The minimum of CPUs is set to 0 for 1-6, so that they run on any system.
 */

#include "nxt_main.h"
#include "nxt_unit.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>


#if (NXT_HAVE_EPOLL) && defined(EPOLLEXCLUSIVE)

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
    t->tid = (pid_t) syscall(SYS_gettid);

    /* The first wait makes the epoll set; nothing is ready yet. */
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


/*
 * Starts the threads, and returns when each one sleeps in its wait.  With
 * "err" not 0, the first wait does not make the epoll set, as if
 * epoll_create1() failed with errno "err", and it must wait as poll()
 * does.  The waits of the thread come after it.
 */

static int
nxt_wake_test_start(int err)
{
    int                     i, k, all, n, bad;
    short                   revents[2];
    nxt_wake_test_thread_t  *t;

    nxt_atomic_store(&nxt_wake_test_ready, 0);
    nxt_atomic_store(&nxt_wake_test_woken, 0);

    bad = 0;

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        t = &nxt_wake_test_threads[i];

        t->ws = nxt_unit_test_wait_set_new();
        if (t->ws == NULL) {
            return -1;
        }

        if (err != 0) {
            nxt_unit_test_wait_set_fail(err);

            /* Nothing is ready, so poll() returns 0. */
            n = nxt_unit_test_wait(t->ws, t->read[0], nxt_wake_test_shared[0],
                                   0, revents);
            if (n != 0) {
                bad = n;
            }
        }

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


/*
 * 1 and 2.  One datagram wakes one thread, and each further datagram wakes
 * one more.  No thread reads a datagram.
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


/*
 * 8. A child process runs on 2 CPUs of its affinity mask, and counts its
 * CPUs.  The count must be 2, not the number of online CPUs.  The case
 * needs 2 CPUs in the mask and more than 2 online CPUs.
 */

static void
nxt_wake_test_cpu_count(void)
{
#if (NXT_HAVE_LINUX_SCHED_GETAFFINITY)
    int        i, k, n, status;
    long       online;
    pid_t      pid, w;
    cpu_set_t  all, two;

    CPU_ZERO(&all);
    CPU_ZERO(&two);

    online = sysconf(_SC_NPROCESSORS_ONLN);

    if (sched_getaffinity(0, sizeof(all), &all) != 0
        || CPU_COUNT(&all) < 2 || online <= 2)
    {
        printf("wake test: the CPU count is skipped, %ld online CPUs\n",
               online);
        return;
    }

    k = 0;

    for (i = 0; i < CPU_SETSIZE && k < 2; i++) {
        if (CPU_ISSET(i, &all)) {
            CPU_SET(i, &two);
            k++;
        }
    }

    pid = fork();

    if (pid == 0) {
        if (sched_setaffinity(0, sizeof(two), &two) != 0) {
            _exit(255);
        }

        _exit(nxt_unit_test_cpu_count());
    }

    n = -1;

    if (pid > 0) {
        do {
            w = waitpid(pid, &status, 0);
        } while (w == -1 && errno == EINTR);

        if (w == pid && WIFEXITED(status)) {
            n = WEXITSTATUS(status);
        }
    }

    nxt_wake_test_assert(n == 2, "a process on 2 CPUs counts 2 CPUs", n);
#else
    printf("wake test: the CPU count is skipped, no sched_getaffinity()\n");
#endif
}


int
main(void)
{
    int                     i, k, epoll, n;
    char                    b;
    void                    *ws;
    short                   revents[2];
    nxt_wake_test_thread_t  *t;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, nxt_wake_test_shared) == -1) {
        perror("socketpair");
        return 1;
    }

    nxt_unit_test_wait_set_min_cpus(0);

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        t = &nxt_wake_test_threads[i];

        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, t->read) == -1) {
            perror("socketpair");
            return 1;
        }
    }

    if (nxt_wake_test_start(0) != 0) {
        return 1;
    }

    epoll = 1;

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        epoll = epoll && nxt_wake_test_threads[i].epoll;
    }

    nxt_wake_test_assert(epoll, "every context uses its epoll set", epoll);

    nxt_wake_test_one_each("one READ_QUEUE wakes one context",
                           "each READ_QUEUE wakes one more context");

    /*
     * 3. Nobody read the datagrams.  The set of a woken context reports
     * the shared port again, at once, and the context reads them all.
     */

    t = &nxt_wake_test_threads[0];

    n = nxt_unit_test_wait(t->ws, t->read[0], nxt_wake_test_shared[0], 0,
                           revents);
    nxt_wake_test_assert(n == 1 && (revents[1] & POLLIN) != 0,
                         "a woken context sees the unread datagram again", n);

    k = nxt_wake_test_drain();

    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "the datagrams were left in the socket", k);

    n = nxt_unit_test_wait(t->ws, t->read[0], nxt_wake_test_shared[0], 0,
                           revents);
    nxt_wake_test_assert(n == 0, "the drained socket is not reported", n);

    /* The read port of a context is reported as poll() reports it. */

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

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        nxt_unit_test_wait_set_free(nxt_wake_test_threads[i].ws);
    }

    /*
     * 4. The epoll sets cannot be made on this system.  Each context waits
     * in poll(), as before, and one datagram wakes all of them.  No thread
     * reads it.  The waits of the threads do not try to make the set again:
     * no failure is armed for them, and a try would make it.
     */

    if (nxt_wake_test_start(EINVAL) != 0) {
        return 1;
    }

    epoll = 0;

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        epoll = epoll || nxt_wake_test_threads[i].epoll;
    }

    nxt_wake_test_assert(!epoll, "after EINVAL, no context uses an epoll set",
                         epoll);

    nxt_wake_test_settle(100);

    nxt_wake_test_ring();
    nxt_wake_test_settle(300);

    n = (int) nxt_atomic_load(&nxt_wake_test_woken);
    nxt_wake_test_assert(n == NXT_WAKE_TEST_THREADS,
                         "with poll(), one READ_QUEUE wakes every context", n);

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        (void) pthread_join(nxt_wake_test_threads[i].thread, NULL);
    }

    for (i = 1; i < NXT_WAKE_TEST_THREADS; i++) {
        nxt_wake_test_ring();
    }

    k = nxt_wake_test_drain();
    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "no context read a datagram", k);

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        nxt_unit_test_wait_set_free(nxt_wake_test_threads[i].ws);
    }

    /*
     * 5. The first wait of each context does not make the set, for EMFILE.
     * That limit can go away, so the next wait makes the set, and one
     * datagram wakes one context again.
     */

    if (nxt_wake_test_start(EMFILE) != 0) {
        return 1;
    }

    epoll = 1;

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        epoll = epoll && nxt_wake_test_threads[i].epoll;
    }

    nxt_wake_test_assert(epoll, "after EMFILE, the next wait makes the set",
                         epoll);

    nxt_wake_test_one_each("after EMFILE, one READ_QUEUE wakes one context",
                           "after EMFILE, each READ_QUEUE wakes one more");

    k = nxt_wake_test_drain();
    nxt_wake_test_assert(k == NXT_WAKE_TEST_THREADS,
                         "after EMFILE, no context read a datagram", k);

    for (i = 0; i < NXT_WAKE_TEST_THREADS; i++) {
        nxt_unit_test_wait_set_free(nxt_wake_test_threads[i].ws);
    }

    /*
     * 6. A context that the shared port woke 16 times adds the port to its
     * set again at the next wait.  The set is still used, and it reports
     * the datagram that waits in the socket.
     */

    ws = nxt_unit_test_wait_set_new();
    if (ws == NULL) {
        return 1;
    }

    t = &nxt_wake_test_threads[0];

    nxt_wake_test_ring();

    for (i = 0; i < 16; i++) {
        n = nxt_unit_test_wait(ws, t->read[0], nxt_wake_test_shared[0], 0,
                               revents);
        if (n != 1 || (revents[1] & POLLIN) == 0) {
            break;
        }
    }

    nxt_wake_test_assert(i == 16 && nxt_unit_test_wait_set_shared_wakes(ws)
                         == 16, "the shared port woke the context 16 times", i);

    n = nxt_unit_test_wait(ws, t->read[0], nxt_wake_test_shared[0], 0,
                           revents);

    nxt_wake_test_assert(n == 1 && (revents[1] & POLLIN) != 0
                         && nxt_unit_test_wait_set_epoll(ws)
                         && nxt_unit_test_wait_set_shared_wakes(ws) == 1,
                         "after the reorder, the set reports the port", n);

    k = nxt_wake_test_drain();
    nxt_wake_test_assert(k == 1, "the reorder read no datagram", k);

    nxt_unit_test_wait_set_free(ws);

    /*
     * 7. With fewer online CPUs than the minimum, the wait does not make
     * the set, and it reports the shared port as poll() does.
     */

    nxt_unit_test_wait_set_min_cpus(INT_MAX);

    ws = nxt_unit_test_wait_set_new();
    if (ws == NULL) {
        return 1;
    }

    nxt_wake_test_ring();

    n = nxt_unit_test_wait(ws, t->read[0], nxt_wake_test_shared[0], 0,
                           revents);

    nxt_wake_test_assert(n == 1 && (revents[1] & POLLIN) != 0
                         && !nxt_unit_test_wait_set_epoll(ws),
                         "with few CPUs, the context waits in poll()", n);

    (void) nxt_wake_test_drain();

    nxt_unit_test_wait_set_free(ws);

    nxt_wake_test_cpu_count();

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
