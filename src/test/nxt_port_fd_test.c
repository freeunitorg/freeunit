/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for descriptor ownership in nxt_port_new_port_handler()
 * and nxt_port_mmap_handler() (src/nxt_port.c).
 *
 * A port message can carry two descriptors whatever its type normally uses
 * -- nxt_socket_msg_oob_get() accepts an SCM_RIGHTS payload of one or two --
 * and the dispatcher does not reclaim them once the handler returns.  Every
 * handler therefore has to release what it does not consume, and clear the
 * slot of what it does.  Both handlers here ignored fd[1] entirely, and
 * nxt_port_mmap_handler() returned from its "invalid fd" guard without
 * closing anything at all, so a peer attaching descriptors to forged
 * messages could exhaust the descriptor table of the router or, through the
 * NEW_PORT path, of the main process.
 *
 * This lives beside nxt_port_ready_test.c rather than inside it: that test
 * builds a fixture for a single handler and walks its reject paths, while
 * these two handlers need a port hash and a message body instead.
 *
 * The one path this cannot reach is nxt_runtime_process_port_create()
 * returning NULL, which needs an allocation failure the test harness has no
 * way to inject.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include "nxt_tests.h"

#include <fcntl.h>


static nxt_bool_t
nxt_port_fd_test_is_open(nxt_fd_t fd)
{
    return fcntl(fd, F_GETFD) != -1;
}


static void
nxt_port_fd_test_close(nxt_fd_t fd)
{
    if (fd != -1 && nxt_port_fd_test_is_open(fd)) {
        (void) close(fd);
    }
}


/*
 * Attach two descriptors to the message, run the handler, and expect it to
 * have released both: neither of these handlers keeps a descriptor of its
 * own beyond the call.
 */
static nxt_int_t
nxt_port_fd_test_closed(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_recv_msg_t *msg,
    void (*handler)(nxt_task_t *task, nxt_port_recv_msg_t *msg),
    nxt_bool_t first_fd, const char *name)
{
    nxt_fd_t  fd0, fd1;

    fd0 = first_fd ? open("/dev/null", O_RDONLY) : -1;
    fd1 = open("/dev/null", O_RDONLY);

    if ((first_fd && fd0 == -1) || fd1 == -1) {
        nxt_log_alert(thr->log, "port fd test failed to open /dev/null");
        goto fail;
    }

    msg->fd[0] = fd0;
    msg->fd[1] = fd1;

    handler(task, msg);

    if ((first_fd && nxt_port_fd_test_is_open(fd0))
        || nxt_port_fd_test_is_open(fd1))
    {
        nxt_log_alert(thr->log, "port fd test: %s leaked a descriptor", name);
        goto fail;   /* closes only what is still open */
    }

    if (msg->fd[0] != -1 || msg->fd[1] != -1) {
        nxt_log_alert(thr->log, "port fd test: %s left fds in the message",
                      name);
        return NXT_ERROR;
    }

    return NXT_OK;

fail:

    /*
     * Close only what the handler left open: a descriptor it consumed is
     * gone already, and closing the number again could reach an unrelated
     * descriptor that has reused it.
     */
    nxt_port_fd_test_close(fd0);
    nxt_port_fd_test_close(fd1);

    return NXT_ERROR;
}


nxt_int_t
nxt_port_fd_test(nxt_thread_t *thr)
{
    nxt_mp_t                 *mp;
    nxt_buf_t                *b;
    nxt_fd_t                 fd0, fd1;
    nxt_task_t               *task;
    nxt_int_t                ret;
    nxt_port_t               *port, *existing;
    nxt_runtime_t            *rt, *saved_rt;
    nxt_port_recv_msg_t      msg;
    nxt_event_engine_t       *engine, *saved_engine;
    nxt_port_msg_new_port_t  new_port_msg;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port fd test started");

    task = thr->task;
    task->thread = thr;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    engine = nxt_mp_zalloc(mp, sizeof(nxt_event_engine_t));
    b = nxt_mp_zalloc(mp, sizeof(nxt_buf_t));

    if (nxt_slow_path(rt == NULL || engine == NULL || b == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    rt->mem_pool = mp;

    if (nxt_slow_path(nxt_thread_mutex_create(&rt->processes_mutex) != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    saved_rt = thr->runtime;
    saved_engine = thr->engine;

    thr->runtime = rt;

    /*
     * nxt_port_write_enable() takes the address of a work queue inside the
     * engine, so a zeroed one is enough to reach the end of the handler.
     */
    thr->engine = engine;

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    /*
     * The mmap handler refuses a message without a segment descriptor.  The
     * refusal used to return without closing the second descriptor, which
     * the sender is free to attach regardless of the message type.
     */
    ret = nxt_port_fd_test_closed(thr, task, &msg, nxt_port_mmap_handler, 0,
                                  "mmap without a segment fd");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * A segment announced by a process the runtime does not know: the
     * handler releases the segment descriptor on that path but used to keep
     * the second one.
     */
    msg.port_msg.pid = nxt_pid + 1;

    ret = nxt_port_fd_test_closed(thr, task, &msg, nxt_port_mmap_handler, 1,
                                  "mmap from an unknown process");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * NEW_PORT naming a port that already exists.  The socket was already
     * refused there; the queue descriptor has to be refused with it, or a
     * caller that maps fd[1] would re-point a live port's queue.
     */
    nxt_memzero(&new_port_msg, sizeof(nxt_port_msg_new_port_t));

    new_port_msg.pid = nxt_pid + 2;
    new_port_msg.id = 7;
    new_port_msg.type = NXT_PROCESS_APP;

    b->mem.pos = (u_char *) &new_port_msg;
    msg.buf = b;

    existing = nxt_runtime_process_port_create(task, rt, new_port_msg.pid,
                                               new_port_msg.id,
                                               new_port_msg.type);
    if (nxt_slow_path(existing == NULL)) {
        ret = NXT_ERROR;
        goto done;
    }

    existing->pair[0] = -1;
    existing->pair[1] = -1;
    existing->socket.fd = -1;

    msg.u.new_port = NULL;

    ret = nxt_port_fd_test_closed(thr, task, &msg, nxt_port_new_port_handler,
                                  1, "new port that already exists");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    if (nxt_slow_path(msg.u.new_port != existing)) {
        nxt_log_alert(thr->log, "port fd test: an existing port was not "
                      "reported back to the caller");
        ret = NXT_ERROR;
        goto done;
    }

    /*
     * The one path that keeps a descriptor: a port is created, it adopts
     * the socket, and the queue descriptor is deliberately left in the
     * message for the caller to map or discard.  Pinned here because every
     * caller now treats "fd[1] still set" as "this one is mine".
     */
    new_port_msg.id = 8;

    fd0 = open("/dev/null", O_RDONLY);
    fd1 = open("/dev/null", O_RDONLY);

    if (nxt_slow_path(fd0 == -1 || fd1 == -1)) {
        nxt_log_alert(thr->log, "port fd test failed to open /dev/null");
        nxt_port_fd_test_close(fd0);
        nxt_port_fd_test_close(fd1);
        ret = NXT_ERROR;
        goto done;
    }

    msg.fd[0] = fd0;
    msg.fd[1] = fd1;
    msg.u.new_port = NULL;

    nxt_port_new_port_handler(task, &msg);

    port = msg.u.new_port;

    if (nxt_slow_path(port == NULL || port == existing)) {
        nxt_log_alert(thr->log, "port fd test: a new port was not created");
        nxt_port_fd_test_close(fd0);
        nxt_port_fd_test_close(fd1);
        ret = NXT_ERROR;
        goto done;
    }

    if (nxt_slow_path(msg.fd[0] != -1 || port->pair[1] != fd0)) {
        nxt_log_alert(thr->log, "port fd test: the new port did not take the "
                      "socket over");
        nxt_port_fd_test_close(fd1);
        ret = NXT_ERROR;
        goto done;
    }

    if (nxt_slow_path(msg.fd[1] != fd1 || !nxt_port_fd_test_is_open(fd1))) {
        nxt_log_alert(thr->log, "port fd test: the queue descriptor was not "
                      "left to the caller");
        ret = NXT_ERROR;
        goto done;
    }

    nxt_port_fd_test_close(fd1);

    ret = NXT_OK;

done:

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_thread_time_update(thr);
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port fd test passed");
    }

    return ret;
}
