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
 *
 * A third leg covers the bound that ownership made necessary
 * (NXT_PORT_MAX_FD_MSGS, freeunitorg/freeunit#394).  A peer that stops
 * reading holds two of this process's descriptors per queued message, so
 * the number of descriptor-carrying entries is capped.  The leg fills a
 * port to the cap, checks that the next descriptor-carrying send is
 * refused with nothing consumed, that a send with no descriptor is still
 * accepted, and that nothing is leaked either way.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_queue.h>
#include <nxt_socket_msg.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>


static nxt_int_t nxt_port_queued_fd_test_owned(nxt_thread_t *thr);
static nxt_int_t nxt_port_queued_fd_test_reused(nxt_thread_t *thr);
static nxt_int_t nxt_port_queued_fd_test_bounded(nxt_thread_t *thr);
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
static nxt_uint_t nxt_port_queued_fd_test_open_fds(void);
static nxt_uint_t nxt_port_queued_fd_test_queued(nxt_port_t *port);
static void nxt_cdecl nxt_port_queued_fd_test_log(nxt_uint_t level,
    nxt_log_t *log, const char *fmt, ...);


static nxt_uint_t  nxt_port_queued_fd_test_completions;
static nxt_uint_t  nxt_port_queued_fd_test_alerts;
static nxt_log_t   *nxt_port_queued_fd_test_saved_log;


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

    if (nxt_port_queued_fd_test_bounded(thr) != NXT_OK) {
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


/*
 * The bound on descriptor-carrying entries.
 *
 * The port is not writable, so every send queues, and each queued copy dups
 * what it was given: the open descriptor count rises by one per message and
 * is the measurement the leg makes.  Nothing here reaches inside the port to
 * read a counter -- the bound is observed the way a caller observes it, from
 * the answer to a send and from what the process still holds open.
 */

static nxt_int_t
nxt_port_queued_fd_test_bounded(nxt_thread_t *thr)
{
    nxt_mp_t               *mp;
    nxt_uint_t             i, base, filled;
    nxt_fd_t               fd, got, pair[2];
    nxt_buf_t              *buf;
    nxt_int_t              ret;
    nxt_log_t              log, *saved_log;
    struct stat            st;
    struct rlimit          rlmt;
    nxt_task_t             *task;
    nxt_port_t             *port;
    nxt_event_engine_t     engine, *saved_engine;
    nxt_event_interface_t  stub;

    task = thr->task;
    task->thread = thr;

    /*
     * The fill holds NXT_PORT_MAX_FD_MSGS duplicates open at once, and the
     * leg needs a few more of its own.  Under a lower RLIMIT_NOFILE a dup
     * fails before the bound is reached, which says nothing about the
     * bound, so the leg is skipped rather than failed.
     */

    if (getrlimit(RLIMIT_NOFILE, &rlmt) == 0
        && rlmt.rlim_cur != RLIM_INFINITY
        && rlmt.rlim_cur < nxt_port_queued_fd_test_open_fds()
                           + NXT_PORT_MAX_FD_MSGS + 16)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: bound leg skipped, "
                      "RLIMIT_NOFILE %d is too low for %d queued "
                      "descriptors", (int) rlmt.rlim_cur,
                      NXT_PORT_MAX_FD_MSGS);
        return NXT_OK;
    }

    saved_log = task->log;

    ret = NXT_ERROR;
    fd = -1;
    got = -1;
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

    nxt_port_write_enable(task, port);

    port->socket.write_ready = 0;
    port->socket.write = NXT_EVENT_INACTIVE;

    fd = open("/dev/null", O_RDONLY);
    if (fd == -1 || fstat(fd, &st) != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: failed to open /dev/null");
        goto done;
    }

    nxt_port_queued_fd_test_completions = 0;

    base = nxt_port_queued_fd_test_open_fds();

    /* Fill the queue to the bound.  Every one of these must be taken. */

    for (i = 0; i < NXT_PORT_MAX_FD_MSGS; i++) {
        if (nxt_port_socket_write2(task, port, NXT_PORT_MSG_NEW_PORT, fd, -1,
                                   i, 0, NULL)
            != NXT_OK)
        {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "port queued fd test: message %ui of %d was "
                          "refused below the bound", i, NXT_PORT_MAX_FD_MSGS);
            goto done;
        }
    }

    filled = nxt_port_queued_fd_test_queued(port);

    if (filled != NXT_PORT_MAX_FD_MSGS) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: %ui messages queued, expected %d",
                      filled, NXT_PORT_MAX_FD_MSGS);
        goto done;
    }

    if (nxt_port_queued_fd_test_open_fds() != base + NXT_PORT_MAX_FD_MSGS) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: %ui descriptors open after the "
                      "fill, expected %ui",
                      nxt_port_queued_fd_test_open_fds(),
                      base + NXT_PORT_MAX_FD_MSGS);
        goto done;
    }

    /*
     * One more, with a payload.  It must be refused, and refused whole: the
     * descriptor is still the caller's and open, the buffer is still the
     * caller's and uncompleted, and the queue is as it was.
     */

    buf = nxt_buf_mem_alloc(mp, 1, 0);
    if (nxt_slow_path(buf == NULL)) {
        goto done;
    }

    buf->completion_handler = nxt_port_queued_fd_test_completion;

    /*
     * The refusals below are counted by the alerts they log: a stalled peer
     * keeps the port at the bound, so only the first one may log.
     */

    log = *saved_log;
    log.handler = nxt_port_queued_fd_test_log;

    nxt_port_queued_fd_test_saved_log = saved_log;
    nxt_port_queued_fd_test_alerts = 0;

    task->log = &log;

    if (nxt_port_socket_write2(task, port, NXT_PORT_MSG_NEW_PORT, fd, -1,
                               NXT_PORT_MAX_FD_MSGS, 0, buf)
        != NXT_ERROR)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: a message past the bound of %d "
                      "was accepted", NXT_PORT_MAX_FD_MSGS);
        goto done;
    }

    if (nxt_port_socket_write2(task, port, NXT_PORT_MSG_NEW_PORT, fd, -1,
                               NXT_PORT_MAX_FD_MSGS, 0, NULL)
        != NXT_ERROR)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: a second message past the bound "
                      "was accepted");
        goto done;
    }

    if (nxt_port_queued_fd_test_alerts != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: two refusals in a row logged %ui "
                      "alerts, expected 1", nxt_port_queued_fd_test_alerts);
        goto done;
    }

    nxt_port_queued_fd_test_drain_wq(&engine.fast_work_queue);

    if (nxt_port_queued_fd_test_completions != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the refused message completed "
                      "%ui buffers, expected 0",
                      nxt_port_queued_fd_test_completions);
        goto done;
    }

    if (!nxt_test_fd_is_open(fd)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the refused message closed the "
                      "caller's descriptor");
        fd = -1;
        goto done;
    }

    if (nxt_port_queued_fd_test_queued(port) != NXT_PORT_MAX_FD_MSGS
        || nxt_port_queued_fd_test_open_fds() != base + NXT_PORT_MAX_FD_MSGS)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the refused message changed the "
                      "queue");
        goto done;
    }

    /*
     * A message with no descriptor is not what the bound is about, and is
     * still taken on the same full port.
     */

    if (nxt_port_socket_write2(task, port, NXT_PORT_MSG_DATA, -1, -1,
                               NXT_PORT_MAX_FD_MSGS + 1, 0, buf)
        != NXT_OK)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: a message with no descriptor was "
                      "refused on a port at the bound");
        goto done;
    }

    if (nxt_port_queued_fd_test_queued(port) != NXT_PORT_MAX_FD_MSGS + 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the message with no descriptor "
                      "was not queued");
        goto done;
    }

    /*
     * Cancelling a queued message releases its place under the bound as
     * well: the port takes one more descriptor, and refusing the one after
     * that is a new stall, so it logs again.
     */

    if (nxt_port_socket_cancel(task, port, NXT_PORT_MSG_NEW_PORT, 0, 0, NULL)
        != NXT_PORT_MSG_CANCELLED)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the first queued message could "
                      "not be cancelled");
        goto done;
    }

    nxt_port_queued_fd_test_drain_wq(&engine.fast_work_queue);

    if (nxt_port_queued_fd_test_open_fds() != base + NXT_PORT_MAX_FD_MSGS - 1)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: %ui descriptors open after the "
                      "cancel, expected %ui",
                      nxt_port_queued_fd_test_open_fds(),
                      base + NXT_PORT_MAX_FD_MSGS - 1);
        goto done;
    }

    if (nxt_port_socket_write2(task, port, NXT_PORT_MSG_NEW_PORT, fd, -1,
                               NXT_PORT_MAX_FD_MSGS + 2, 0, NULL)
        != NXT_OK)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: a cancelled message did not "
                      "release its place under the bound");
        goto done;
    }

    if (nxt_port_socket_write2(task, port, NXT_PORT_MSG_NEW_PORT, fd, -1,
                               NXT_PORT_MAX_FD_MSGS + 3, 0, NULL)
        != NXT_ERROR
        || nxt_port_queued_fd_test_alerts != 2)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the port at the bound again did "
                      "not refuse and log once more (%ui alerts)",
                      nxt_port_queued_fd_test_alerts);
        goto done;
    }

    task->log = saved_log;

    /*
     * Draining is what releases the bound.  The peer gets what was queued
     * below it, and the duplicates go with the messages: the descriptor
     * count comes back to where it started.
     */

    port->socket.write_ready = 1;

    port->socket.write_handler(task, &port->socket, NULL);

    nxt_port_queued_fd_test_drain_wq(&engine.fast_work_queue);

    got = nxt_port_queued_fd_test_recv_fd(pair[0]);

    if (got == -1 || !nxt_port_queued_fd_test_same_file(got, &st)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the peer did not receive the "
                      "first message queued below the bound");
        goto done;
    }

    nxt_fd_close(got);
    got = -1;

    /* Whatever the socket would not take is dropped the ordinary way. */

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_port_test_run_error_handler(task, port);
        nxt_port_queued_fd_test_drain_wq(&engine.fast_work_queue);
    }

    while ((got = nxt_port_queued_fd_test_recv_fd(pair[0])) != -1) {
        nxt_fd_close(got);
    }

    if (nxt_port_queued_fd_test_open_fds() != base) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: %ui descriptors open after the "
                      "queue drained, expected the %ui it started with",
                      nxt_port_queued_fd_test_open_fds(), base);
        goto done;
    }

    /* And the port takes descriptor-carrying messages again. */

    if (nxt_port_socket_write2(task, port, NXT_PORT_MSG_NEW_PORT, fd, -1,
                               0, 0, NULL)
        != NXT_OK)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port queued fd test: the drained port still refuses a "
                      "descriptor");
        goto done;
    }

    ret = NXT_OK;

done:

    task->log = saved_log;

    if (fd != -1 && nxt_test_fd_is_open(fd)) {
        nxt_fd_close(fd);
    }

    if (got != -1) {
        nxt_fd_close(got);
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


/*
 * How many descriptors this process holds open.  Counted by probing numbers
 * rather than by reading /proc, which is not there on every platform the C
 * suite builds on.
 */

static nxt_uint_t
nxt_port_queued_fd_test_open_fds(void)
{
    nxt_fd_t        fd;
    nxt_uint_t      n, limit;
    struct rlimit   rlmt;

    limit = 4096;

    if (getrlimit(RLIMIT_NOFILE, &rlmt) == 0
        && rlmt.rlim_cur != RLIM_INFINITY
        && rlmt.rlim_cur < limit)
    {
        limit = rlmt.rlim_cur;
    }

    n = 0;

    for (fd = 0; fd < (nxt_fd_t) limit; fd++) {
        if (nxt_test_fd_is_open(fd)) {
            n++;
        }
    }

    return n;
}


/*
 * Counts the alerts and passes every line on to the log the leg replaced,
 * so the refusal still shows in the output.
 */

static void nxt_cdecl
nxt_port_queued_fd_test_log(nxt_uint_t level, nxt_log_t *log,
    const char *fmt, ...)
{
    u_char     *p;
    va_list    args;
    nxt_log_t  *saved;
    u_char     msg[NXT_MAX_ERROR_STR];

    if (level == NXT_LOG_ALERT) {
        nxt_port_queued_fd_test_alerts++;
    }

    va_start(args, fmt);
    p = nxt_vsprintf(msg, msg + NXT_MAX_ERROR_STR - 1, fmt, args);
    va_end(args);

    *p = '\0';

    saved = nxt_port_queued_fd_test_saved_log;

    saved->handler(level, saved, "%s", msg);
}


static nxt_uint_t
nxt_port_queued_fd_test_queued(nxt_port_t *port)
{
    nxt_uint_t        n;
    nxt_queue_link_t  *lnk;

    n = 0;

    for (lnk = nxt_queue_first(&port->messages);
         lnk != nxt_queue_tail(&port->messages);
         lnk = nxt_queue_next(lnk))
    {
        n++;
    }

    return n;
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
