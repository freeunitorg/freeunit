/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for descriptor ownership of a queued port message
 * (nxt_port_msg_alloc() in src/nxt_port_socket.c, #388).
 *
 * nxt_port_send_port() (src/nxt_port.c) hands another port's pair[1] and
 * queue_fd to nxt_port_socket_write2() as bare numbers, without
 * NXT_PORT_MSG_CLOSE_FD: the message borrows them.  When the socket is not
 * writable the message is copied to the heap and queued, and the sendmsg()
 * that puts the descriptors into SCM_RIGHTS runs later.  If the owner closes
 * them first, the deferred send names a closed number -- or, once the kernel
 * has reused it, an unrelated descriptor of this process.
 *
 * Two legs.  The first inspects the queued copy: it must own duplicates,
 * not the caller's numbers, and dropping it through the error handler must
 * close exactly those duplicates.  The second drives the race the issue
 * describes end to end over a real socketpair: queue, close the originals,
 * reopen something else at the same number, then let the write handler
 * send.  The peer must receive the descriptor that was queued, not the
 * one that took its number.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_queue.h>
#include <nxt_socket_msg.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>


static nxt_int_t nxt_port_queued_fd_test_owned(nxt_thread_t *thr);
static nxt_int_t nxt_port_queued_fd_test_reused(nxt_thread_t *thr);
static nxt_port_t *nxt_port_queued_fd_test_port(nxt_task_t *task,
    nxt_event_engine_t *engine);
static void nxt_port_queued_fd_test_stub(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_port_queued_fd_test_completion(nxt_task_t *task, void *obj,
    void *data);
static void nxt_port_queued_fd_test_drain_wq(nxt_work_queue_t *wq);
static nxt_bool_t nxt_port_queued_fd_test_same_file(nxt_fd_t a,
    const struct stat *b);
static nxt_fd_t nxt_port_queued_fd_test_recv_fd(nxt_fd_t sock);


static nxt_uint_t  nxt_port_queued_fd_test_completions;


nxt_int_t
nxt_port_queued_fd_test(nxt_thread_t *thr)
{
    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port queued fd test started");

    if (nxt_port_queued_fd_test_owned(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_queued_fd_test_reused(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port queued fd test passed");

    return NXT_OK;
}


static void
nxt_port_queued_fd_test_stub(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    /* The fixture engine polls nothing. */
}


/*
 * A port on the caller's engine with a stub event interface, so that the
 * write path's re-arm and block calls land on nothing.  socket.write_ready
 * is left clear: nxt_port_msg_chk_insert() then queues every message.
 */

static nxt_port_t *
nxt_port_queued_fd_test_port(nxt_task_t *task, nxt_event_engine_t *engine)
{
    nxt_port_t  *port;

    port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_MAIN);
    if (nxt_slow_path(port == NULL)) {
        return NULL;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;
    port->socket.task = task;
    port->socket.log = task->log;
    port->socket.write_ready = 0;
    port->socket.write = NXT_EVENT_INACTIVE;
    port->engine = engine;
    port->max_size = 1024;
    port->max_share = 1024;

    port->socket.write_work_queue = &engine->fast_work_queue;

    return port;
}


static nxt_int_t
nxt_port_queued_fd_test_owned(nxt_thread_t *thr)
{
    nxt_mp_t               *mp;
    nxt_fd_t               fd0, fd1;
    nxt_buf_t              *buf;
    nxt_int_t              ret;
    nxt_task_t             *task;
    nxt_port_t             *port;
    struct stat            st0, st1;
    nxt_event_engine_t     engine, *saved_engine;
    nxt_port_send_msg_t    *msg;
    nxt_event_interface_t  stub;

    task = thr->task;
    task->thread = thr;

    ret = NXT_ERROR;
    fd0 = -1;
    fd1 = -1;

    nxt_memzero(&engine, sizeof(engine));
    nxt_memzero(&stub, sizeof(stub));

    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    stub.enable_write = nxt_port_queued_fd_test_stub;
    stub.block_write = nxt_port_queued_fd_test_stub;
    engine.event = stub;

    saved_engine = thr->engine;
    thr->engine = &engine;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        thr->engine = saved_engine;
        return NXT_ERROR;
    }

    port = nxt_port_queued_fd_test_port(task, &engine);
    if (nxt_slow_path(port == NULL)) {
        nxt_mp_destroy(mp);
        thr->engine = saved_engine;
        return NXT_ERROR;
    }

    fd0 = open("/dev/null", O_RDONLY);
    fd1 = open("/dev/zero", O_RDONLY);

    if (fd0 == -1 || fd1 == -1 || fstat(fd0, &st0) != 0
        || fstat(fd1, &st1) != 0)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: failed to open /dev/null");
        goto done;
    }

    buf = nxt_buf_mem_alloc(mp, 1, 0);
    if (nxt_slow_path(buf == NULL)) {
        goto done;
    }

    buf->completion_handler = nxt_port_queued_fd_test_completion;
    nxt_port_queued_fd_test_completions = 0;

    /* The shape of nxt_port_send_port(): two borrowed descriptors. */

    if (nxt_port_socket_write2(task, port, NXT_PORT_MSG_NEW_PORT, fd0, fd1,
                               0, 0, buf)
        != NXT_OK)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: write was not queued");
        goto done;
    }

    if (nxt_queue_is_empty(&port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: nothing queued");
        goto done;
    }

    msg = nxt_queue_link_data(nxt_queue_first(&port->messages),
                              nxt_port_send_msg_t, link);

    if (!msg->close_fd) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the queued message does not own "
                      "its descriptors");
        goto done;
    }

    if (msg->fd[0] == fd0 || msg->fd[1] == fd1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the queued message names the "
                      "caller's descriptors %FD %FD", msg->fd[0], msg->fd[1]);
        goto done;
    }

    if (!nxt_port_queued_fd_test_same_file(msg->fd[0], &st0)
        || !nxt_port_queued_fd_test_same_file(msg->fd[1], &st1))
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the queued duplicates name "
                      "other files");
        goto done;
    }

    /* The owner closes; the queued copy must be unaffected. */

    nxt_fd_close(fd0);
    nxt_fd_close(fd1);

    if (!nxt_test_fd_is_open(msg->fd[0]) || !nxt_test_fd_is_open(msg->fd[1])) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: closing the originals closed "
                      "the queued copy");
        fd0 = -1;
        fd1 = -1;
        goto done;
    }

    fd0 = msg->fd[0];
    fd1 = msg->fd[1];

    /* Drop the queue: the duplicates go with it, and nothing else does. */

    nxt_port_test_run_error_handler(task, port);

    nxt_port_queued_fd_test_drain_wq(&engine.fast_work_queue);

    if (nxt_test_fd_is_open(fd0) || nxt_test_fd_is_open(fd1)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: dropping the queue leaked the "
                      "duplicates");
        goto done;
    }

    fd0 = -1;
    fd1 = -1;

    if (nxt_port_queued_fd_test_completions != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: %ui completions, expected 1",
                      nxt_port_queued_fd_test_completions);
        goto done;
    }

    ret = NXT_OK;

done:

    if (fd0 != -1 && nxt_test_fd_is_open(fd0)) {
        nxt_fd_close(fd0);
    }

    if (fd1 != -1 && nxt_test_fd_is_open(fd1)) {
        nxt_fd_close(fd1);
    }

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_port_test_run_error_handler(task, port);
        nxt_port_queued_fd_test_drain_wq(&engine.fast_work_queue);
    }

    nxt_port_use(task, port, -1);
    nxt_mp_destroy(mp);

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    thr->engine = saved_engine;

    return ret;
}


/*
 * The race itself: the owner closes after the message was queued, the
 * number is taken by something else, then the send goes out.
 */

static nxt_int_t
nxt_port_queued_fd_test_reused(nxt_thread_t *thr)
{
    nxt_mp_t               *mp;
    nxt_fd_t               fd, decoy, got, pair[2];
    nxt_buf_t              *buf;
    nxt_int_t              ret;
    nxt_task_t             *task;
    nxt_port_t             *port;
    struct stat            st, dst;
    nxt_event_engine_t     engine, *saved_engine;
    nxt_event_interface_t  stub;

    task = thr->task;
    task->thread = thr;

    ret = NXT_ERROR;
    fd = -1;
    got = -1;
    decoy = -1;
    pair[0] = -1;
    pair[1] = -1;

    nxt_memzero(&engine, sizeof(engine));
    nxt_memzero(&stub, sizeof(stub));

    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    stub.enable_write = nxt_port_queued_fd_test_stub;
    stub.block_write = nxt_port_queued_fd_test_stub;
    engine.event = stub;

    saved_engine = thr->engine;
    thr->engine = &engine;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        thr->engine = saved_engine;
        return NXT_ERROR;
    }

    port = nxt_port_queued_fd_test_port(task, &engine);
    if (nxt_slow_path(port == NULL)) {
        nxt_mp_destroy(mp);
        thr->engine = saved_engine;
        return NXT_ERROR;
    }

    /* SOCK_DGRAM: the type src/nxt_socketpair.c selects. */

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: socketpair failed");
        goto done;
    }

    if (nxt_slow_path(fcntl(pair[1], F_SETFL, O_NONBLOCK) == -1)) {
        goto done;
    }

    port->pair[0] = pair[0];
    port->pair[1] = pair[1];

    /* Installs the real write and error handlers; then not writable yet. */

    nxt_port_write_enable(task, port);

    port->socket.write_ready = 0;
    port->socket.write = NXT_EVENT_INACTIVE;

    fd = open("/dev/null", O_RDONLY);
    if (fd == -1 || fstat(fd, &st) != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: failed to open /dev/null");
        goto done;
    }

    buf = nxt_buf_mem_alloc(mp, 1, 0);
    if (nxt_slow_path(buf == NULL)) {
        goto done;
    }

    buf->completion_handler = nxt_port_queued_fd_test_completion;
    nxt_port_queued_fd_test_completions = 0;

    /* write_ready is clear: queued, not sent. */

    if (nxt_port_socket_write2(task, port, NXT_PORT_MSG_NEW_PORT, fd, -1,
                               0, 0, buf)
        != NXT_OK
        || nxt_queue_is_empty(&port->messages))
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: write was not queued");
        goto done;
    }

    /*
     * The owner goes away, and the lowest free number -- the one just
     * closed -- is taken by an unrelated file before the queue drains.
     */

    nxt_fd_close(fd);

    decoy = open("/dev/zero", O_RDONLY);
    if (decoy == -1 || fstat(decoy, &dst) != 0) {
        fd = -1;
        goto done;
    }

    if (decoy != fd) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the closed number %FD was not "
                      "reused (%FD), cannot stage the race", fd, decoy);
        fd = -1;
        goto done;
    }

    fd = -1;

    /* Now the socket is writable and the queue drains. */

    port->socket.write_ready = 1;

    port->socket.write_handler(task, &port->socket, NULL);

    nxt_port_queued_fd_test_drain_wq(&engine.fast_work_queue);

    got = nxt_port_queued_fd_test_recv_fd(pair[0]);
    if (got == -1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the peer received no descriptor");
        goto done;
    }

    if (nxt_port_queued_fd_test_same_file(got, &dst)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the peer received the descriptor "
                      "that reused the number, not the one that was queued");
        goto done;
    }

    if (!nxt_port_queued_fd_test_same_file(got, &st)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the peer received an unexpected "
                      "descriptor");
        goto done;
    }

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the message was sent but stayed "
                      "queued");
        goto done;
    }

    if (nxt_port_queued_fd_test_completions != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: %ui completions, expected 1",
                      nxt_port_queued_fd_test_completions);
        goto done;
    }

    ret = NXT_OK;

done:

    if (fd != -1) {
        nxt_fd_close(fd);
    }

    if (got != -1) {
        nxt_fd_close(got);
    }

    if (decoy != -1) {
        nxt_fd_close(decoy);
    }

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_port_test_run_error_handler(task, port);
        nxt_port_queued_fd_test_drain_wq(&engine.fast_work_queue);
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    if (pair[0] != -1) {
        nxt_fd_close(pair[0]);
    }

    if (pair[1] != -1) {
        nxt_fd_close(pair[1]);
    }

    nxt_port_use(task, port, -1);
    nxt_mp_destroy(mp);

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    thr->engine = saved_engine;

    return ret;
}


static void
nxt_port_queued_fd_test_completion(nxt_task_t *task, void *obj, void *data)
{
    nxt_port_queued_fd_test_completions++;
}


static void
nxt_port_queued_fd_test_drain_wq(nxt_work_queue_t *wq)
{
    void                *obj, *data;
    nxt_task_t          *t;
    nxt_work_handler_t  handler;

    while (wq->head != NULL) {
        handler = nxt_work_queue_pop(wq, &t, &obj, &data);
        handler(t, obj, data);
    }
}


static nxt_bool_t
nxt_port_queued_fd_test_same_file(nxt_fd_t a, const struct stat *b)
{
    struct stat  st;

    if (a == -1 || fstat(a, &st) != 0) {
        return 0;
    }

    return st.st_dev == b->st_dev && st.st_ino == b->st_ino
           && st.st_rdev == b->st_rdev;
}


/* One datagram with its SCM_RIGHTS payload; -1 if none arrived. */

static nxt_fd_t
nxt_port_queued_fd_test_recv_fd(nxt_fd_t sock)
{
    ssize_t          n;
    nxt_fd_t         fd;
    struct iovec     iov;
    struct msghdr    mh;
    struct cmsghdr   *cm;
    u_char           payload[256];
    union {
        struct cmsghdr  align;
        u_char          space[CMSG_SPACE(sizeof(int) * 2)];
    } cbuf;

    iov.iov_base = payload;
    iov.iov_len = sizeof(payload);

    nxt_memzero(&mh, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = &cbuf;
    mh.msg_controllen = sizeof(cbuf);

    n = recvmsg(sock, &mh, MSG_DONTWAIT);
    if (n < (ssize_t) sizeof(nxt_port_msg_t)) {
        return -1;
    }

    fd = -1;

    for (cm = CMSG_FIRSTHDR(&mh); cm != NULL; cm = NXT_CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            nxt_memcpy(&fd, CMSG_DATA(cm), sizeof(int));
            break;
        }
    }

    return fd;
}
