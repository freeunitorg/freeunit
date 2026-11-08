/*
 * Copyright (C) FreeUnit Community
 */

/*
 * Regression test for close-on-exec on the descriptors that Unit creates or
 * receives.
 *
 * An external application process is the only Unit process that calls
 * execve().  It inherited every descriptor of its prototype process that was
 * not close-on-exec: the epoll, signalfd and eventfd descriptors of the event
 * engine, the control socket, the log file and the descriptors that came
 * over a port.  The application cannot use them, and each one counts against
 * its RLIMIT_NOFILE.
 *
 * Each case makes a descriptor as Unit does and checks FD_CLOEXEC.  The cases
 * run in a child process: the engine blocks signals, and a failed case
 * leaves its descriptors open.
 */

#include <nxt_main.h>
#include <nxt_event_engine.h>
#include <nxt_socket_msg.h>
#include "nxt_tests.h"


static nxt_bool_t
nxt_cloexec_test_is_set(nxt_fd_t fd)
{
    int  flags;

    flags = fcntl(fd, F_GETFD);

    return flags != -1 && (flags & FD_CLOEXEC) != 0;
}


#if ((NXT_HAVE_EPOLL && !NXT_TEST_BUILD_EPOLL) || NXT_HAVE_KQUEUE)

static void
nxt_cloexec_test_signal(nxt_task_t *task, void *obj, void *data)
{
}


static const nxt_sig_event_t  nxt_cloexec_test_signals[] = {
    nxt_event_signal(SIGUSR2, nxt_cloexec_test_signal),
    nxt_event_signal_end,
};


#if (NXT_HAVE_EPOLL && !NXT_TEST_BUILD_EPOLL)

static nxt_int_t
nxt_cloexec_test_engine(nxt_thread_t *thr)
{
    nxt_event_engine_t  *engine;

    /* The main, controller and prototype processes have signals. */

    engine = nxt_event_engine_create(thr->task, &nxt_epoll_level_engine,
                                     nxt_cloexec_test_signals, 0, 0);

    NXT_TEST_CHECK(thr->log, engine != NULL,
                   "cloexec test: no epoll engine");

    NXT_TEST_CHECK(thr->log, nxt_cloexec_test_is_set(engine->u.epoll.fd),
                   "cloexec test: epoll descriptor %d is inheritable",
                   engine->u.epoll.fd);

#if (NXT_HAVE_SIGNALFD)
    NXT_TEST_CHECK(thr->log,
                   nxt_cloexec_test_is_set(engine->u.epoll.signalfd.fd),
                   "cloexec test: signalfd descriptor %d is inheritable",
                   engine->u.epoll.signalfd.fd);
#endif

#if (NXT_HAVE_EVENTFD)
    NXT_TEST_CHECK(thr->log,
                   nxt_cloexec_test_is_set(engine->u.epoll.eventfd.fd),
                   "cloexec test: eventfd descriptor %d is inheritable",
                   engine->u.epoll.eventfd.fd);
#endif

    return NXT_OK;
}


/*
 * A failed nxt_epoll_create() calls nxt_epoll_free(), which closes each
 * engine descriptor that is not -1.  The eventfd descriptor must be -1
 * before the engine makes it, else nxt_epoll_free() closes descriptor 0.
 *
 * The case runs in its own child process, so the test program keeps its
 * descriptor 0 if the case fails.
 */

static int
nxt_cloexec_test_emfile_child(void *data)
{
    int                 fd;
    nxt_thread_t        *thr;
    struct rlimit       rlim;
    nxt_event_engine_t  *engine;

    thr = data;

    /* Descriptor 0 must be open, else the case cannot see it close. */

    if (fcntl(0, F_GETFD) == -1) {
        fd = open("/dev/null", O_RDONLY);

        if (fd != 0) {
            nxt_log_alert(thr->log, "cloexec test: cannot open descriptor 0, "
                          "open() returned %d %E", fd, nxt_errno);
            return 1;
        }
    }

    /*
     * The lowest free descriptor becomes the limit, so the next descriptor
     * that the process makes fails with EMFILE.
     */

    fd = dup(0);

    if (fd == -1) {
        nxt_log_alert(thr->log, "cloexec test: dup(0) failed %E", nxt_errno);
        return 1;
    }

    (void) close(fd);

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        nxt_log_alert(thr->log, "cloexec test: getrlimit() failed %E",
                      nxt_errno);
        return 1;
    }

    rlim.rlim_cur = fd;

    if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        nxt_log_alert(thr->log, "cloexec test: setrlimit(%d) failed %E",
                      fd, nxt_errno);
        return 1;
    }

    engine = nxt_event_engine_create(thr->task, &nxt_epoll_level_engine,
                                     NULL, 0, 0);

    if (engine != NULL) {
        nxt_log_alert(thr->log, "cloexec test: the epoll engine was made "
                      "with RLIMIT_NOFILE %d", fd);
        return 1;
    }

    if (fcntl(0, F_GETFD) == -1) {
        nxt_log_alert(thr->log, "cloexec test: descriptor 0 is closed after "
                      "a failed epoll engine create %E", nxt_errno);
        return 1;
    }

    return 0;
}

#else

static nxt_int_t
nxt_cloexec_test_engine(nxt_thread_t *thr)
{
    nxt_event_engine_t  *engine;

    engine = nxt_event_engine_create(thr->task, &nxt_kqueue_engine,
                                     nxt_cloexec_test_signals, 0, 0);

    NXT_TEST_CHECK(thr->log, engine != NULL,
                   "cloexec test: no kqueue engine");

    NXT_TEST_CHECK(thr->log, nxt_cloexec_test_is_set(engine->u.kqueue.fd),
                   "cloexec test: kqueue descriptor %d is inheritable",
                   engine->u.kqueue.fd);

    return NXT_OK;
}

#endif

#endif


static nxt_int_t
nxt_cloexec_test_socket(nxt_thread_t *thr)
{
    nxt_socket_t  s;

    s = nxt_socket_create(thr->task, AF_UNIX, SOCK_STREAM, 0, 0);

    NXT_TEST_CHECK(thr->log, s != -1, "cloexec test: socket() failed");

    NXT_TEST_CHECK(thr->log, nxt_cloexec_test_is_set(s),
                   "cloexec test: socket %d is inheritable", s);

    nxt_socket_close(thr->task, s);

    return NXT_OK;
}


/*
 * The descriptor sent is inheritable in the sender.  The one received is a
 * new descriptor, and only the receiving side decides its flag.
 */

static nxt_int_t
nxt_cloexec_test_recvmsg(nxt_thread_t *thr)
{
    int             pair[2], pp[2], fds[2], rfd[2];
    u_char          byte;
    ssize_t         n;
    nxt_iobuf_t     iob;
    nxt_send_oob_t  soob;
    nxt_recv_oob_t  roob;

    NXT_TEST_CHECK(thr->log, socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0,
                   "cloexec test: socketpair() failed %E", nxt_errno);

    NXT_TEST_CHECK(thr->log, pipe(pp) == 0,
                   "cloexec test: pipe() failed %E", nxt_errno);

    NXT_TEST_CHECK(thr->log, !nxt_cloexec_test_is_set(pp[0]),
                   "cloexec test: pipe() made a close-on-exec descriptor");

    byte = 'x';
    iob.iov_base = &byte;
    iob.iov_len = 1;

    fds[0] = pp[0];
    fds[1] = -1;

    nxt_socket_msg_oob_init(&soob, fds);

    n = nxt_sendmsg(pair[0], &iob, 1, &soob, 0);

    NXT_TEST_CHECK(thr->log, n == 1, "cloexec test: sendmsg() returned %z %E",
                   n, nxt_errno);

    n = nxt_recvmsg(pair[1], &iob, 1, &roob);

    NXT_TEST_CHECK(thr->log, n == 1, "cloexec test: recvmsg() returned %z %E",
                   n, nxt_errno);

    rfd[0] = -1;
    rfd[1] = -1;

    NXT_TEST_CHECK(thr->log,
                   nxt_socket_msg_oob_get_fds(&roob, rfd) == NXT_OK
                   && rfd[0] != -1 && rfd[1] == -1,
                   "cloexec test: received descriptors %d, %d",
                   rfd[0], rfd[1]);

    NXT_TEST_CHECK(thr->log, nxt_cloexec_test_is_set(rfd[0]),
                   "cloexec test: descriptor %d from SCM_RIGHTS is "
                   "inheritable", rfd[0]);

    (void) close(rfd[0]);
    (void) close(pp[0]);
    (void) close(pp[1]);
    (void) close(pair[0]);
    (void) close(pair[1]);

    return NXT_OK;
}


/*
 * The log file: main opens it with nxt_file_open(), and a reopen redirects
 * the old descriptor to the new file with nxt_file_redirect().
 */

static nxt_int_t
nxt_cloexec_test_file(nxt_thread_t *thr, char *path)
{
    nxt_fd_t    fd;
    nxt_int_t   ret;
    nxt_file_t  file;

    nxt_memzero(&file, sizeof(nxt_file_t));

    file.name = (nxt_file_name_t *) path;
    file.log_level = NXT_LOG_ALERT;

    ret = nxt_file_open(thr->task, &file, O_WRONLY | O_APPEND, O_CREAT,
                        NXT_FILE_OWNER_ACCESS);

    NXT_TEST_CHECK(thr->log, ret == NXT_OK, "cloexec test: open() failed");

    NXT_TEST_CHECK(thr->log, nxt_cloexec_test_is_set(file.fd),
                   "cloexec test: nxt_file_open() descriptor %d is "
                   "inheritable", file.fd);

    fd = open(path, O_WRONLY | O_APPEND);

    NXT_TEST_CHECK(thr->log, fd != -1,
                   "cloexec test: open(\"%s\") failed %E", path, nxt_errno);

    ret = nxt_file_redirect(&file, fd);

    NXT_TEST_CHECK(thr->log, ret == NXT_OK,
                   "cloexec test: nxt_file_redirect() failed");

    NXT_TEST_CHECK(thr->log, nxt_cloexec_test_is_set(file.fd),
                   "cloexec test: descriptor %d is inheritable after "
                   "nxt_file_redirect()", file.fd);

    nxt_file_close(thr->task, &file);

#if (NXT_HAVE_OPENAT2)
    /* Without O_CREAT, openat2() refuses a non-zero mode. */

    ret = nxt_file_openat2(thr->task, &file, O_WRONLY | O_APPEND, 0, 0,
                           AT_FDCWD, 0);

    /* A seccomp filter or an old kernel can refuse the system call. */

    if (ret != NXT_OK && (file.error == NXT_ENOSYS || file.error == EPERM)) {
        return NXT_OK;
    }

    NXT_TEST_CHECK(thr->log, ret == NXT_OK, "cloexec test: openat2() failed");

    NXT_TEST_CHECK(thr->log, nxt_cloexec_test_is_set(file.fd),
                   "cloexec test: nxt_file_openat2() descriptor %d is "
                   "inheritable", file.fd);

    nxt_file_close(thr->task, &file);
#endif

    return NXT_OK;
}


typedef struct {
    nxt_thread_t  *thr;
    char          *path;
} nxt_cloexec_test_t;


static int
nxt_cloexec_test_child(void *data)
{
    nxt_thread_t        *thr;
    nxt_cloexec_test_t  *t;

    t = data;
    thr = t->thr;

#if ((NXT_HAVE_EPOLL && !NXT_TEST_BUILD_EPOLL) || NXT_HAVE_KQUEUE)
    if (nxt_cloexec_test_engine(thr) != NXT_OK) {
        return 1;
    }
#endif

    if (nxt_cloexec_test_socket(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_cloexec_test_recvmsg(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_cloexec_test_file(thr, t->path) != NXT_OK) {
        return 1;
    }

    return 0;
}


nxt_int_t
nxt_cloexec_test(nxt_thread_t *thr)
{
    int                 rc, fd;
    char                path[256];
    const char          *tmpdir;
    nxt_cloexec_test_t  t;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "cloexec test started");

    thr->task->thread = thr;

    tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] != '/') {
        tmpdir = "/tmp";
    }

    (void) nxt_sprintf((u_char *) path, (u_char *) path + sizeof(path),
                       "%s/nxt_cloexec_test.XXXXXX%Z", tmpdir);

    fd = mkstemp(path);

    NXT_TEST_CHECK(thr->log, fd != -1,
                   "cloexec test: mkstemp(\"%s\") failed %E", path, nxt_errno);

    (void) close(fd);

    t.thr = thr;
    t.path = path;

    rc = nxt_test_in_child(thr, "cloexec test", nxt_cloexec_test_child, &t);

    (void) unlink(path);

    NXT_TEST_CHECK(thr->log, rc == 0,
                   "cloexec test failed, child status %d", rc);

#if (NXT_HAVE_EPOLL && !NXT_TEST_BUILD_EPOLL)
    rc = nxt_test_in_child(thr, "cloexec test", nxt_cloexec_test_emfile_child,
                           thr);

    NXT_TEST_CHECK(thr->log, rc == 0,
                   "cloexec test: EMFILE case failed, child status %d", rc);
#endif

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "cloexec test passed");

    return NXT_OK;
}
