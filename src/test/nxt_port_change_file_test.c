/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for nxt_port_change_log_file_handler() (src/nxt_port.c).
 *
 * CHANGE_FILE carries a log file slot in its payload and the descriptor the
 * slot is to be redirected to.  Neither is authenticated, and the discovery
 * and prototype processes, the router and the controller all install this
 * handler.
 *
 * The handler read the slot without bounding it against the payload, passed
 * it to nxt_list_elt() -- which returns NULL past the end of the list -- and
 * dereferenced the result, so a forged slot crashed the receiver.  It also
 * never closed the second descriptor of the message, and lost the first one
 * when the redirect failed; the dispatcher does not reclaim fds once the
 * handler returns (see nxt_port_recv_msg_close_fds()), so a peer attaching
 * descriptors to forged messages can exhaust the receiver's descriptor
 * table.
 *
 * A message can carry two descriptors regardless of how many its type
 * normally uses, so the rejected cases attach both and expect both back.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include "nxt_tests.h"

#include <fcntl.h>


static nxt_bool_t
nxt_port_change_file_test_fd_is_open(nxt_fd_t fd)
{
    return fcntl(fd, F_GETFD) != -1;
}


/*
 * Feed the handler a message it has to refuse, and check that nothing the
 * message carried outlives the call.
 */
static nxt_int_t
nxt_port_change_file_test_reject(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_recv_msg_t *msg, nxt_bool_t with_fd0, const char *name)
{
    nxt_fd_t  fd0, fd1;

    fd0 = -1;

    if (with_fd0) {
        fd0 = open("/dev/null", O_RDONLY);
    }

    fd1 = open("/dev/null", O_RDONLY);

    if ((with_fd0 && fd0 == -1) || fd1 == -1) {
        nxt_log_alert(thr->log, "port change file test failed to open "
                      "/dev/null");
        goto fail;
    }

    msg->fd[0] = fd0;
    msg->fd[1] = fd1;

    nxt_port_change_log_file_handler(task, msg);

    if ((fd0 != -1 && nxt_port_change_file_test_fd_is_open(fd0))
        || nxt_port_change_file_test_fd_is_open(fd1))
    {
        nxt_log_alert(thr->log, "port change file test: %s leaked a "
                      "descriptor", name);
        goto fail;   /* closes only what is still open */
    }

    if (msg->fd[0] != -1 || msg->fd[1] != -1) {
        nxt_log_alert(thr->log, "port change file test: %s left fds in the "
                      "message", name);
        return NXT_ERROR;
    }

    return NXT_OK;

fail:

    /*
     * Close only what the handler left open: the descriptors it consumed
     * are gone already, and closing them again could reach an unrelated
     * descriptor that reused the number.
     */
    if (fd0 != -1 && nxt_port_change_file_test_fd_is_open(fd0)) {
        (void) close(fd0);
    }

    if (fd1 != -1 && nxt_port_change_file_test_fd_is_open(fd1)) {
        (void) close(fd1);
    }

    return NXT_ERROR;
}


/*
 * The accepted case: the slot exists, so the first descriptor is redirected
 * onto the log file -- and consumed by dup2() doing so -- while the second
 * one, which this message type has no use for, is closed rather than left
 * behind.
 */
static nxt_int_t
nxt_port_change_file_test_accept(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_recv_msg_t *msg, nxt_file_t *log_file, const char *name)
{
    nxt_fd_t  fd0, fd1;

    fd0 = open("/dev/null", O_WRONLY);
    fd1 = open("/dev/null", O_RDONLY);

    if (fd0 == -1 || fd1 == -1) {
        nxt_log_alert(thr->log, "port change file test failed to open "
                      "/dev/null");
        goto fail;
    }

    msg->fd[0] = fd0;
    msg->fd[1] = fd1;

    nxt_port_change_log_file_handler(task, msg);

    if (nxt_port_change_file_test_fd_is_open(fd0)
        || nxt_port_change_file_test_fd_is_open(fd1))
    {
        nxt_log_alert(thr->log, "port change file test: %s leaked a "
                      "descriptor", name);
        goto fail;
    }

    if (msg->fd[0] != -1 || msg->fd[1] != -1) {
        nxt_log_alert(thr->log, "port change file test: %s left fds in the "
                      "message", name);
        return NXT_ERROR;
    }

    if (!nxt_port_change_file_test_fd_is_open(log_file->fd)) {
        nxt_log_alert(thr->log, "port change file test: %s closed the log "
                      "file", name);
        return NXT_ERROR;
    }

    return NXT_OK;

fail:

    if (fd0 != -1 && nxt_port_change_file_test_fd_is_open(fd0)) {
        (void) close(fd0);
    }

    if (fd1 != -1 && nxt_port_change_file_test_fd_is_open(fd1)) {
        (void) close(fd1);
    }

    return NXT_ERROR;
}


nxt_int_t
nxt_port_change_file_test(nxt_thread_t *thr)
{
    nxt_mp_t             *mp;
    nxt_buf_t            *b;
    nxt_file_name_t      *name;
    nxt_uint_t           slot;
    nxt_task_t           *task;
    nxt_int_t            ret;
    nxt_file_t           *log_file;
    nxt_runtime_t        *rt, *saved_rt;
    nxt_port_recv_msg_t  msg;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port change file test started");

    task = thr->task;
    task->thread = thr;

    name = (nxt_file_name_t *) "port change file test";

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    if (nxt_slow_path(rt == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    rt->mem_pool = mp;

    /*
     * Two slots.  Slot 0 stays unopened, so a redirect onto it fails, and
     * the accepted case uses slot 1: a successful redirect of slot 0 makes
     * the handler point stderr at the new file, which would follow the test
     * process out of this function.
     */
    rt->log_files = nxt_list_create(mp, 2, sizeof(nxt_file_t));
    if (nxt_slow_path(rt->log_files == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    log_file = nxt_list_add(rt->log_files);
    if (nxt_slow_path(log_file == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    nxt_memzero(log_file, sizeof(nxt_file_t));

    /*
     * Never opened, so redirecting onto it fails -- and the failure path
     * logs the name, which therefore cannot be left NULL.
     */
    log_file->name = name;
    log_file->fd = NXT_FILE_INVALID;

    log_file = nxt_list_add(rt->log_files);
    if (nxt_slow_path(log_file == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    nxt_memzero(log_file, sizeof(nxt_file_t));

    log_file->name = name;
    log_file->fd = open("/dev/null", O_WRONLY);
    if (nxt_slow_path(log_file->fd == -1)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    saved_rt = thr->runtime;
    thr->runtime = rt;

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    /*
     * No payload at all.  The dispatcher always hands the handler a buffer,
     * but a NULL one costs a single test to rule out and would be a NULL
     * dereference otherwise.
     */
    msg.buf = NULL;

    ret = nxt_port_change_file_test_reject(thr, task, &msg, 1, "no payload");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * A buffer shorter than the slot it is supposed to carry: the sender
     * declares the size, so a zero-length payload arrives the same way a
     * full one does, and reading the slot out of it reads past its end.
     */
    b = nxt_buf_mem_alloc(mp, sizeof(nxt_uint_t), 0);
    if (nxt_slow_path(b == NULL)) {
        ret = NXT_ERROR;
        goto done;
    }

    msg.buf = b;

    ret = nxt_port_change_file_test_reject(thr, task, &msg, 1,
                                           "short payload");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * A slot past the end of rt->log_files.  nxt_list_elt() returns NULL
     * for it, and the handler used to dereference that straight away.
     */
    slot = 4242;
    b->mem.free = nxt_cpymem(b->mem.free, &slot, sizeof(nxt_uint_t));

    ret = nxt_port_change_file_test_reject(thr, task, &msg, 1,
                                           "out of range slot");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /* An existing slot, but nothing to redirect it to. */
    b->mem.free = b->mem.pos;
    slot = 1;
    b->mem.free = nxt_cpymem(b->mem.free, &slot, sizeof(nxt_uint_t));

    ret = nxt_port_change_file_test_reject(thr, task, &msg, 0,
                                           "slot without a descriptor");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * A slot whose file was never opened: dup2() onto NXT_FILE_INVALID
     * fails, and nxt_file_redirect() used to return without closing the
     * descriptor it had been handed -- the caller does not close it on the
     * success path, so nobody did.
     */
    b->mem.free = b->mem.pos;
    slot = 0;
    b->mem.free = nxt_cpymem(b->mem.free, &slot, sizeof(nxt_uint_t));

    ret = nxt_port_change_file_test_reject(thr, task, &msg, 1,
                                           "slot that cannot be redirected");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    b->mem.free = b->mem.pos;
    slot = 1;
    b->mem.free = nxt_cpymem(b->mem.free, &slot, sizeof(nxt_uint_t));

    ret = nxt_port_change_file_test_accept(thr, task, &msg, log_file,
                                           "redirected slot");

done:

    thr->runtime = saved_rt;

    if (log_file->fd != -1) {
        (void) close(log_file->fd);
    }

    nxt_mp_destroy(mp);

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_thread_time_update(thr);
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port change file test passed");
    }

    return ret;
}
