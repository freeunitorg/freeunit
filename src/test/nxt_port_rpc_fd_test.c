/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for descriptor ownership in nxt_port_rpc_handler()
 * (src/nxt_port_rpc.c).
 *
 * A stream that has no registered handler falls through nxt_port_rpc_handler()
 * without running one, and until the fix nothing closed the descriptors the
 * message carried.  The stream number is a value off the wire -- any peer can
 * name a stream this process never registered -- so a compromised application
 * could exhaust the router's descriptor table one such message at a time.
 *
 * nxt_port_read_msg_process() now also closes what a dispatch leaves behind,
 * which covers this path a second time; the close this test pins keeps the
 * reject path correct on its own terms.
 *
 * This lives beside nxt_port_fd_test.c rather than inside it: that test
 * drives nxt_port_new_port_handler(), nxt_port_mmap_handler() and the
 * dispatcher itself, all of which need a port hash and NEW_PORT-shaped
 * message bodies.  This one only needs a port with an empty rpc_streams
 * hash and a stream number nothing has registered -- the "find" and
 * "delete" arms of nxt_port_rpc_handler() both take that path with a
 * plain zeroed message.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include "nxt_tests.h"

#include <fcntl.h>


static void
nxt_port_rpc_fd_test_close(nxt_fd_t fd)
{
    if (fd != -1 && nxt_test_fd_is_open(fd)) {
        (void) close(fd);
    }
}


/*
 * Runs nxt_port_rpc_handler() over an unregistered stream and checks that
 * every real descriptor the message carried was closed, and that the slots
 * were cleared the way a handler that consumes them would leave them.
 * "last" selects between the two ways an unregistered stream is looked up:
 * nxt_lvlhsh_delete() when the message is the last for its stream,
 * nxt_lvlhsh_find() otherwise -- both reach the same early return.
 */
static nxt_int_t
nxt_port_rpc_fd_test_unregistered(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_t *port, uint32_t stream, uint8_t last, nxt_bool_t second_fd,
    const char *name)
{
    int                   pipe_fds[2];
    nxt_fd_t              fd0, fd1;
    nxt_port_recv_msg_t   msg;

    if (nxt_slow_path(pipe(pipe_fds) != 0)) {
        nxt_log_alert(thr->log, "port rpc fd test failed to create a pipe");
        return NXT_ERROR;
    }

    fd0 = pipe_fds[0];
    fd1 = second_fd ? pipe_fds[1] : -1;

    if (!second_fd) {
        /* Not needed for this case; close the write end right away. */
        (void) close(pipe_fds[1]);
    }

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.port = port;
    msg.fd[0] = fd0;
    msg.fd[1] = fd1;
    msg.port_msg.stream = stream;
    msg.port_msg.last = last;
    msg.port_msg.type = _NXT_PORT_MSG_RPC_READY;

    nxt_port_rpc_handler(task, &msg);

    if (nxt_slow_path(nxt_test_fd_is_open(fd0))) {
        nxt_log_alert(thr->log, "port rpc fd test: %s leaked fd[0]", name);
        goto fail;
    }

    if (second_fd && nxt_slow_path(nxt_test_fd_is_open(fd1))) {
        nxt_log_alert(thr->log, "port rpc fd test: %s leaked fd[1]", name);
        goto fail;
    }

    if (nxt_slow_path(msg.fd[0] != -1 || msg.fd[1] != -1)) {
        nxt_log_alert(thr->log, "port rpc fd test: %s left fds in the "
                      "message", name);
        return NXT_ERROR;
    }

    return NXT_OK;

fail:

    /* Close only what is still open; a closed number may already be reused. */
    nxt_port_rpc_fd_test_close(fd0);
    nxt_port_rpc_fd_test_close(fd1);

    return NXT_ERROR;
}


nxt_int_t
nxt_port_rpc_fd_test(nxt_thread_t *thr)
{
    nxt_mp_t              *mp;
    nxt_int_t             ret;
    nxt_task_t            *task;
    nxt_port_t            *port;
    nxt_port_recv_msg_t   msg;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port rpc fd test started");

    task = thr->task;
    task->thread = thr;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    port = nxt_mp_zalloc(mp, sizeof(nxt_port_t));
    if (nxt_slow_path(port == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    /*
     * port->rpc_streams starts zeroed, which is exactly "no stream
     * registered" for nxt_lvlhsh: no insert has ever happened.
     */
    port->mem_pool = mp;

    /* The stream number itself is arbitrary: nothing ever registers it. */
    ret = nxt_port_rpc_fd_test_unregistered(thr, task, port, 42, 0, 0,
                                            "find, one fd");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_port_rpc_fd_test_unregistered(thr, task, port, 42, 0, 1,
                                            "find, two fds");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_port_rpc_fd_test_unregistered(thr, task, port, 43, 1, 0,
                                            "delete, one fd");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_port_rpc_fd_test_unregistered(thr, task, port, 43, 1, 1,
                                            "delete, two fds");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * A message with no descriptor attached (fd[0] == -1, as every real
     * receive path sets it when the peer sent none) must not be mistaken
     * for one that needs closing.
     */
    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.port = port;
    msg.fd[0] = -1;
    msg.fd[1] = -1;
    msg.port_msg.stream = 44;
    msg.port_msg.last = 0;
    msg.port_msg.type = _NXT_PORT_MSG_RPC_READY;

    nxt_port_rpc_handler(task, &msg);

    if (nxt_slow_path(msg.fd[0] != -1 || msg.fd[1] != -1)) {
        nxt_log_alert(thr->log, "port rpc fd test: a message without "
                      "descriptors was mishandled");
        ret = NXT_ERROR;
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_mp_destroy(mp);

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_thread_time_update(thr);
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port rpc fd test passed");
    }

    return ret;
}
