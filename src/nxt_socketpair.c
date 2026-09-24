
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_socket_msg.h>

/*
 * SOCK_SEQPACKET protocol is supported for AF_UNIX in Solaris 8 X/Open
 * sockets, Linux 2.6.4, FreeBSD 9.0, NetBSD 6.0, and OpenBSD 5.0.
 */

/* SOCK_SEQPACKET is disabled to test SOCK_DGRAM on all platforms. */
#if (0 || NXT_HAVE_AF_UNIX_SOCK_SEQPACKET)
#define NXT_UNIX_SOCKET  SOCK_SEQPACKET
#else
#define NXT_UNIX_SOCKET  SOCK_DGRAM
#endif


nxt_int_t
nxt_socketpair_create(nxt_task_t *task, nxt_socket_t *pair)
{
    if (nxt_slow_path(socketpair(AF_UNIX, NXT_UNIX_SOCKET, 0, pair) != 0)) {
        nxt_alert(task, "socketpair() failed %E", nxt_errno);
        return NXT_ERROR;
    }

    nxt_debug(task, "socketpair(): %d:%d", pair[0], pair[1]);

    if (nxt_slow_path(nxt_socket_nonblocking(task, pair[0]) != NXT_OK)) {
        goto fail;
    }

    if (nxt_slow_path(fcntl(pair[0], F_SETFD, FD_CLOEXEC) == -1)) {
        goto fail;
    }

    if (nxt_slow_path(nxt_socket_nonblocking(task, pair[1]) != NXT_OK)) {
        goto fail;
    }

    if (nxt_slow_path(fcntl(pair[1], F_SETFD, FD_CLOEXEC) == -1)) {
        goto fail;
    }

#if NXT_HAVE_SOCKOPT_SO_PASSCRED
    int  enable_creds = 1;

    if (nxt_slow_path(setsockopt(pair[0], SOL_SOCKET, SO_PASSCRED,
                      &enable_creds, sizeof(enable_creds)) == -1))
    {
        nxt_alert(task, "failed to set SO_PASSCRED %E", nxt_errno);
        goto fail;
    }

    if (nxt_slow_path(setsockopt(pair[1], SOL_SOCKET, SO_PASSCRED,
                      &enable_creds, sizeof(enable_creds)) == -1))
    {
        nxt_alert(task, "failed to set SO_PASSCRED %E", nxt_errno);
        goto fail;
    }
#endif

    return NXT_OK;

fail:

    nxt_socketpair_close(task, pair);

    return NXT_ERROR;
}


void
nxt_socketpair_close(nxt_task_t *task, nxt_socket_t *pair)
{
    nxt_socket_close(task, pair[0]);
    nxt_socket_close(task, pair[1]);
}


#if (NXT_TESTS)
static nxt_err_t   nxt_socketpair_test_send_err;
static nxt_uint_t  nxt_socketpair_test_send_err_left;


/*
 * Make the next "n" calls of sendmsg() in nxt_socketpair_send() fail with
 * "err" without running it.  The call after that runs the real sendmsg().
 * Two is what expresses "the retry fails too", and zero injects nothing.
 * src/test/nxt_port_fail_test.c uses it for the errors a socketpair between
 * two live processes will not produce on demand.
 */

void
nxt_socketpair_test_send_fail(nxt_err_t err, nxt_uint_t n)
{
    nxt_socketpair_test_send_err = err;
    nxt_socketpair_test_send_err_left = (err != 0) ? n : 0;
}

#endif


/*
 * Set around a send to a peer that may have exited: EPIPE, ECONNRESET and
 * the like are then logged at nxt_socket_error_level(), not as alerts.
 */
nxt_bool_t  nxt_socketpair_peer_may_be_gone;


ssize_t
nxt_socketpair_send(nxt_fd_event_t *ev, nxt_fd_t *fd, nxt_iobuf_t *iob,
    nxt_uint_t niob)
{
    ssize_t         n;
    nxt_err_t       err;
    nxt_send_oob_t  oob;

    nxt_socket_msg_oob_init(&oob, fd);

    for ( ;; ) {
        n = -1;
        err = 0;

#if (NXT_TESTS)
        if (nxt_socketpair_test_send_err_left > 0) {
            err = nxt_socketpair_test_send_err;

            nxt_socketpair_test_send_err_left--;

            if (nxt_socketpair_test_send_err_left == 0) {
                nxt_socketpair_test_send_err = 0;
            }
        }
#endif

        if (nxt_fast_path(err == 0)) {
            n = nxt_sendmsg(ev->fd, iob, niob, &oob);

            err = (n == -1) ? nxt_socket_errno : 0;
        }

        nxt_debug(ev->task, "sendmsg(%d, %FD, %FD, %ui): %z", ev->fd, fd[0],
                  fd[1], niob, n);

        if (n > 0) {
            /*
             * Everywhere else in the tree a non-zero socket.error means the
             * socket is broken (nxt_conn_write.c, nxt_conn_read.c,
             * nxt_conn_close.c), and nxt_conn_io_write() stores 0 on a
             * successful write for that reason.  Keep the field meaning the
             * same thing here: the errno recorded below is only for the
             * NXT_AGAIN that follows it.
             */
            ev->error = 0;

            return n;
        }

        /* n == -1 */

        switch (err) {

        case NXT_EAGAIN:
            nxt_debug(ev->task, "sendmsg(%d) not ready", ev->fd);
            break;

        /*
         * Returned (at least on OSX) when trying to send many small messages.
         *
         * ENOMEM is the same condition by another name: the kernel could not
         * allocate for this call.  Linux answers it when the SCM_RIGHTS
         * copy of a descriptor-carrying message cannot be allocated, and
         * nothing of the message has gone out.  The socket and the peer are
         * fine, so the caller must keep the message and try again, as it
         * does after ENOBUFS.  The "default" arm below is for errors the
         * socket will not recover from; answering NXT_ERROR here made a
         * caller drop a message a live peer was still owed (#392).
         */
        case NXT_ENOBUFS:
        case NXT_ENOMEM:
            nxt_debug(ev->task, "sendmsg(%d) cannot allocate %E", ev->fd, err);
            break;

        case NXT_EINTR:
            nxt_debug(ev->task, "sendmsg(%d) interrupted", ev->fd);
            continue;

        default:
            nxt_log(ev->task, nxt_socketpair_peer_may_be_gone
                              ? nxt_socket_error_level(err) : NXT_LOG_ALERT,
                    "sendmsg(%d, %FD, %FD, %ui) failed %E",
                    ev->fd, fd[0], fd[1], niob, err);

            /*
             * The one exit where the socket really is broken, so it is the
             * one that must leave the errno behind: see the note above on
             * what a non-zero socket.error means.
             */
            ev->error = err;

            return NXT_ERROR;
        }

        ev->error = err;
        ev->write_ready = 0;

        return NXT_AGAIN;
    }
}


ssize_t
nxt_socketpair_recv(nxt_fd_event_t *ev, nxt_iobuf_t *iob, nxt_uint_t niob,
    void *oob)
{
    ssize_t    n;
    nxt_err_t  err;

    for ( ;; ) {
        n = nxt_recvmsg(ev->fd, iob, niob, oob);

        err = (n == -1) ? nxt_socket_errno : 0;

        nxt_debug(ev->task, "recvmsg(%d, %ui, %uz): %z",
                  ev->fd, niob, ((nxt_recv_oob_t *) oob)->size, n);

        if (n > 0) {
            return n;
        }

        if (n == 0) {
            ev->closed = 1;
            ev->read_ready = 0;

            return n;
        }

        /* n == -1 */

        switch (err) {

        case NXT_EAGAIN:
            nxt_debug(ev->task, "recvmsg(%d) not ready", ev->fd);
            ev->read_ready = 0;

            return NXT_AGAIN;

        case NXT_EINTR:
            nxt_debug(ev->task, "recvmsg(%d) interrupted", ev->fd);
            continue;

        default:
            nxt_alert(ev->task, "recvmsg(%d, %ui) failed %E",
                      ev->fd, niob, err);

            return NXT_ERROR;
        }
    }
}
