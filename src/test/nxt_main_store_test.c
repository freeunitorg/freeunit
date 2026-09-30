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
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_main_process.h>
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
