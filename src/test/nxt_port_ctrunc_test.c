/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for control-data truncation on a port socket
 * (src/nxt_socket_msg.h, src/nxt_socket_msg.c).
 *
 * recvmsg() sets MSG_CTRUNC when it could not deliver all of the control
 * data the sender attached.  The case that matters here is an SCM_RIGHTS the
 * kernel discards while delivering everything else -- the payload and, on
 * Linux, the SCM_CREDENTIALS the receiver's SO_PASSCRED asks for.  In
 * production that happens under descriptor pressure: scm_detach_fds() cannot
 * install the descriptors and drops the whole cmsg.
 *
 * Nothing used to look at msg_flags, so what reached a handler was a message
 * that passed every check it makes -- right size, right type, kernel-validated
 * sender -- and simply had fd[0] == -1.  NEW_PORT, MMAP and CHANGE_FILE each
 * take some fd-less path from there.
 *
 * The truncation is driven directly rather than through RLIMIT_NOFILE: the
 * receive buffer's length is the other input to the same kernel decision, and
 * unlike a descriptor limit it is deterministic and leaves the rest of the
 * test process alone.  What is asserted is the contract every receive path
 * shares, that nxt_socket_msg_oob_get() and nxt_socket_msg_oob_get_fds()
 * refuse a truncated control block -- and that they refuse it after reporting
 * whatever descriptors did arrive, since the caller can only close what it
 * has been told about.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_socket_msg.h>
#include "nxt_tests.h"

#include <fcntl.h>


#define NXT_CTRUNC_TEST_FD_MAX  1024

/*
 * The control space the receiver has to keep for the credential cmsg before
 * the SCM_RIGHTS can begin.  Zero where the platform passes no credential.
 */
#if (NXT_CRED_USECMSG)
#define NXT_CTRUNC_TEST_CRED_SPACE  CMSG_SPACE(sizeof(nxt_socket_cred_t))
#else
#define NXT_CTRUNC_TEST_CRED_SPACE  0
#endif


static nxt_uint_t
nxt_port_ctrunc_test_fd_count(void)
{
    int         fd;
    nxt_uint_t  n;

    n = 0;

    for (fd = 0; fd < NXT_CTRUNC_TEST_FD_MAX; fd++) {
        if (fcntl(fd, F_GETFD) != -1) {
            n++;
        }
    }

    return n;
}


/*
 * Send one message carrying two descriptors, and receive it with a control
 * buffer of exactly "controllen" bytes, filling "oob" the way nxt_recvmsg()
 * would.  A short buffer is what makes the kernel drop the SCM_RIGHTS and
 * raise MSG_CTRUNC.
 */
static nxt_int_t
nxt_port_ctrunc_test_exchange(nxt_thread_t *thr, nxt_socket_t *pair,
    size_t controllen, nxt_recv_oob_t *oob, ssize_t *received)
{
    int             fds[2];
    u_char          payload;
    ssize_t         n;
    nxt_send_oob_t  send_oob;
    struct iovec    iov[1];
    struct msghdr   msg;

    fds[0] = open("/dev/null", O_RDONLY);
    fds[1] = open("/dev/null", O_RDONLY);

    if (fds[0] == -1 || fds[1] == -1) {
        nxt_log_alert(thr->log, "port ctrunc test failed to open /dev/null");
        goto fail;
    }

    nxt_socket_msg_oob_init(&send_oob, fds);

    payload = 0x5A;

    iov[0].iov_base = &payload;
    iov[0].iov_len = sizeof(payload);

    n = nxt_sendmsg(pair[1], iov, 1, &send_oob);

    if (n != (ssize_t) sizeof(payload)) {
        nxt_log_alert(thr->log, "port ctrunc test sendmsg failed %E",
                      nxt_errno);
        goto fail;
    }

    /*
     * sendmsg() duplicated the descriptors into the socket; this end has no
     * further use for them, and leaving them open would hide a leak on the
     * receiving side behind a constant offset.
     */
    (void) close(fds[0]);
    (void) close(fds[1]);

    fds[0] = -1;
    fds[1] = -1;

    payload = 0;

    iov[0].iov_base = &payload;
    iov[0].iov_len = sizeof(payload);

    nxt_memzero(oob, sizeof(nxt_recv_oob_t));

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = oob->buf;
    msg.msg_controllen = controllen;
    msg.msg_flags = 0;

    n = recvmsg(pair[0], &msg, 0);

    if (n != (ssize_t) sizeof(payload) || payload != 0x5A) {
        nxt_log_alert(thr->log, "port ctrunc test recvmsg failed %z %E",
                      n, nxt_errno);
        return NXT_ERROR;
    }

    oob->size = msg.msg_controllen;
    oob->truncated = (msg.msg_flags & MSG_CTRUNC) != 0;

    *received = n;

    return NXT_OK;

fail:

    if (fds[0] != -1) {
        (void) close(fds[0]);
    }

    if (fds[1] != -1) {
        (void) close(fds[1]);
    }

    return NXT_ERROR;
}


static void
nxt_port_ctrunc_test_close_fds(nxt_fd_t *fd)
{
    nxt_uint_t  i;

    for (i = 0; i < 2; i++) {
        if (fd[i] != -1) {
            (void) close(fd[i]);
            fd[i] = -1;
        }
    }
}


/*
 * The regression itself: a control buffer that holds the credential but not
 * the SCM_RIGHTS.  Everything a handler inspects is intact and the message is
 * one descriptor short, which is exactly the shape the kernel delivers under
 * descriptor pressure.  Before the fix both accessors returned NXT_OK here.
 */
static nxt_int_t
nxt_port_ctrunc_test_truncated(nxt_thread_t *thr, nxt_socket_t *pair,
    size_t controllen, const char *name)
{
    ssize_t         n;
    nxt_int_t       ret;
    nxt_pid_t       pid;
    nxt_uint_t      before, after;
    nxt_fd_t        fd[2], fd2[2];
    nxt_recv_oob_t  oob;

    before = nxt_port_ctrunc_test_fd_count();

    if (nxt_port_ctrunc_test_exchange(thr, pair, controllen, &oob, &n)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (!oob.truncated) {
        nxt_log_alert(thr->log, "port ctrunc test: %s did not truncate "
                      "(controllen %uz, received %uz)",
                      name, controllen, oob.size);
        return NXT_ERROR;
    }

    fd[0] = -1;
    fd[1] = -1;

    /*
     * The two accessors are checked against the same received control block,
     * so both see the identical descriptor numbers; they are closed once, at
     * the end, rather than after each call.
     */
    ret = nxt_socket_msg_oob_get_fds(&oob, fd);

    if (ret != NXT_ERROR) {
        nxt_log_alert(thr->log, "port ctrunc test: %s: oob_get_fds accepted "
                      "a truncated control block (fd %d)", name, fd[0]);

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }

    fd2[0] = -1;
    fd2[1] = -1;
    pid = -1;

    ret = nxt_socket_msg_oob_get(&oob, fd2, &pid);

    if (ret != NXT_ERROR) {
        nxt_log_alert(thr->log, "port ctrunc test: %s: oob_get accepted a "
                      "truncated control block (fd %d, pid %PI)",
                      name, fd2[0], pid);

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }

    if (fd2[0] != fd[0] || fd2[1] != fd[1]) {
        nxt_log_alert(thr->log, "port ctrunc test: %s: the two accessors "
                      "reported different descriptors (%d %d vs %d %d)",
                      name, fd[0], fd[1], fd2[0], fd2[1]);

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }

    /*
     * Whatever the kernel did install has to come back in fd[] even though
     * the call failed, or nothing can close it.
     */
    nxt_port_ctrunc_test_close_fds(fd);

    after = nxt_port_ctrunc_test_fd_count();

    if (after != before) {
        nxt_log_alert(thr->log, "port ctrunc test: %s leaked %d descriptors",
                      name, (int) (after - before));
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * The control case: a buffer of the size every production receive path uses.
 * A message Unit can send never truncates against it, so the accessors accept
 * it and hand back both descriptors -- which is what makes the case above an
 * assertion about truncation rather than about the buffer being short.
 */
static nxt_int_t
nxt_port_ctrunc_test_intact(nxt_thread_t *thr, nxt_socket_t *pair)
{
    ssize_t         n;
    nxt_int_t       ret;
    nxt_pid_t       pid;
    nxt_uint_t      before, after;
    nxt_fd_t        fd[2];
    nxt_recv_oob_t  oob;

    before = nxt_port_ctrunc_test_fd_count();

    if (nxt_port_ctrunc_test_exchange(thr, pair, NXT_OOB_RECV_SIZE, &oob, &n)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (oob.truncated) {
        nxt_log_alert(thr->log, "port ctrunc test: a %uz byte control buffer "
                      "truncated a two descriptor message",
                      (size_t) NXT_OOB_RECV_SIZE);
        return NXT_ERROR;
    }

    fd[0] = -1;
    fd[1] = -1;
    pid = -1;

    ret = nxt_socket_msg_oob_get(&oob, fd, &pid);

    if (ret != NXT_OK || fd[0] == -1 || fd[1] == -1) {
        nxt_log_alert(thr->log, "port ctrunc test: intact message refused "
                      "(ret %i, fd %d %d)", ret, fd[0], fd[1]);

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }

#if (NXT_CRED_USECMSG)
    if (pid != getpid()) {
        nxt_log_alert(thr->log, "port ctrunc test: intact message carried "
                      "pid %PI, expected %PI", pid, (nxt_pid_t) getpid());

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }
#endif

    nxt_port_ctrunc_test_close_fds(fd);

    after = nxt_port_ctrunc_test_fd_count();

    if (after != before) {
        nxt_log_alert(thr->log, "port ctrunc test: intact case leaked %d "
                      "descriptors", (int) (after - before));
        return NXT_ERROR;
    }

    return NXT_OK;
}


nxt_int_t
nxt_port_ctrunc_test(nxt_thread_t *thr)
{
    nxt_int_t      ret;
    nxt_task_t     *task;
    nxt_socket_t   pair[2];

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port ctrunc test started");

    task = thr->task;
    task->thread = thr;

    /*
     * nxt_socketpair_create() rather than socketpair(): SO_PASSCRED is what
     * puts a credential cmsg ahead of the SCM_RIGHTS, and so what lets a
     * buffer sized for the credential alone reproduce the production shape --
     * a fully authenticated message with its descriptor missing.
     */
    if (nxt_socketpair_create(task, pair) != NXT_OK) {
        nxt_log_alert(thr->log, "port ctrunc test: socketpair failed");
        return NXT_ERROR;
    }

    ret = nxt_port_ctrunc_test_intact(thr, pair);

    if (ret == NXT_OK) {
        ret = nxt_port_ctrunc_test_truncated(thr, pair,
                                  NXT_CTRUNC_TEST_CRED_SPACE + CMSG_LEN(0),
                                  "credential kept, SCM_RIGHTS dropped");
    }

    if (ret == NXT_OK) {
        /*
         * The other side of the same kernel decision: room for one descriptor
         * where two were sent.  scm_detach_fds() installs what fits and still
         * raises MSG_CTRUNC, so the accessors have to report the partial
         * descriptor and refuse the message, or it leaks.
         *
         * The length is CMSG_LEN(0) + sizeof(int), not CMSG_SPACE(sizeof(int)):
         * cmsg padding rounds a one-descriptor payload up to the same space
         * two occupy, so a CMSG_SPACE-derived budget delivers both and does
         * not truncate at all.
         */
        ret = nxt_port_ctrunc_test_truncated(thr, pair,
                                  NXT_CTRUNC_TEST_CRED_SPACE
                                  + CMSG_LEN(0) + sizeof(int),
                                  "one of two descriptors delivered");
    }

    nxt_socket_close(task, pair[0]);
    nxt_socket_close(task, pair[1]);

    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port ctrunc test passed");

    return NXT_OK;
}
