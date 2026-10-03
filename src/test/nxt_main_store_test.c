/*
 * Copyright (C) F5, Inc.
 */

/*
 * Tests of the state store child of main (src/nxt_main_process.c,
 * https://github.com/freeunitorg/freeunit/issues/516).
 *
 * The test drives the scheduler directly.  An NXT_TESTS hook makes each
 * store child sleep before it stores, so the test can act while a child
 * runs.  The test reaps each child with waitpid() and gives the status to
 * the handler that the SIGCHLD handler of main calls.
 *
 * Cases:
 *
 *   - While main runs, a newer store waits for the running store child,
 *     which finishes its store.
 *   - At exit, main stops a running store child with SIGKILL when a newer
 *     store waits: when the exit starts, and when a store comes during
 *     the exit.  The killed child is not logged as a failed store, it does
 *     not set nxt_conf_ver, and the newer store runs when the child is
 *     reaped.
 *   - A child that exited before the signal arrived keeps its exit code:
 *     its store counts, and nxt_conf_ver is set.
 *   - The store child closes the descriptors it inherits from 3 upwards
 *     before it stores.  The test holds the read end of a pipe and gives
 *     the write end to the child only.  The read end must see the end of
 *     file while the child still sleeps.
 *   - Stores start in the order they came.  A conf.json store that
 *     replaces a pending one takes its place.  So a DELETE never runs
 *     before an older conf.json store.
 *   - Certificate bundles (with TLS).  nxt_cert_store_put_handler() gives
 *     the bundle to a store child and answers the controller only when the
 *     child exits: nothing is on the reply port while the child runs.  Two
 *     uploads in quick succession both land, each gets its own answer, in
 *     order.  A conf.json store that arrives between them runs between
 *     them, and it does not kill the certificate child, even when main
 *     exits.  A DELETE that follows a PUT of the same bundle runs after
 *     it, so the bundle is gone at the end.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_port.h>
#include <nxt_port_hash.h>
#include <nxt_main_process.h>
#include <nxt_event_engine.h>
#if (NXT_TLS)
#include <nxt_cert.h>
#endif
#include "nxt_tests.h"

#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>


#define NXT_MAIN_STORE_TEST_DELAY  500


#define nxt_main_store_test_fail(thr, ...)                                    \
    do {                                                                      \
        nxt_log_alert((thr)->log, "main store test: " __VA_ARGS__);           \
        goto done;                                                            \
    } while (0)


typedef struct {
    char  dir[NXT_MAX_PATH_LEN];
    char  conf[NXT_MAX_PATH_LEN];
    char  conf_tmp[NXT_MAX_PATH_LEN];
    char  ver[NXT_MAX_PATH_LEN];
    char  ver_tmp[NXT_MAX_PATH_LEN];
    char  certs[NXT_MAX_PATH_LEN];
    char  bundle[NXT_MAX_PATH_LEN];
} nxt_main_store_test_paths_t;


static nxt_uint_t         nxt_main_store_test_alerts;
static nxt_log_handler_t  nxt_main_store_test_next_handler;


/* Counts the alerts of this process, and logs as before. */
static void nxt_cdecl
nxt_main_store_test_log(nxt_uint_t level, nxt_log_t *log, const char *fmt,
    ...)
{
    u_char   *p;
    va_list  args;
    u_char   msg[NXT_MAX_ERROR_STR];

    if (level == NXT_LOG_ALERT) {
        nxt_main_store_test_alerts++;
    }

    va_start(args, fmt);
    p = nxt_vsprintf(msg, msg + sizeof(msg), fmt, args);
    va_end(args);

    nxt_main_store_test_next_handler(level, log, "%*s", (size_t) (p - msg),
                                     msg);
}


/* The mapping that the scheduler takes and unmaps. */
static u_char *
nxt_main_store_test_map(const char *content, size_t *size)
{
    u_char  *p;

    *size = nxt_strlen(content);

    p = nxt_mem_mmap(NULL, *size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (nxt_slow_path(p == MAP_FAILED)) {
        return NULL;
    }

    nxt_memcpy(p, content, *size);

    return p;
}


static nxt_int_t
nxt_main_store_test_schedule(nxt_task_t *task, const char *content)
{
    u_char  *p;
    size_t  size;

    p = nxt_main_store_test_map(content, &size);
    if (nxt_slow_path(p == NULL)) {
        return NXT_ERROR;
    }

    nxt_main_test_store_schedule(task, p, size);

    return NXT_OK;
}


static int
nxt_main_store_test_wait(nxt_pid_t pid, int *status)
{
    while (waitpid(pid, status, 0) != pid) {
        if (nxt_errno != NXT_EINTR) {
            return -1;
        }
    }

    return 0;
}


static nxt_bool_t
nxt_main_store_test_holds(const char *name, const char *expected)
{
    int      fd;
    char     buf[256];
    size_t   len;
    ssize_t  n;

    fd = open(name, O_RDONLY);
    if (fd == -1) {
        return 0;
    }

    n = read(fd, buf, sizeof(buf));

    close(fd);

    len = nxt_strlen(expected);

    return (n == (ssize_t) len && memcmp(buf, expected, len) == 0);
}


/* Stop and reap a store child that a failed case left behind. */
static void
nxt_main_store_test_reap_all(nxt_task_t *task)
{
    int        status;
    nxt_pid_t  pid;

    nxt_main_test_store_set_delay(0);
    nxt_main_test_store_set_exiting(task, 0);

    for ( ;; ) {
        pid = nxt_main_test_store_pid();
        if (pid == 0) {
            return;
        }

        (void) kill(pid, SIGKILL);

        if (nxt_main_store_test_wait(pid, &status) != 0) {
            return;
        }

        (void) nxt_main_test_store_exited(task, pid, status);
    }
}


/*
 * While main runs, a newer store does not stop the running child.  Stores
 * that come faster than one store takes would otherwise keep conf.json old
 * until they stop.
 */
static nxt_int_t
nxt_main_store_test_running(nxt_thread_t *thr, nxt_task_t *task,
    nxt_main_store_test_paths_t *paths)
{
    int        status;
    nxt_int_t  ret;
    nxt_pid_t  first, second;

    ret = NXT_ERROR;

    nxt_main_store_test_alerts = 0;

    nxt_main_test_store_set_delay(NXT_MAIN_STORE_TEST_DELAY);

    if (nxt_main_store_test_schedule(task, "{\"r\":1}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map the first store");
    }

    first = nxt_main_test_store_pid();
    if (first == 0) {
        nxt_main_store_test_fail(thr, "the first store started no child");
    }

    if (nxt_main_store_test_schedule(task, "{\"r\":2}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map the second store");
    }

    nxt_main_test_store_set_delay(0);

    if (nxt_main_store_test_wait(first, &status) != 0) {
        nxt_main_store_test_fail(thr, "waitpid(%PI) failed %E", first,
                                 nxt_errno);
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        nxt_main_store_test_fail(thr, "the running store child %PI did not "
                                 "finish while main runs: status 0x%Xi",
                                 first, (nxt_uint_t) status);
    }

    (void) nxt_main_test_store_exited(task, first, status);

    if (!nxt_main_store_test_holds(paths->conf, "{\"r\":1}")) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold the first "
                                 "configuration", paths->conf);
    }

    second = nxt_main_test_store_pid();
    if (second == 0 || second == first) {
        nxt_main_store_test_fail(thr, "the pending store did not start");
    }

    if (nxt_main_store_test_wait(second, &status) != 0) {
        nxt_main_store_test_fail(thr, "waitpid(%PI) failed %E", second,
                                 nxt_errno);
    }

    (void) nxt_main_test_store_exited(task, second, status);

    if (!nxt_main_store_test_holds(paths->conf, "{\"r\":2}")) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold the last "
                                 "configuration", paths->conf);
    }

    if (nxt_main_store_test_alerts != 0) {
        nxt_main_store_test_fail(thr, "%ui alerts", nxt_main_store_test_alerts);
    }

    ret = NXT_OK;

done:

    nxt_main_store_test_reap_all(task);

    return ret;
}


/* Reap "pid" and require that main killed it, without an alert. */
static nxt_int_t
nxt_main_store_test_killed(nxt_thread_t *thr, nxt_task_t *task,
    nxt_pid_t pid)
{
    int  status;

    if (nxt_main_store_test_wait(pid, &status) != 0) {
        nxt_log_alert(thr->log, "main store test: waitpid(%PI) failed %E",
                      pid, nxt_errno);
        return NXT_ERROR;
    }

    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        nxt_log_alert(thr->log, "main store test: the superseded store child "
                      "%PI was not killed at exit: it exited with status "
                      "0x%Xi", pid, (nxt_uint_t) status);
        return NXT_ERROR;
    }

    if (!nxt_main_test_store_exited(task, pid, status)) {
        nxt_log_alert(thr->log, "main store test: child %PI is not the store "
                      "child", pid);
        return NXT_ERROR;
    }

    if (nxt_main_store_test_alerts != 0) {
        nxt_log_alert(thr->log, "main store test: the killed child %PI was "
                      "logged as a failure (%ui alerts)", pid,
                      nxt_main_store_test_alerts);
        return NXT_ERROR;
    }

    if (nxt_conf_ver != 0) {
        nxt_log_alert(thr->log, "main store test: a killed child set "
                      "nxt_conf_ver");
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * At exit, a newer store stops the running child: first when the exit
 * starts with a store pending, then when a store comes during the exit.
 */
static nxt_int_t
nxt_main_store_test_cancel(nxt_thread_t *thr, nxt_task_t *task,
    nxt_main_store_test_paths_t *paths)
{
    int          status;
    char         ver[NXT_INT_T_LEN + 1];
    nxt_int_t    ret;
    nxt_pid_t    first, second, third;
    struct stat  st;

    ret = NXT_ERROR;

    nxt_conf_ver = 0;
    nxt_main_store_test_alerts = 0;

    nxt_main_test_store_set_delay(NXT_MAIN_STORE_TEST_DELAY);

    if (nxt_main_store_test_schedule(task, "{\"a\":1}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map the first store");
    }

    first = nxt_main_test_store_pid();
    if (first == 0) {
        nxt_main_store_test_fail(thr, "the first store started no child");
    }

    if (nxt_main_store_test_schedule(task, "{\"b\":2}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map the second store");
    }

    if (nxt_main_test_store_pid() != first) {
        nxt_main_store_test_fail(thr, "a second child started while the "
                                 "first one runs");
    }

    /* SIGTERM or SIGQUIT: the exit starts with a store pending. */
    nxt_main_test_store_set_exiting(task, 1);

    if (nxt_main_store_test_killed(thr, task, first) != NXT_OK) {
        goto done;
    }

    second = nxt_main_test_store_pid();
    if (second == 0 || second == first) {
        nxt_main_store_test_fail(thr, "the pending store did not start when "
                                 "the killed child was reaped");
    }

    /* A store that comes during the exit. */

    nxt_main_test_store_set_delay(0);

    if (nxt_main_store_test_schedule(task, "{\"c\":3}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map the third store");
    }

    if (nxt_main_store_test_killed(thr, task, second) != NXT_OK) {
        goto done;
    }

    third = nxt_main_test_store_pid();
    if (third == 0 || third == second) {
        nxt_main_store_test_fail(thr, "the store that came during the exit "
                                 "did not start");
    }

    if (nxt_main_store_test_wait(third, &status) != 0) {
        nxt_main_store_test_fail(thr, "waitpid(%PI) failed %E", third,
                                 nxt_errno);
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        nxt_main_store_test_fail(thr, "the last store failed: status "
                                 "0x%Xi", (nxt_uint_t) status);
    }

    (void) nxt_main_test_store_exited(task, third, status);

    if (nxt_main_test_store_pid() != 0) {
        nxt_main_store_test_fail(thr, "a store child runs with nothing "
                                 "pending");
    }

    if (nxt_conf_ver != NXT_VERNUM) {
        nxt_main_store_test_fail(thr, "nxt_conf_ver is not set after the "
                                 "version was stored");
    }

    if (!nxt_main_store_test_holds(paths->conf, "{\"c\":3}")) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold the last "
                                 "configuration", paths->conf);
    }

    (void) nxt_sprintf((u_char *) ver, (u_char *) ver + sizeof(ver), "%d%Z",
                       NXT_VERNUM);

    if (!nxt_main_store_test_holds(paths->ver, ver)) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold the version",
                                 paths->ver);
    }

    if (stat(paths->conf_tmp, &st) == 0 || stat(paths->ver_tmp, &st) == 0) {
        nxt_main_store_test_fail(thr, "a temporary file was left behind");
    }

    if (nxt_main_store_test_alerts != 0) {
        nxt_main_store_test_fail(thr, "%ui alerts", nxt_main_store_test_alerts);
    }

    ret = NXT_OK;

done:

    nxt_main_store_test_reap_all(task);

    return ret;
}


/*
 * The child finished its store before main killed it.  kill() of a zombie
 * succeeds, and the exit code 0 still counts.
 */
static nxt_int_t
nxt_main_store_test_finished(nxt_thread_t *thr, nxt_task_t *task,
    nxt_main_store_test_paths_t *paths)
{
    int        status;
    nxt_int_t  ret;
    nxt_pid_t  first, second;
    siginfo_t  info;

    ret = NXT_ERROR;

    nxt_conf_ver = 0;
    nxt_main_store_test_alerts = 0;

    nxt_main_test_store_set_delay(0);

    if (nxt_main_store_test_schedule(task, "{\"c\":3}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map the first store");
    }

    first = nxt_main_test_store_pid();
    if (first == 0) {
        nxt_main_store_test_fail(thr, "the first store started no child");
    }

    /* Wait for the exit, and leave the child a zombie. */

    nxt_memzero(&info, sizeof(info));

    while (waitid(P_PID, first, &info, WEXITED | WNOWAIT) != 0) {
        if (nxt_errno != NXT_EINTR) {
            nxt_main_store_test_fail(thr, "waitid(%PI) failed %E", first,
                                     nxt_errno);
        }
    }

    nxt_main_test_store_set_exiting(task, 1);

    if (nxt_main_store_test_schedule(task, "{\"d\":4}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map the second store");
    }

    if (nxt_main_store_test_wait(first, &status) != 0) {
        nxt_main_store_test_fail(thr, "waitpid(%PI) failed %E", first,
                                 nxt_errno);
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        nxt_main_store_test_fail(thr, "the first store failed: status 0x%Xi",
                                 (nxt_uint_t) status);
    }

    (void) nxt_main_test_store_exited(task, first, status);

    if (nxt_conf_ver != NXT_VERNUM) {
        nxt_main_store_test_fail(thr, "the store of a child that exited "
                                 "with 0 did not count");
    }

    second = nxt_main_test_store_pid();
    if (second == 0) {
        nxt_main_store_test_fail(thr, "the pending store did not start");
    }

    if (nxt_main_store_test_wait(second, &status) != 0) {
        nxt_main_store_test_fail(thr, "waitpid(%PI) failed %E", second,
                                 nxt_errno);
    }

    (void) nxt_main_test_store_exited(task, second, status);

    if (!nxt_main_store_test_holds(paths->conf, "{\"d\":4}")) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold the last "
                                 "configuration", paths->conf);
    }

    if (nxt_main_store_test_alerts != 0) {
        nxt_main_store_test_fail(thr, "%ui alerts", nxt_main_store_test_alerts);
    }

    ret = NXT_OK;

done:

    nxt_main_store_test_reap_all(task);

    return ret;
}


static nxt_msec_t
nxt_main_store_test_now(void)
{
    struct timespec  ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);

    return (nxt_msec_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}


/*
 * The child closes the write end of the pipe before its delay.  Before the
 * change, it held the write end until it exited, after the delay.
 */
static nxt_int_t
nxt_main_store_test_fds(nxt_thread_t *thr, nxt_task_t *task,
    nxt_main_store_test_paths_t *paths)
{
    int            pp[2], n, status;
    char           c;
    ssize_t        r;
    nxt_int_t      ret;
    nxt_pid_t      pid;
    nxt_msec_t     start, elapsed;
    struct pollfd  pfd;

    ret = NXT_ERROR;

    pp[0] = -1;
    pp[1] = -1;

    if (pipe(pp) != 0) {
        nxt_main_store_test_fail(thr, "pipe() failed %E", nxt_errno);
    }

    nxt_main_test_store_set_delay(NXT_MAIN_STORE_TEST_DELAY);

    start = nxt_main_store_test_now();

    if (nxt_main_store_test_schedule(task, "{\"e\":5}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map the store");
    }

    nxt_main_test_store_set_delay(0);

    pid = nxt_main_test_store_pid();
    if (pid == 0) {
        nxt_main_store_test_fail(thr, "the store started no child");
    }

    /* Now only the child can hold the write end. */

    (void) close(pp[1]);
    pp[1] = -1;

    pfd.fd = pp[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    do {
        n = poll(&pfd, 1, 4 * NXT_MAIN_STORE_TEST_DELAY);
    } while (n == -1 && nxt_errno == NXT_EINTR);

    elapsed = nxt_main_store_test_now() - start;

    if (n != 1) {
        nxt_main_store_test_fail(thr, "the pipe did not become readable: "
                                 "poll() returned %d", n);
    }

    r = read(pp[0], &c, 1);

    if (r != 0) {
        nxt_main_store_test_fail(thr, "read() of the pipe returned %z", r);
    }

    if (waitpid(pid, &status, WNOHANG) != 0
        || elapsed >= NXT_MAIN_STORE_TEST_DELAY / 2)
    {
        nxt_main_store_test_fail(thr, "the store child %PI held an inherited "
                                 "descriptor until it exited (%M ms)", pid,
                                 elapsed);
    }

    if (nxt_main_store_test_wait(pid, &status) != 0) {
        nxt_main_store_test_fail(thr, "waitpid(%PI) failed %E", pid,
                                 nxt_errno);
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        nxt_main_store_test_fail(thr, "the store failed: status 0x%Xi",
                                 (nxt_uint_t) status);
    }

    (void) nxt_main_test_store_exited(task, pid, status);

    if (!nxt_main_store_test_holds(paths->conf, "{\"e\":5}")) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold the "
                                 "configuration", paths->conf);
    }

    ret = NXT_OK;

done:

    nxt_main_store_test_reap_all(task);

    if (pp[0] != -1) {
        (void) close(pp[0]);
    }

    if (pp[1] != -1) {
        (void) close(pp[1]);
    }

    return ret;
}


static nxt_int_t
nxt_main_store_test_write(const char *name, const char *content)
{
    int      fd;
    size_t   len;
    ssize_t  n;

    fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd == -1) {
        return NXT_ERROR;
    }

    len = nxt_strlen(content);

    n = write(fd, content, len);

    close(fd);

    return (n == (ssize_t) len) ? NXT_OK : NXT_ERROR;
}


/* Reap the store child that runs now, and require that it exited with 0. */
static nxt_int_t
nxt_main_store_test_reap(nxt_thread_t *thr, nxt_task_t *task,
    const char *what)
{
    int        status;
    nxt_pid_t  pid;

    pid = nxt_main_test_store_pid();
    if (pid == 0) {
        nxt_log_alert(thr->log, "main store test: no store child for %s",
                      what);
        return NXT_ERROR;
    }

    if (nxt_main_store_test_wait(pid, &status) != 0) {
        nxt_log_alert(thr->log, "main store test: waitpid(%PI) failed %E",
                      pid, nxt_errno);
        return NXT_ERROR;
    }

    (void) nxt_main_test_store_exited(task, pid, status);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        nxt_log_alert(thr->log, "main store test: the store child of %s "
                      "ended with status 0x%Xi", what, (nxt_uint_t) status);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/* "<dir><name>" exists. */
static nxt_bool_t
nxt_main_store_test_exists(const char *dir, const char *name)
{
    char         path[NXT_MAX_PATH_LEN];
    struct stat  st;

    (void) nxt_sprintf((u_char *) path, (u_char *) path + sizeof(path),
                       "%s%s%Z", dir, name);

    return (stat(path, &st) == 0);
}


/* Queue a DELETE of "<dir><name>" that nobody waits for. */
static nxt_int_t
nxt_main_store_test_submit_delete(nxt_task_t *task, const char *dir,
    const char *name)
{
    nxt_str_t             d, n;
    nxt_main_store_job_t  *job;

    d.start = (u_char *) dir;
    d.length = nxt_strlen(dir);
    n.start = (u_char *) name;
    n.length = nxt_strlen(name);

    job = nxt_main_store_job_create(NXT_MAIN_STORE_DELETE, "file", &d, &n);
    if (nxt_slow_path(job == NULL)) {
        return NXT_ERROR;
    }

    nxt_main_store_submit(task, job);

    return NXT_OK;
}


/*
 * Stores start in the order they came.  A conf.json store "A" runs.  Then
 * come a DELETE of "first", a conf.json store "B", a DELETE of "old", and a
 * conf.json store "C" that replaces "B".  The order must be A, the DELETE
 * of "first", C, the DELETE of "old".  Before the change, both DELETEs ran
 * before C, so "old" was gone while conf.json still held A.
 */
static nxt_int_t
nxt_main_store_test_order(nxt_thread_t *thr, nxt_task_t *task,
    nxt_main_store_test_paths_t *paths)
{
    nxt_int_t  ret;
    char       dir[NXT_MAX_PATH_LEN], name[NXT_MAX_PATH_LEN];

    ret = NXT_ERROR;

    nxt_main_store_test_alerts = 0;

    nxt_main_test_store_set_delay(0);

    (void) nxt_sprintf((u_char *) dir, (u_char *) dir + sizeof(dir), "%s/%Z",
                       paths->dir);

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%sfirst%Z", dir);

    if (nxt_main_store_test_write(name, "first") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot write \"%s\"", name);
    }

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%sold%Z", dir);

    if (nxt_main_store_test_write(name, "old") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot write \"%s\"", name);
    }

    /* The child of A runs; the rest waits for it. */

    if (nxt_main_store_test_schedule(task, "{\"A\":1}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map A");
    }

    if (nxt_main_test_store_pid() == 0) {
        nxt_main_store_test_fail(thr, "A started no child");
    }

    if (nxt_main_store_test_submit_delete(task, dir, "first") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot queue the DELETE of first");
    }

    if (nxt_main_store_test_schedule(task, "{\"B\":2}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map B");
    }

    if (nxt_main_store_test_submit_delete(task, dir, "old") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot queue the DELETE of old");
    }

    if (nxt_main_store_test_schedule(task, "{\"C\":3}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map C");
    }

    if (nxt_main_store_test_reap(thr, task, "A") != NXT_OK) {
        goto done;
    }

    if (!nxt_main_store_test_holds(paths->conf, "{\"A\":1}")) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold A", paths->conf);
    }

    /* The DELETE of "first" came before B, so it runs before C. */

    if (nxt_main_store_test_reap(thr, task, "the second store") != NXT_OK) {
        goto done;
    }

    if (nxt_main_store_test_exists(dir, "first")
        || !nxt_main_store_test_holds(paths->conf, "{\"A\":1}"))
    {
        nxt_main_store_test_fail(thr, "the second store was not the DELETE "
                                 "of \"first\", which came before B");
    }

    /* C took the place of B, so it runs before the DELETE of "old". */

    if (nxt_main_store_test_reap(thr, task, "the third store") != NXT_OK) {
        goto done;
    }

    if (!nxt_main_store_test_exists(dir, "old")) {
        nxt_main_store_test_fail(thr, "\"%sold\" was deleted before C, which "
                                 "took the place of the older B", dir);
    }

    if (!nxt_main_store_test_holds(paths->conf, "{\"C\":3}")) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold C after the "
                                 "third store", paths->conf);
    }

    if (nxt_main_store_test_reap(thr, task, "the DELETE of old") != NXT_OK) {
        goto done;
    }

    if (nxt_main_store_test_exists(dir, "old")) {
        nxt_main_store_test_fail(thr, "\"%sold\" was not deleted", dir);
    }

    if (nxt_main_test_store_pid() != 0) {
        nxt_main_store_test_fail(thr, "a store child runs with nothing "
                                 "pending");
    }

    if (nxt_main_store_test_alerts != 0) {
        nxt_main_store_test_fail(thr, "%ui alerts", nxt_main_store_test_alerts);
    }

    ret = NXT_OK;

done:

    nxt_main_store_test_reap_all(task);

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%sfirst%Z", dir);
    (void) unlink(name);

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%sold%Z", dir);
    (void) unlink(name);

    return ret;
}


#if (NXT_TLS)

/* A message of the controller, as the port layer gives it to main. */
static void
nxt_main_store_test_msg(nxt_port_recv_msg_t *msg, nxt_buf_t *b, u_char *buf,
    size_t len, nxt_port_t *port, uint32_t stream, nxt_fd_t fd)
{
    nxt_memzero(msg, sizeof(nxt_port_recv_msg_t));
    nxt_memzero(b, sizeof(nxt_buf_t));

    b->mem.start = buf;
    b->mem.pos = buf;
    b->mem.free = buf + len;
    b->mem.end = buf + len;

    msg->buf = b;
    msg->fd[0] = fd;
    msg->fd[1] = -1;
    msg->port_msg.pid = nxt_pid;
    msg->port_msg.reply_port = port->id;
    msg->port_msg.stream = stream;
#if (NXT_USE_CMSG_PID)
    msg->cmsg_pid = nxt_pid;
#endif
}


/* PUT /certificates/<name>, as nxt_cert_store_put() sends it. */
static nxt_int_t
nxt_main_store_test_put(nxt_task_t *task, nxt_main_store_test_paths_t *paths,
    nxt_port_t *port, const char *name, const char *content, uint32_t stream)
{
    u_char               *p;
    size_t               size;
    nxt_fd_t             fd;
    nxt_buf_t            b;
    nxt_port_recv_msg_t  msg;
    u_char               buf[64];

    if (nxt_main_store_test_write(paths->bundle, content) != NXT_OK) {
        return NXT_ERROR;
    }

    fd = open(paths->bundle, O_RDONLY);
    if (fd == -1) {
        return NXT_ERROR;
    }

    size = nxt_strlen(content);

    p = nxt_cpymem(buf, name, nxt_strlen(name) + 1);
    p = nxt_cpymem(p, &size, sizeof(size_t));

    nxt_main_store_test_msg(&msg, &b, buf, p - buf, port, stream, fd);

    nxt_cert_store_put_handler(task, &msg);

    if (msg.fd[0] != -1 && nxt_test_fd_is_open(fd)) {
        (void) close(fd);
    }

    return NXT_OK;
}


/* DELETE /certificates/<name>, as nxt_cert_store_delete() sends it. */
static void
nxt_main_store_test_delete(nxt_task_t *task, nxt_port_t *port,
    const char *name)
{
    size_t               len;
    nxt_buf_t            b;
    nxt_port_recv_msg_t  msg;
    u_char               buf[64];

    len = nxt_strlen(name) + 1;

    nxt_memcpy(buf, name, len);

    nxt_main_store_test_msg(&msg, &b, buf, len, port, 0, -1);

    nxt_cert_store_delete_handler(task, &msg);
}


/* The number of answers on the port, and the last one. */
static nxt_uint_t
nxt_main_store_test_replies(nxt_port_t *port, nxt_port_send_msg_t **last)
{
    nxt_uint_t        n;
    nxt_queue_link_t  *link;

    n = 0;
    *last = NULL;

    for (link = nxt_queue_first(&port->messages);
         link != nxt_queue_tail(&port->messages);
         link = nxt_queue_next(link))
    {
        n++;
        *last = nxt_queue_link_data(link, nxt_port_send_msg_t, link);
    }

    return n;
}


static nxt_int_t
nxt_main_store_test_reply(nxt_thread_t *thr, nxt_port_t *port, nxt_uint_t n,
    uint32_t stream, const char *what)
{
    nxt_uint_t           count;
    nxt_port_send_msg_t  *sent;

    count = nxt_main_store_test_replies(port, &sent);

    if (count != n) {
        nxt_log_alert(thr->log, "main store test: %ui answers after %s "
                      "(expected %ui)", count, what, n);
        return NXT_ERROR;
    }

    if (sent->port_msg.type != _NXT_PORT_MSG_RPC_READY
        || sent->port_msg.last != 1
        || sent->port_msg.stream != stream)
    {
        nxt_log_alert(thr->log, "main store test: %s answered type %d, "
                      "last %d, stream %uD (expected %d, 1, %uD)", what,
                      (int) sent->port_msg.type, (int) sent->port_msg.last,
                      sent->port_msg.stream, (int) _NXT_PORT_MSG_RPC_READY,
                      stream);
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_main_store_test_certs(nxt_thread_t *thr, nxt_task_t *task,
    nxt_runtime_t *rt, nxt_main_store_test_paths_t *paths)
{
    char                 name[NXT_MAX_PATH_LEN];
    nxt_mp_t             *mp;
    nxt_int_t            ret;
    nxt_port_t           *port;
    struct stat          st;
    nxt_event_engine_t   engine, *saved_engine;
    nxt_port_send_msg_t  *sent;

    ret = NXT_ERROR;
    port = NULL;

    if (mkdir(paths->certs, 0700) != 0) {
        nxt_log_alert(thr->log, "main store test: mkdir(\"%s\") failed %E",
                      paths->certs, nxt_errno);
        return NXT_ERROR;
    }

    rt->certs.start = (u_char *) paths->certs;
    rt->certs.length = nxt_strlen(paths->certs);

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    /* The engine is what the teardown of queued messages needs. */
    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");
    engine.mem_pool = mp;

    saved_engine = thr->engine;
    thr->engine = &engine;
    rt->main_engine = &engine;

    /*
     * The port of the controller.  No queue and write_ready unset, so an
     * answer is queued on port->messages rather than written.
     */
    port = nxt_port_new(task, 0, nxt_pid, NXT_PROCESS_CONTROLLER);
    if (nxt_slow_path(port == NULL)) {
        goto done;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    rt->port_by_type[NXT_PROCESS_CONTROLLER] = port;

    if (nxt_slow_path(nxt_port_hash_add(&rt->ports, port) != NXT_OK)) {
        nxt_port_use(task, port, -1);
        port = NULL;
        goto done;
    }

    nxt_main_store_test_alerts = 0;

    /* The first upload.  Its child sleeps before it stores. */

    nxt_main_test_store_set_delay(NXT_MAIN_STORE_TEST_DELAY);

    if (nxt_main_store_test_put(task, paths, port, "one", "bundle one",
                                0x11111111)
        != NXT_OK)
    {
        nxt_main_store_test_fail(thr, "cannot send the first bundle");
    }

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_main_store_test_fail(thr, "main answered the upload before the "
                                 "bundle was stored");
    }

    if (nxt_main_test_store_pid() == 0) {
        nxt_main_store_test_fail(thr, "the upload started no store child");
    }

    /*
     * A conf.json store and a second upload arrive while it runs.  Main
     * starts to exit then, so a conf.json store child would be killed.
     */

    if (nxt_main_store_test_schedule(task, "{\"f\":6}") != NXT_OK) {
        nxt_main_store_test_fail(thr, "cannot map the conf.json store");
    }

    nxt_main_test_store_set_exiting(task, 1);

    if (nxt_main_store_test_put(task, paths, port, "two", "bundle two",
                                0x22222222)
        != NXT_OK)
    {
        nxt_main_store_test_fail(thr, "cannot send the second bundle");
    }

    nxt_main_test_store_set_delay(0);

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_main_store_test_fail(thr, "main answered an upload before its "
                                 "store child exited");
    }

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%sone%Z", paths->certs);

    if (nxt_main_store_test_reap(thr, task, "the first bundle") != NXT_OK) {
        goto done;
    }

    if (nxt_main_store_test_reply(thr, port, 1, 0x11111111,
                                  "the first bundle")
        != NXT_OK)
    {
        goto done;
    }

    if (!nxt_main_store_test_holds(name, "bundle one")) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold the first bundle "
                                 "after the answer", name);
    }

    /* The conf.json store came before the second upload. */

    if (nxt_main_store_test_reap(thr, task, "conf.json") != NXT_OK) {
        goto done;
    }

    if (!nxt_main_store_test_holds(paths->conf, "{\"f\":6}")) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold the "
                                 "configuration", paths->conf);
    }

    if (nxt_main_store_test_replies(port, &sent) != 1) {
        nxt_main_store_test_fail(thr, "the second bundle was stored before "
                                 "the older conf.json store");
    }

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%stwo%Z", paths->certs);

    if (nxt_main_store_test_reap(thr, task, "the second bundle") != NXT_OK) {
        goto done;
    }

    if (nxt_main_store_test_reply(thr, port, 2, 0x22222222,
                                  "the second bundle")
        != NXT_OK)
    {
        goto done;
    }

    if (!nxt_main_store_test_holds(name, "bundle two")) {
        nxt_main_store_test_fail(thr, "\"%s\" does not hold the second "
                                 "bundle after the answer", name);
    }

    if (nxt_main_test_store_pid() != 0) {
        nxt_main_store_test_fail(thr, "a store child runs with nothing "
                                 "pending");
    }

    nxt_main_test_store_set_exiting(task, 0);

    /* A DELETE right after a PUT of the same bundle runs after it. */

    nxt_main_test_store_set_delay(NXT_MAIN_STORE_TEST_DELAY);

    if (nxt_main_store_test_put(task, paths, port, "three", "bundle three",
                                0x33333333)
        != NXT_OK)
    {
        nxt_main_store_test_fail(thr, "cannot send the third bundle");
    }

    nxt_main_test_store_set_delay(0);

    nxt_main_store_test_delete(task, port, "three");

    if (nxt_main_store_test_reap(thr, task, "the third bundle") != NXT_OK) {
        goto done;
    }

    /* A delete that main ran at once started no child. */
    if (nxt_main_test_store_pid() != 0
        && nxt_main_store_test_reap(thr, task, "the delete") != NXT_OK)
    {
        goto done;
    }

    if (nxt_main_store_test_reply(thr, port, 3, 0x33333333,
                                  "the third bundle")
        != NXT_OK)
    {
        goto done;
    }

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%sthree%Z", paths->certs);

    if (stat(name, &st) == 0) {
        nxt_main_store_test_fail(thr, "\"%s\" is back: the delete ran before "
                                 "the store", name);
    }

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%s" NXT_MAIN_STORE_TMP "%Z", paths->certs);

    if (stat(name, &st) == 0) {
        nxt_main_store_test_fail(thr, "\"%s\" was left behind", name);
    }

    if (nxt_main_store_test_alerts != 0) {
        nxt_main_store_test_fail(thr, "%ui alerts", nxt_main_store_test_alerts);
    }

    ret = NXT_OK;

done:

    nxt_main_store_test_reap_all(task);

    if (port != NULL) {
        /* Frees the queued answers and their port references. */
        nxt_port_test_run_error_handler(task, port);

        (void) nxt_port_hash_remove(&rt->ports, port);
        rt->port_by_type[NXT_PROCESS_CONTROLLER] = NULL;

        nxt_port_use(task, port, -1);
    }

    thr->engine = saved_engine;
    rt->main_engine = NULL;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_mp_destroy(mp);

    (void) unlink(paths->bundle);

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%sone%Z", paths->certs);
    (void) unlink(name);

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%stwo%Z", paths->certs);
    (void) unlink(name);

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%sthree%Z", paths->certs);
    (void) unlink(name);

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%s" NXT_MAIN_STORE_TMP "%Z", paths->certs);
    (void) unlink(name);

    (void) rmdir(paths->certs);

    return ret;
}

#endif /* NXT_TLS */


static void
nxt_main_store_test_path(char *buf, const char *dir, const char *name)
{
    (void) nxt_sprintf((u_char *) buf, (u_char *) buf + NXT_MAX_PATH_LEN,
                       "%s/%s%Z", dir, name);
}


nxt_int_t
nxt_main_store_test(nxt_thread_t *thr)
{
    nxt_uint_t                   conf_ver;
    nxt_int_t                    ret;
    nxt_task_t                   *task;
    const char                   *tmpdir;
    nxt_runtime_t                rt, *saved_rt;
    nxt_main_store_test_paths_t  paths;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main store test started");

    task = thr->task;
    task->thread = thr;

    tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] != '/') {
        tmpdir = "/tmp";
    }

    (void) nxt_sprintf((u_char *) paths.dir,
                       (u_char *) paths.dir + sizeof(paths.dir),
                       "%s/nxt_main_store_test.XXXXXX%Z", tmpdir);

    if (nxt_slow_path(mkdtemp(paths.dir) == NULL)) {
        nxt_log_alert(thr->log, "main store test: mkdtemp(\"%s\") failed %E",
                      paths.dir, nxt_errno);
        return NXT_ERROR;
    }

    nxt_main_store_test_path(paths.conf, paths.dir, "conf.json");
    nxt_main_store_test_path(paths.conf_tmp, paths.dir, "conf.json.tmp");
    nxt_main_store_test_path(paths.ver, paths.dir, "version");
    nxt_main_store_test_path(paths.ver_tmp, paths.dir, "version.tmp");
    nxt_main_store_test_path(paths.certs, paths.dir, "certs/");
    nxt_main_store_test_path(paths.bundle, paths.dir, "bundle");

    nxt_memzero(&rt, sizeof(nxt_runtime_t));

    rt.state = paths.dir;
    rt.conf = paths.conf;
    rt.conf_tmp = paths.conf_tmp;
    rt.ver = paths.ver;
    rt.ver_tmp = paths.ver_tmp;

    saved_rt = thr->runtime;
    thr->runtime = &rt;

    conf_ver = nxt_conf_ver;

    nxt_main_store_test_next_handler = thr->log->handler;
    thr->log->handler = nxt_main_store_test_log;

    ret = nxt_main_store_test_running(thr, task, &paths);

    if (ret == NXT_OK) {
        ret = nxt_main_store_test_cancel(thr, task, &paths);
    }

    if (ret == NXT_OK) {
        ret = nxt_main_store_test_finished(thr, task, &paths);
    }

    if (ret == NXT_OK) {
        ret = nxt_main_store_test_fds(thr, task, &paths);
    }

    if (ret == NXT_OK) {
        ret = nxt_main_store_test_order(thr, task, &paths);
    }

#if (NXT_TLS)
    if (ret == NXT_OK) {
        ret = nxt_main_store_test_certs(thr, task, &rt, &paths);
    }
#endif

    thr->log->handler = nxt_main_store_test_next_handler;
    thr->runtime = saved_rt;
    nxt_conf_ver = conf_ver;

    (void) unlink(paths.conf);
    (void) unlink(paths.conf_tmp);
    (void) unlink(paths.ver);
    (void) unlink(paths.ver_tmp);
    (void) rmdir(paths.dir);

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "main store test passed");
    }

    return ret;
}
