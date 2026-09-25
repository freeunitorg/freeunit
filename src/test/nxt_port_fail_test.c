/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_rpc.h>
#include <nxt_event_engine.h>
#include <nxt_port_queue.h>
#include "nxt_tests.h"

#if (NXT_LINUX)
#include <dirent.h>
#endif


static nxt_port_t *nxt_port_fail_test_port(nxt_task_t *task);
static nxt_int_t nxt_port_fail_test_socket_write(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_inline_drop(nxt_thread_t *thr);
static void nxt_port_fail_test_enable_write(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static nxt_int_t nxt_port_fail_test_wakeup_errno(nxt_thread_t *thr);
#if (NXT_HAVE_EPOLL_EDGE)
static nxt_int_t nxt_port_fail_test_enqueue(nxt_task_t *task,
    nxt_port_t *port, nxt_mp_t *mp, nxt_err_t err, nxt_uint_t fails,
    nxt_bool_t no_memory, nxt_bool_t *queued);
#endif
static nxt_int_t nxt_port_fail_test_dead_peer(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_rpc_register(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_error_handler(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_mp_baseline(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_cross_engine_release(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_cross_engine_acquire(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_cross_engine_race(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_cross_engine_batch(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_cross_engine_rearm(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_caller_owns_buf(nxt_thread_t *thr);
static void nxt_port_fail_test_racer(void *data);
static nxt_int_t nxt_port_fail_test_queued(nxt_locked_work_queue_t *lwq,
    nxt_work_t *item);
static void nxt_port_fail_test_engine_signal(nxt_event_engine_t *engine,
    nxt_uint_t signo);
static void nxt_port_fail_test_released(nxt_task_t *task, void *obj,
    void *data);
static nxt_int_t nxt_port_fail_test_sender_pattern(nxt_task_t *task,
    nxt_port_t *port, nxt_mp_t *mp);
static void nxt_port_fail_test_mp_completion(nxt_task_t *task, void *obj,
    void *data);
static void nxt_port_fail_test_completion(nxt_task_t *task, void *obj,
    void *data);
static void nxt_port_fail_test_drain_wq(nxt_work_queue_t *wq);
static nxt_int_t nxt_port_fail_test_fd_count(void);


static nxt_uint_t  nxt_port_fail_test_completions;
static nxt_uint_t  nxt_port_fail_test_rearms;
static nxt_uint_t  nxt_port_fail_test_releases;
static nxt_uint_t  nxt_port_fail_test_signals;


nxt_int_t
nxt_port_fail_test(nxt_thread_t *thr)
{
    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port failure test started");

    if (nxt_port_fail_test_socket_write(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_inline_drop(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_dead_peer(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_wakeup_errno(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_rpc_register(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_error_handler(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_mp_baseline(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_cross_engine_release(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_cross_engine_acquire(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_cross_engine_race(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_cross_engine_batch(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_cross_engine_rearm(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_port_fail_test_caller_owns_buf(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port failure test passed");

    return NXT_OK;
}


static nxt_port_t *
nxt_port_fail_test_port(nxt_task_t *task)
{
    nxt_port_t  *port;

    port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_MAIN);

    if (nxt_slow_path(port == NULL)) {
        return NULL;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    return port;
}


static nxt_int_t
nxt_port_fail_test_socket_write(nxt_thread_t *thr)
{
    nxt_mp_t    *mp;
    nxt_fd_t    fd;
    nxt_buf_t   *buf;
    nxt_task_t  *task;
    nxt_port_t  *port;
    nxt_int_t   before, after_open, after_fail, after_close;

    task = thr->task;
    task->thread = thr;

    /*
     * Per Gemini's PR #57 review feedback: allocate the test buffer
     * from a transient mp rather than the stack — keeps the buf
     * lifetime tied to a heap object, so a future change that lets
     * the port layer access it asynchronously cannot UAF the stack
     * frame.  The mp is destroyed below on every exit path.
     */
    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    fd = -1;

    before = nxt_port_fail_test_fd_count();

    fd = open("/dev/null", O_RDONLY);
    if (fd == -1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test failed to open /dev/null");
        goto fail;
    }

    after_open = nxt_port_fail_test_fd_count();

    buf = nxt_buf_mem_alloc(mp, 1, 0);
    if (nxt_slow_path(buf == NULL)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test failed to allocate buf");
        goto fail_close_fd;
    }

    buf->completion_handler = nxt_port_fail_test_completion;

    nxt_port_fail_test_completions = 0;
    nxt_port_test_msg_alloc_failures(1);

    if (nxt_port_socket_write(task, port, NXT_PORT_MSG_DATA
                              | NXT_PORT_MSG_CLOSE_FD, fd, 1, 0, buf)
        != NXT_ERROR)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test expected socket write failure");
        goto fail_close_fd;
    }

    nxt_port_test_msg_alloc_failures(0);

    if (!nxt_test_fd_is_open(fd)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test closed fd before ownership transfer");
        goto fail_close_port;
    }

    if (nxt_port_fail_test_completions != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test completed unsent buffer");
        goto fail_close_fd;
    }

    after_fail = nxt_port_fail_test_fd_count();

    nxt_fd_close(fd);
    fd = -1;

    after_close = nxt_port_fail_test_fd_count();

    if (before >= 0
        && (after_open != before + 1 || after_fail != after_open
            || after_close != before))
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test fd count mismatch: %d %d %d %d",
                      before, after_open, after_fail, after_close);
        goto fail_close_port;
    }

    nxt_port_use(task, port, -1);
    nxt_mp_destroy(mp);

    return NXT_OK;

fail_close_fd:

    if (fd != -1 && nxt_test_fd_is_open(fd)) {
        nxt_fd_close(fd);
    }

fail_close_port:

    nxt_port_test_msg_alloc_failures(0);

fail:

    nxt_port_use(task, port, -1);
    nxt_mp_destroy(mp);

    return NXT_ERROR;
}


static void
nxt_port_fail_test_enable_write(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_port_fail_test_rearms++;
}


/*
 * The inline write that the port cannot take.
 *
 * The other leg above drives the allocation failure in
 * nxt_port_msg_chk_insert(), before anything is sent.  This one drives the
 * other half: the port is write-ready with an empty queue, so chk_insert()
 * declines and nxt_port_socket_write2() sends inline, and there the socket
 * answers EAGAIN with the message unable to queue for later either.
 *
 * Nothing of the message was consumed on that path, so the answer must be
 * NXT_ERROR, with the descriptor still open and no completion queued: the
 * ownership contract in src/nxt_port.h, which the caller relies on to free
 * its own payload.  Answering NXT_OK there is what left an RPC reply lost,
 * with the caller believing it had sent it (#335).
 *
 * The write event must also be re-armed.  nxt_socketpair_send() cleared
 * write_ready on the EAGAIN, and nothing else sets it back, so a port that
 * skipped the re-arm would never drain what is already queued on it.  The
 * stub engine below exists to count that call.
 */

static nxt_int_t
nxt_port_fail_test_inline_drop(nxt_thread_t *thr)
{
    int                    sndbuf;
    ssize_t                n;
    nxt_mp_t               *mp;
    nxt_fd_t               fd, pair[2];
    nxt_buf_t              *buf;
    nxt_int_t              ret;
    nxt_task_t             *task;
    nxt_port_t             *port;
    nxt_event_engine_t     engine, *saved_engine;
    nxt_port_queue_t       *queue;
    nxt_event_interface_t  stub;
    u_char                 block[4096];

    task = thr->task;
    task->thread = thr;

    ret = NXT_ERROR;
    fd = -1;
    pair[0] = -1;
    pair[1] = -1;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    /*
     * Only fast_work_queue and the one event method this path can reach are
     * real.  The code under test never reaches anything else, so it stays
     * unset.
     */

    nxt_memzero(&engine, sizeof(engine));
    nxt_memzero(&stub, sizeof(stub));

    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    stub.enable_write = nxt_port_fail_test_enable_write;
    engine.event = stub;

    saved_engine = thr->engine;
    thr->engine = &engine;

    /*
     * port->engine matches the caller's, so nxt_port_post() is a direct
     * call.  The re-arm then lands on the stub above instead of on an event
     * loop this fixture does not have.
     */
    port->engine = &engine;

    /*
     * SOCK_DGRAM: the type the port layer itself selects, since
     * src/nxt_socketpair.c keeps SEQPACKET switched off.  This fixture then
     * builds a pair on any platform the product supports.  It needs only a
     * send buffer it can fill, not seqpacket semantics.
     */

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: socketpair failed");
        goto done;
    }

    sndbuf = 4096;
    (void) setsockopt(pair[1], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    if (nxt_slow_path(fcntl(pair[1], F_SETFL, O_NONBLOCK) == -1)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: O_NONBLOCK failed");
        goto done;
    }

    port->pair[0] = pair[0];
    port->pair[1] = pair[1];
    port->socket.fd = pair[1];
    port->socket.task = task;
    port->socket.log = thr->log;
    port->max_size = 1024;
    port->max_share = 1024;

    /* Nobody reads pair[0], so the send buffer fills and stays full. */

    nxt_memzero(block, sizeof(block));

    while (send(pair[1], block, sizeof(block), 0) > 0) {
        /* void */
    }

    port->socket.write_ready = 1;
    port->socket.write = NXT_EVENT_INACTIVE;

    fd = open("/dev/null", O_RDONLY);
    if (nxt_slow_path(fd == -1)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: failed to open /dev/null");
        goto done;
    }

    buf = nxt_buf_mem_alloc(mp, 1, 0);
    if (nxt_slow_path(buf == NULL)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: failed to allocate buf");
        goto done;
    }

    buf->completion_handler = nxt_port_fail_test_completion;

    nxt_port_fail_test_completions = 0;
    nxt_port_fail_test_rearms = 0;

    nxt_port_test_msg_alloc_failures(1);

    ret = nxt_port_socket_write(task, port, NXT_PORT_MSG_DATA
                                | NXT_PORT_MSG_CLOSE_FD, fd, 1, 0, buf);

    nxt_port_test_msg_alloc_failures(0);

    if (ret != NXT_ERROR) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: an inline write that was neither "
                      "sent nor queued answered %d, expected NXT_ERROR",
                      (int) ret);
        ret = NXT_ERROR;
        goto done;
    }

    ret = NXT_ERROR;

    if (!nxt_test_fd_is_open(fd)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the inline drop closed a descriptor "
                      "it reported as not taken");
        fd = -1;
        goto done;
    }

    /*
     * Drain first: both consuming paths queue the completion rather than run
     * it, so a counter read before this would be 0 whether or not the port
     * took the buffer -- an assertion that holds for the wrong reason.
     */

    nxt_port_fail_test_drain_wq(&engine.fast_work_queue);

    if (nxt_port_fail_test_completions != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the inline drop completed a buffer "
                      "the caller still owns");
        goto done;
    }

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the refused message was left on the "
                      "port");
        goto done;
    }

    if (port->socket.write_ready != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: write_ready survived an EAGAIN");
        goto done;
    }

    if (nxt_port_fail_test_rearms != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the write event was re-armed %d "
                      "times, expected once -- without it nothing queued on "
                      "this port from here on would ever drain",
                      (int) nxt_port_fail_test_rearms);
        goto done;
    }

    /*
     * The same failure, but on a port with a shared queue: the payload goes
     * into the ring, and the inline write is only the wake-up that tells the
     * reader to look.  The buffer is already handed over, its completion
     * already queued, so this one must answer NXT_OK however the wake-up
     * ends.  nxt_runtime_port_send_quit() is a caller that cleans up after a
     * failed write; it would otherwise complete the same buffer twice.
     */

    queue = nxt_mp_zalloc(mp, sizeof(nxt_port_queue_t));
    if (nxt_slow_path(queue == NULL)) {
        ret = NXT_ERROR;
        goto done;
    }

    nxt_port_queue_init(queue);
    port->queue = queue;

    buf = nxt_buf_mem_alloc(mp, 1, 0);
    if (nxt_slow_path(buf == NULL)) {
        ret = NXT_ERROR;
        goto done;
    }

    buf->completion_handler = nxt_port_fail_test_completion;
    buf->mem.free++;

    nxt_port_fail_test_completions = 0;

    /* As the poller would leave it; the socket buffer is still full. */
    port->socket.write_ready = 1;

    nxt_port_test_msg_alloc_failures(1);

    ret = nxt_port_socket_write(task, port, NXT_PORT_MSG_DATA, -1, 0, 0, buf);

    nxt_port_test_msg_alloc_failures(0);

    if (ret != NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: a payload that reached the shared "
                      "queue answered %d; the buffer is already the port's "
                      "and the caller must not be told to reclaim it",
                      (int) ret);
        ret = NXT_ERROR;
        goto done;
    }

    ret = NXT_ERROR;

    nxt_port_fail_test_drain_wq(&engine.fast_work_queue);

    if (nxt_port_fail_test_completions != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the enqueued payload was completed "
                      "%d times, expected once",
                      (int) nxt_port_fail_test_completions);
        goto done;
    }

    /*
     * The marker that should have announced it could not be written, so the
     * port owes one.  Nothing has been lost: once the socket takes writes
     * again, the re-arm sends the marker from the stack -- no allocation,
     * which is the point, since an allocation is what failed.
     */

    if (port->announce != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the lost queue marker was not "
                      "recorded (announce %d)", (int) port->announce);
        goto done;
    }

    while (recv(pair[0], block, sizeof(block), MSG_DONTWAIT) > 0) {
        /* Make room, as a peer that reads its backlog would. */
    }

    nxt_port_rearm(task, port);

    if (port->announce != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the queue marker was still owed "
                      "after the port became writable");
        goto done;
    }

    /*
     * Every re-arm in this leg ran on the port's own engine, so none of them
     * posted anything and none may have touched the count of posts in flight.
     * The flag is unsigned: one stray decrement wraps it to its maximum and
     * every later cross-engine post is refused by the CAS, silently.
     */

    if (port->rearm_pending != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: a same-engine re-arm changed the "
                      "posted-item flag (rearm_pending %A)",
                      port->rearm_pending);
        goto done;
    }

    n = recv(pair[0], block, sizeof(block), MSG_DONTWAIT);

    if (n != (ssize_t) sizeof(nxt_port_msg_t)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the announcement was %d bytes, "
                      "expected a %d byte header", (int) n,
                      (int) sizeof(nxt_port_msg_t));
        goto done;
    }

    if (((nxt_port_msg_t *) block)->type != _NXT_PORT_MSG_READ_QUEUE) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the announcement was of type %d, "
                      "expected READ_QUEUE",
                      (int) ((nxt_port_msg_t *) block)->type);
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_port_test_msg_alloc_failures(0);

    if (fd != -1 && nxt_test_fd_is_open(fd)) {
        nxt_fd_close(fd);
    }

    nxt_port_fail_test_drain_wq(&engine.fast_work_queue);

    /*
     * Close through the port's own teardown, not by closing the pair here.
     * nxt_port_mp_cleanup() asserts both descriptors are gone, and that
     * assertion compiles out of a release build, so closing them by hand
     * looks clean until someone runs the suite with --debug.
     */

    /*
     * Take the queue back first.  It came from the test's own mem pool, and
     * nxt_port_close() munmap()s whatever port->queue points at -- 655380
     * bytes of live heap if the allocator ever returns a page-aligned
     * pointer.  Today the call fails EINVAL and only logs, which is luck,
     * not a design.
     */

    port->queue = NULL;

    nxt_port_close(task, port);

    if (pair[0] != -1 && nxt_test_fd_is_open(pair[0])) {
        nxt_fd_close(pair[0]);
    }

    nxt_port_use(task, port, -1);

    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_mp_destroy(mp);

    return ret;
}


/*
 * The same wake-up, to a peer that is gone.
 *
 * The payload reaches the shared queue exactly as above, so it is the port's
 * whatever becomes of the wake-up and the answer is still NXT_OK.  What
 * differs is everything the port does afterwards.  An EAGAIN leaves a socket
 * that will take writes again, so the marker is owed and the write event is
 * re-armed.  A send that fails outright leaves a peer that will never read
 * the ring, and nxt_port_write_msgs() has raised the error handler on it.
 *
 * Owing a marker there is not merely useless.  The re-arm enables the write
 * event, the next pass finds announce non-zero and sends the marker again,
 * and each pass is another failed send on a port the error handler is tearing
 * down -- until the event fires on a port that has been freed.  That is what
 * the application churn tests saw: repeated EPIPE, then pthread_mutex_lock()
 * failing with EINVAL on the write mutex of a destroyed port.
 */

static nxt_int_t
nxt_port_fail_test_dead_peer(nxt_thread_t *thr)
{
    nxt_mp_t               *mp;
    nxt_fd_t               pair[2];
    nxt_buf_t              *buf;
    nxt_int_t              ret;
    nxt_task_t             *task;
    nxt_port_t             *port;
    nxt_event_engine_t     engine, *saved_engine;
    nxt_port_queue_t       *queue;
    nxt_event_interface_t  stub;
    u_char                 qbuf[NXT_PORT_QUEUE_MSG_SIZE];

    task = thr->task;
    task->thread = thr;

    ret = NXT_ERROR;
    pair[0] = -1;
    pair[1] = -1;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    nxt_memzero(&engine, sizeof(engine));
    nxt_memzero(&stub, sizeof(stub));

    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    stub.enable_write = nxt_port_fail_test_enable_write;
    engine.event = stub;

    saved_engine = thr->engine;
    thr->engine = &engine;

    port->engine = &engine;

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: socketpair failed");
        goto done;
    }

    port->pair[0] = pair[0];
    port->pair[1] = pair[1];
    port->socket.fd = pair[1];
    port->socket.task = task;
    port->socket.log = thr->log;
    port->max_size = 1024;
    port->max_share = 1024;

    queue = nxt_mp_zalloc(mp, sizeof(nxt_port_queue_t));
    if (nxt_slow_path(queue == NULL)) {
        goto done;
    }

    nxt_port_queue_init(queue);
    port->queue = queue;

    buf = nxt_buf_mem_alloc(mp, 1, 0);
    if (nxt_slow_path(buf == NULL)) {
        goto done;
    }

    buf->completion_handler = nxt_port_fail_test_completion;
    buf->mem.free++;

    nxt_port_fail_test_completions = 0;
    nxt_port_fail_test_rearms = 0;

    /*
     * The reader end goes now, so the wake-up fails outright rather than
     * with EAGAIN.  Both copies of the number are cleared with it: the
     * teardown below closes whatever port->pair[0] still holds, and closing
     * a number twice would hand this fixture somebody else's descriptor.
     */

    nxt_fd_close(pair[0]);
    pair[0] = -1;
    port->pair[0] = -1;

    port->socket.write_ready = 1;
    port->socket.write = NXT_EVENT_INACTIVE;

    ret = nxt_port_socket_write(task, port, NXT_PORT_MSG_DATA, -1, 0, 0, buf);

    if (ret != NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: a payload that reached the shared "
                      "queue answered %d after the peer was gone; the buffer "
                      "is already the port's and the caller must not be told "
                      "to reclaim it", (int) ret);
        ret = NXT_ERROR;
        goto done;
    }

    ret = NXT_ERROR;

    nxt_port_fail_test_drain_wq(&engine.fast_work_queue);

    if (nxt_port_fail_test_completions != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the enqueued payload was completed "
                      "%d times, expected once",
                      (int) nxt_port_fail_test_completions);
        goto done;
    }

    if (port->announce != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: a queue marker was owed to a peer "
                      "that is gone (announce %d); every later pass resends "
                      "it on a port the error handler is tearing down",
                      (int) port->announce);
        goto done;
    }

    if (nxt_port_fail_test_rearms != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the write event was re-armed %d "
                      "times on a failed port, expected none",
                      (int) nxt_port_fail_test_rearms);
        goto done;
    }

    /*
     * The same port, now owing a marker from before the peer died -- the
     * state an earlier EAGAIN leaves, and the one the re-arm exists to
     * settle.  A failed pass must not settle it: the debt is read again on
     * the way out of every write, so a port that keeps re-arming on it sends
     * one more doomed marker per pass.
     *
     * The ring is drained first only so the next payload is again the item
     * that raises "notify"; nxt_port_queue_send() raises it on the 0-to-1
     * transition alone.
     */

    while (nxt_port_queue_recv(queue, qbuf) >= 0) {
        /* void */
    }

    nxt_atomic_fetch_add(&port->announce, 1);

    buf = nxt_buf_mem_alloc(mp, 1, 0);
    if (nxt_slow_path(buf == NULL)) {
        goto done;
    }

    buf->completion_handler = nxt_port_fail_test_completion;
    buf->mem.free++;

    nxt_port_fail_test_completions = 0;

    port->socket.write_ready = 1;

    ret = nxt_port_socket_write(task, port, NXT_PORT_MSG_DATA, -1, 0, 0, buf);

    if (ret != NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the second enqueued payload "
                      "answered %d", (int) ret);
        ret = NXT_ERROR;
        goto done;
    }

    ret = NXT_ERROR;

    nxt_port_fail_test_drain_wq(&engine.fast_work_queue);

    if (nxt_port_fail_test_rearms != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: a failed port re-armed to send a "
                      "marker it already owed (%d times)",
                      (int) nxt_port_fail_test_rearms);
        goto done;
    }

    if (port->announce != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the owed marker was counted again "
                      "on a port that is going away (announce %d)",
                      (int) port->announce);
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_port_fail_test_drain_wq(&engine.fast_work_queue);

    port->queue = NULL;

    nxt_port_close(task, port);

    nxt_port_use(task, port, -1);

    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_mp_destroy(mp);

    return ret;
}


static nxt_int_t
nxt_port_fail_test_rpc_register(nxt_thread_t *thr)
{
    void        *ex;
    nxt_task_t  *task;
    nxt_port_t  *port;

    task = thr->task;
    task->thread = thr;

    if (nxt_port_rpc_init() != NXT_OK) {
        return NXT_ERROR;
    }

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        return NXT_ERROR;
    }

    port->pair[0] = 0;

    nxt_port_rpc_test_alloc_failures(1);

    ex = nxt_port_rpc_register_handler_ex(task, port, NULL, NULL, 0);
    if (ex != NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test expected rpc alloc failure");
        goto fail;
    }

    if (port->use_count != 1 || !nxt_lvlhsh_is_empty(&port->rpc_streams)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test leaked failed rpc alloc");
        goto fail;
    }

    nxt_port_rpc_test_insert_failures(1);

    ex = nxt_port_rpc_register_handler_ex(task, port, NULL, NULL, 0);
    if (ex != NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test expected rpc insert failure");
        goto fail;
    }

    if (port->use_count != 1 || !nxt_lvlhsh_is_empty(&port->rpc_streams)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test leaked failed rpc insert");
        goto fail;
    }

    port->pair[0] = -1;
    nxt_port_use(task, port, -1);

    return NXT_OK;

fail:

    nxt_port_rpc_test_alloc_failures(0);
    nxt_port_rpc_test_insert_failures(0);

    port->pair[0] = -1;
    nxt_port_use(task, port, -1);

    return NXT_ERROR;
}


/*
 * Verify the "queued, then write failed" cleanup path inside
 * nxt_port_error_handler() — the reference behaviour that the
 * cert/script/socket/access-log reply paths now mirror after the
 * audit fix (close fd first, queue buffer completion second).  A
 * synthesised port + send_msg + buf are pushed into port->messages,
 * then nxt_port_test_run_error_handler() is invoked and the
 * resulting fast_work_queue is drained manually so the completion
 * handler runs.  Asserts:
 *
 *   - the queued message is removed from port->messages,
 *   - the close_fd-marked fd is actually closed,
 *   - the buffer completion runs exactly once.
 */
static nxt_int_t
nxt_port_fail_test_error_handler(nxt_thread_t *thr)
{
    nxt_mp_t             *mp;
    nxt_fd_t             fd;
    nxt_buf_t            *buf;
    nxt_task_t           *task;
    nxt_port_t           *port;
    nxt_event_engine_t   engine;
    nxt_port_send_msg_t  *msg;

    task = thr->task;
    task->thread = thr;

    /*
     * The minimal engine the test injects so nxt_port_error_handler
     * can deref task->thread->engine->fast_work_queue.  Only the
     * fast_work_queue + its cache need to be initialised.
     *
     * Invariant: the error path exercised here must touch nothing on
     * the engine beyond fast_work_queue (and the port-owned
     * write_mutex).  Every other field is left zeroed, so any future
     * code that dereferences another engine member would read garbage
     * here -- extend this initialisation (or use a real engine) before
     * relying on it.
     */
    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    thr->engine = &engine;

    /*
     * Test mp owns the buf so that the buffer outlives the test's
     * stack frame even though the completion handler runs
     * asynchronously via the work queue (see Gemini PR #57 review).
     * Destroyed on every exit path below.
     */
    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        goto fail_engine;
    }

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        goto fail_mp;
    }

    /*
     * Bump use_count so the per-msg use_delta-- inside
     * nxt_port_error_handler() does not drive the port to zero
     * before we have inspected its state.  Released explicitly
     * below.
     */
    port->use_count = 2;

    fd = -1;
    msg = NULL;

    fd = open("/dev/null", O_RDONLY);
    if (fd == -1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test failed to open /dev/null");
        goto fail_port;
    }

    buf = nxt_buf_mem_alloc(mp, 1, 0);
    if (nxt_slow_path(buf == NULL)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test failed to allocate buf");
        goto fail_port;
    }

    buf->completion_handler = nxt_port_fail_test_completion;

    /*
     * nxt_port_release_send_msg() free()s msg only when ->allocated
     * is set, so we mirror what nxt_port_msg_alloc() would do for a
     * heap-allocated message: nxt_malloc + ->allocated = 1.
     */
    msg = nxt_malloc(sizeof(nxt_port_send_msg_t));
    if (msg == NULL) {
        goto fail_port;
    }

    nxt_memzero(msg, sizeof(*msg));
    msg->allocated = 1;
    msg->close_fd  = 1;
    msg->fd[0]     = fd;
    msg->fd[1]     = -1;
    msg->buf       = buf;

    nxt_queue_insert_tail(&port->messages, &msg->link);

    nxt_port_fail_test_completions = 0;

    nxt_port_test_run_error_handler(task, port);

    /*
     * msg was freed inside nxt_port_release_send_msg(); take the
     * ownership pointer off so the failure label does not free it
     * again.
     */
    msg = NULL;

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test left a queued message after "
                      "error_handler");
        goto fail_port;
    }

    if (nxt_test_fd_is_open(fd)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test did not close the queued fd");
        goto fail_port;
    }

    /* fd is closed; clear so the failure label does not re-close. */
    fd = -1;

    /* Buffer completion was enqueued, not invoked synchronously. */
    if (nxt_port_fail_test_completions != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test invoked completion synchronously");
        goto fail_port;
    }

    nxt_port_fail_test_drain_wq(&engine.fast_work_queue);

    if (nxt_port_fail_test_completions != 1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test completion count: %d (want 1)",
                      (int) nxt_port_fail_test_completions);
        goto fail_port;
    }

    nxt_port_use(task, port, -1);
    nxt_mp_destroy(mp);

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    thr->engine = NULL;

    return NXT_OK;

fail_port:

    if (msg != NULL) {
        nxt_queue_remove(&msg->link);
        nxt_free(msg);
    }
    if (fd != -1 && nxt_test_fd_is_open(fd)) {
        nxt_fd_close(fd);
    }
    nxt_port_use(task, port, -1);

fail_mp:

    nxt_mp_destroy(mp);

fail_engine:

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    thr->engine = NULL;

    return NXT_ERROR;
}


/*
 * Verify the mp-refcount invariant from phpclub's #56 review: when
 * nxt_port_socket_write() fails before the buffer is handed off to
 * the port machinery, the temp config mp's retain count stays at
 * its baseline (1).  This regression-checks the audit fix in
 * src/nxt_cert.c / src/nxt_script.c, which moved `nxt_mp_retain(mp)`
 * to AFTER the successful socket_write.
 *
 * The helper nxt_port_fail_test_sender_pattern() is a deliberate
 * copy of the fixed sender shape: allocate the buf in `mp`, send,
 * retain only on success.  If a future change moves the retain back
 * above socket_write (the bug), an injected msg_alloc failure
 * leaves retain at 2 and this test fails.
 */
static nxt_int_t
nxt_port_fail_test_mp_baseline(nxt_thread_t *thr)
{
    nxt_mp_t    *mp;
    nxt_int_t   res;
    nxt_task_t  *task;
    nxt_port_t  *port;
    uint32_t    retain_before, retain_after;

    task = thr->task;
    task->thread = thr;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    retain_before = nxt_mp_test_retain_count(mp);

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    nxt_port_fail_test_completions = 0;
    nxt_port_test_msg_alloc_failures(1);

    res = nxt_port_fail_test_sender_pattern(task, port, mp);

    nxt_port_test_msg_alloc_failures(0);

    if (res != NXT_ERROR) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "mp baseline test: sender expected failure return");
        goto fail;
    }

    retain_after = nxt_mp_test_retain_count(mp);

    if (retain_after != retain_before) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "mp baseline test: retain before %uD, after %uD "
                      "(retain leaked across failed send)",
                      retain_before, retain_after);
        goto fail;
    }

    /*
     * Buf-completion handler must not have run: the buf never
     * entered the port queue.
     */
    if (nxt_port_fail_test_completions != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "mp baseline test: completion ran for unsent buf");
        goto fail;
    }

    nxt_port_use(task, port, -1);
    nxt_mp_destroy(mp);

    return NXT_OK;

fail:

    nxt_port_use(task, port, -1);
    nxt_mp_destroy(mp);

    return NXT_ERROR;
}


/*
 * Mirror of the post-audit nxt_cert_store_get sender shape:
 *   - allocate buf in the caller-supplied temp_conf mp;
 *   - attempt socket_write;
 *   - retain mp ONLY after the write succeeded.
 *
 * The completion handler released by the buf — when it eventually
 * runs — would call nxt_mp_release(mp); we do not exercise that
 * branch here since the test forces a pre-queue failure.  See
 * nxt_port_fail_test_mp_baseline().
 */
static nxt_int_t
nxt_port_fail_test_sender_pattern(nxt_task_t *task, nxt_port_t *port,
    nxt_mp_t *mp)
{
    nxt_buf_t  *b;
    nxt_int_t  res;

    b = nxt_buf_mem_alloc(mp, 16, 0);
    if (b == NULL) {
        return NXT_ERROR;
    }

    b->completion_handler = nxt_port_fail_test_mp_completion;
    b->parent = mp;

    res = nxt_port_socket_write(task, port, NXT_PORT_MSG_DATA, -1, 1, 0, b);
    if (res != NXT_OK) {
        return NXT_ERROR;
    }

    /*
     * Retain only after the buffer has been handed off to the port
     * machinery — matches the fix in nxt_cert_store_get /
     * nxt_script_store_get.
     */
    nxt_mp_retain(mp);

    return NXT_OK;
}


/*
 * A last reference dropped from a thread other than port->engine's must be
 * deferred to that engine, and the deferral must not be able to fail.
 *
 * nxt_port_use() used to route the drop through nxt_port_post(), which
 * nxt_zalloc()s an nxt_port_work_t and returns NXT_ERROR when that fails.
 * The return value was ignored: use_count was already zero, nothing was
 * queued, and the port, its memory pool and the process reference it holds
 * leaked (issue #187).
 *
 * There is no allocation-failure injection hook reachable from here, so the
 * regression is pinned structurally instead: the item that lands on the
 * foreign engine's locked work queue must be the one embedded in the port.
 * That is false for any implementation that allocates it, whether or not the
 * allocation succeeds.  Draining then has to reach nxt_port_release(), which
 * is observed through a cleanup on the port's own memory pool.
 */

static nxt_int_t
nxt_port_fail_test_cross_engine_release(nxt_thread_t *thr)
{
    nxt_task_t          *task;
    nxt_port_t          *port;
    nxt_work_t          *posted;
    nxt_event_engine_t  current, foreign;

    task = thr->task;
    task->thread = thr;

    /*
     * Two minimal engines: the one this thread runs on, and the port's.
     * They only have to differ and to carry a work queue; the post path
     * touches locked_work_queue, event.signal and task.
     */
    nxt_memzero(&current, sizeof(current));
    nxt_work_queue_cache_create(&current.work_queue_cache, 1024);
    current.fast_work_queue.cache = &current.work_queue_cache;
    nxt_work_queue_name(&current.fast_work_queue, "fast");

    nxt_memzero(&foreign, sizeof(foreign));
    foreign.task.thread = thr;
    foreign.task.log = thr->log;

    /*
     * Without this stub nxt_event_engine_signal() would fall through to
     * writing on foreign.pipe, which a bare engine does not have.
     */
    foreign.event.signal = nxt_port_fail_test_engine_signal;

    thr->engine = &current;

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        goto fail_engine;
    }

    port->engine = &foreign;

    if (nxt_slow_path(nxt_mp_cleanup(port->mem_pool,
                                     nxt_port_fail_test_released,
                                     task, port, NULL) != NXT_OK))
    {
        nxt_port_use(task, port, -1);
        goto fail_engine;
    }

    nxt_port_fail_test_releases = 0;
    nxt_port_fail_test_signals = 0;

    posted = &port->release_work;

    nxt_port_use(task, port, -1);

    /* The release must be deferred, not run on this thread. */
    if (nxt_slow_path(nxt_port_fail_test_releases != 0)) {
        nxt_log_alert(thr->log, "port fail test: cross-engine release ran "
                      "on the calling thread");
        goto fail_engine;
    }

    if (nxt_slow_path(nxt_port_fail_test_signals != 1)) {
        nxt_log_alert(thr->log, "port fail test: cross-engine release did "
                      "not signal the target engine (%ui)",
                      nxt_port_fail_test_signals);
        goto fail_engine;
    }

    /*
     * The heart of the regression check: an allocated work item would make
     * this a different pointer, and a failed allocation would leave the
     * queue empty.
     */
    if (nxt_slow_path(foreign.locked_work_queue.head != posted)) {
        nxt_log_alert(thr->log, "port fail test: the posted item is not the "
                      "port's embedded release work");
        goto fail_engine;
    }

    /*
     * The reference is handed to the item, not given up: the count is back
     * at 1 and the posted handler is what drops it.
     */
    if (nxt_slow_path(port->use_count != 1)) {
        nxt_log_alert(thr->log, "port fail test: the deferred release does "
                      "not hold a reference (use_count %A)", port->use_count);
        goto fail_engine;
    }

    /* The handler runs on the port's engine, so this thread becomes it. */
    thr->engine = &foreign;

    nxt_locked_work_queue_move(thr, &foreign.locked_work_queue,
                               &current.fast_work_queue);

    nxt_port_fail_test_drain_wq(&current.fast_work_queue);

    if (nxt_slow_path(nxt_port_fail_test_releases != 1)) {
        nxt_log_alert(thr->log, "port fail test: draining the target engine "
                      "did not release the port (%ui)",
                      nxt_port_fail_test_releases);
        goto fail_engine;
    }

    nxt_work_queue_cache_destroy(&current.work_queue_cache);
    thr->engine = NULL;

    return NXT_OK;

fail_engine:

    nxt_work_queue_cache_destroy(&current.work_queue_cache);
    thr->engine = NULL;

    return NXT_ERROR;
}


/*
 * A cross-engine last drop leaves the port reachable through
 * process->ports until the deferred item runs, so another engine can still
 * take a reference in that window -- nxt_process_broadcast_shm_ack() does.
 * The deferral therefore has to hand its reference over and re-check at the
 * far end, which is what routing through nxt_port_post() used to provide:
 * it took a reference of its own and ended in nxt_port_use(port, -1).
 *
 * A deferred handler that released unconditionally would free a port
 * somebody holds -- a use-after-free, and only an assertion in a --debug
 * build.  Here the acquisition is made between the post and the drain: the
 * drain must not release, and the release must happen exactly once, when
 * the other holder drops its reference.
 */

static nxt_int_t
nxt_port_fail_test_cross_engine_acquire(nxt_thread_t *thr)
{
    nxt_task_t          *task;
    nxt_port_t          *port;
    nxt_event_engine_t  current, foreign;

    task = thr->task;
    task->thread = thr;

    nxt_memzero(&current, sizeof(current));
    nxt_work_queue_cache_create(&current.work_queue_cache, 1024);
    current.fast_work_queue.cache = &current.work_queue_cache;
    nxt_work_queue_name(&current.fast_work_queue, "fast");

    nxt_memzero(&foreign, sizeof(foreign));
    foreign.task.thread = thr;
    foreign.task.log = thr->log;
    foreign.event.signal = nxt_port_fail_test_engine_signal;

    thr->engine = &current;

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        goto fail_engine;
    }

    port->engine = &foreign;

    if (nxt_slow_path(nxt_mp_cleanup(port->mem_pool,
                                     nxt_port_fail_test_released,
                                     task, port, NULL) != NXT_OK))
    {
        nxt_port_use(task, port, -1);
        goto fail_engine;
    }

    nxt_port_fail_test_releases = 0;

    nxt_port_use(task, port, -1);

    /*
     * The concurrent acquisition, made while the item is queued.  A real one
     * comes from another engine; what matters here is that it happens after
     * the drop that posted the item and before that item runs.
     */
    nxt_port_use(task, port, 1);

    thr->engine = &foreign;

    nxt_locked_work_queue_move(thr, &foreign.locked_work_queue,
                               &current.fast_work_queue);

    nxt_port_fail_test_drain_wq(&current.fast_work_queue);

    if (nxt_slow_path(nxt_port_fail_test_releases != 0)) {
        nxt_log_alert(thr->log, "port fail test: the deferred release freed "
                      "a port another reference was taken on");
        goto fail_port;
    }

    if (nxt_slow_path(port->use_count != 1)) {
        nxt_log_alert(thr->log, "port fail test: use_count is %A after the "
                      "deferred release, expected 1", port->use_count);
        goto fail_port;
    }

    /* Now the other holder lets go, on the port's own engine. */
    nxt_port_use(task, port, -1);

    if (nxt_slow_path(nxt_port_fail_test_releases != 1)) {
        nxt_log_alert(thr->log, "port fail test: dropping the last reference "
                      "released the port %ui times, expected 1",
                      nxt_port_fail_test_releases);
        goto fail_engine;
    }

    nxt_work_queue_cache_destroy(&current.work_queue_cache);
    thr->engine = NULL;

    return NXT_OK;

fail_port:

    nxt_port_use(task, port, -1);

fail_engine:

    nxt_work_queue_cache_destroy(&current.work_queue_cache);
    thr->engine = NULL;

    return NXT_ERROR;
}


/*
 * The hand-over must not be able to post the embedded item twice.
 *
 * The item is a single object, so two threads that both believe they are
 * dropping the last reference would post it twice, and
 * nxt_locked_work_queue_add() links a work onto the queue tail: the second
 * post makes tail->next point at the item itself, and the engine draining
 * that queue then walks a one-element cycle forever.  Routing through
 * nxt_port_post() could not produce this, because it allocated a separate
 * item for every post.
 *
 * The shape that would produce it is a foreign drop that publishes a zero
 * use_count: another engine promoting a bare port pointer --
 * nxt_process_broadcast_shm_ack() walks a process's ports that way, and
 * nxt_port_socket_write() takes a reference on each -- can then complete a
 * 0 -> 1 -> 0 cycle of its own and reach the deferral as well.  So the
 * invariant this pins is the one that makes it impossible: use_count is
 * never observable as zero off port->engine.
 *
 * A racer thread on a third engine watches use_count across a cross-engine
 * last drop and reports every zero it sees; then, with the item already in
 * flight, it takes a reference and drops it again -- the 1 -> 2 -> 1 that
 * must not post anything.  The locked queue is walked with a step limit, so
 * a self-linked item fails the test instead of hanging it.
 */

#define NXT_PORT_FAIL_TEST_RACE_TRIALS  128
#define NXT_PORT_FAIL_TEST_RACE_SPINS   4000000
#define NXT_PORT_FAIL_TEST_QUEUE_LIMIT  16


typedef struct {
    nxt_port_t          *port;
    nxt_event_engine_t  *engine;
    nxt_atomic_t        ready;
    nxt_atomic_t        stop;
    nxt_uint_t          zeroes;
} nxt_port_fail_test_race_t;


static nxt_int_t
nxt_port_fail_test_cross_engine_race(nxt_thread_t *thr)
{
    nxt_int_t                  queued;
    nxt_task_t                 *task;
    nxt_uint_t                 trial;
    nxt_port_t                 *port;
    nxt_thread_link_t          *link;
    nxt_thread_handle_t        handle;
    nxt_event_engine_t         current, foreign, other;
    nxt_port_fail_test_race_t  race;

    task = thr->task;
    task->thread = thr;

    nxt_memzero(&other, sizeof(other));

    for (trial = 0; trial < NXT_PORT_FAIL_TEST_RACE_TRIALS; trial++) {

        nxt_memzero(&current, sizeof(current));
        nxt_work_queue_cache_create(&current.work_queue_cache, 1024);
        current.fast_work_queue.cache = &current.work_queue_cache;
        nxt_work_queue_name(&current.fast_work_queue, "fast");

        nxt_memzero(&foreign, sizeof(foreign));
        foreign.task.thread = thr;
        foreign.task.log = thr->log;
        foreign.event.signal = nxt_port_fail_test_engine_signal;

        thr->engine = &current;

        port = nxt_port_fail_test_port(task);
        if (nxt_slow_path(port == NULL)) {
            goto fail_engine;
        }

        port->engine = &foreign;

        if (nxt_slow_path(nxt_mp_cleanup(port->mem_pool,
                                         nxt_port_fail_test_released,
                                         task, port, NULL) != NXT_OK))
        {
            nxt_port_use(task, port, -1);
            goto fail_engine;
        }

        nxt_port_fail_test_releases = 0;
        nxt_port_fail_test_signals = 0;

        nxt_memzero(&race, sizeof(race));
        race.port = port;
        race.engine = &other;

        link = nxt_zalloc(sizeof(nxt_thread_link_t));
        if (nxt_slow_path(link == NULL)) {
            goto fail_port;
        }

        link->start = nxt_port_fail_test_racer;
        link->work.data = &race;

        if (nxt_slow_path(nxt_thread_create(&handle, link) != NXT_OK)) {
            goto fail_port;
        }

        /*
         * The racer has to be spinning before the drop, or the window it
         * watches for is gone before it looks.
         */

        while (race.ready == 0) {
            nxt_cpu_pause();
        }

        nxt_port_use(task, port, -1);

        race.stop = 1;

        nxt_thread_wait(handle);

        if (nxt_slow_path(race.zeroes != 0)) {
            nxt_log_alert(thr->log, "port fail test: a foreign thread saw "
                          "use_count 0 on a live port %ui times", race.zeroes);
            goto fail_port;
        }

        queued = nxt_port_fail_test_queued(&foreign.locked_work_queue,
                                           &port->release_work);

        if (nxt_slow_path(queued < 0)) {
            nxt_log_alert(thr->log, "port fail test: the embedded release "
                          "work is linked into the target engine's queue "
                          "more than once");
            goto fail_engine;
        }

        if (nxt_slow_path(queued != 1)) {
            nxt_log_alert(thr->log, "port fail test: the embedded release "
                          "work is queued %i times, expected 1", queued);
            goto fail_port;
        }

        if (nxt_slow_path(nxt_port_fail_test_signals != 1)) {
            nxt_log_alert(thr->log, "port fail test: the target engine was "
                          "signalled %ui times, expected 1",
                          nxt_port_fail_test_signals);
            goto fail_port;
        }

        if (nxt_slow_path(port->use_count != 1)) {
            nxt_log_alert(thr->log, "port fail test: use_count is %A with "
                          "the deferral in flight, expected 1",
                          port->use_count);
            goto fail_port;
        }

        thr->engine = &foreign;

        nxt_locked_work_queue_move(thr, &foreign.locked_work_queue,
                                   &current.fast_work_queue);

        nxt_port_fail_test_drain_wq(&current.fast_work_queue);

        if (nxt_slow_path(nxt_port_fail_test_releases != 1)) {
            nxt_log_alert(thr->log, "port fail test: draining the target "
                          "engine released the port %ui times, expected 1",
                          nxt_port_fail_test_releases);
            goto fail_engine;
        }

        nxt_work_queue_cache_destroy(&current.work_queue_cache);
    }

    thr->engine = NULL;

    return NXT_OK;

fail_port:

    nxt_port_use(task, port, -1);

fail_engine:

    nxt_work_queue_cache_destroy(&current.work_queue_cache);
    thr->engine = NULL;

    return NXT_ERROR;
}


static void
nxt_port_fail_test_racer(void *data)
{
    nxt_task_t                 task;
    nxt_uint_t                 i;
    nxt_thread_t               *thr;
    nxt_port_fail_test_race_t  *race;

    race = data;

    thr = nxt_thread();
    thr->engine = race->engine;

    nxt_memzero(&task, sizeof(task));
    task.thread = thr;
    task.log = thr->log;

    race->ready = 1;

    for (i = 0; i < NXT_PORT_FAIL_TEST_RACE_SPINS; i++) {

        if (nxt_slow_path(race->port->use_count == 0)) {
            race->zeroes++;
            break;
        }

        if (race->stop != 0) {
            break;
        }

        nxt_cpu_pause();
    }

    /*
     * The acquisition another engine can still make while the deferral is
     * in flight, and the drop that follows it: 1 -> 2 -> 1, which must not
     * reach the deferral and must not post anything.
     */

    nxt_port_use(&task, race->port, 1);
    nxt_port_use(&task, race->port, -1);
}


/*
 * Count the occurrences of one work item in a locked work queue, refusing to
 * walk further than the queue can legitimately be.  A work item posted twice
 * becomes its own successor, so the walk would otherwise not terminate --
 * which is exactly what the draining engine does.
 */

/*
 * Re-arming a port that belongs to another engine.
 *
 * nxt_port_rearm() posts the port's own item rather than allocating one, so
 * the re-arm survives the memory pressure that made the write fail in the
 * first place.  Three things have to hold: the item on the queue is the
 * port's, a second attempt does not link it twice, and the reference the post
 * takes is given back by the handler.
 *
 * The last phase covers a port closed while the item is in flight.  The
 * handler must not enable the event then: nxt_port_write_close() leaves
 * socket.fd at the number it had, which by that point may belong to somebody
 * else.
 */

static nxt_int_t
nxt_port_fail_test_cross_engine_rearm(nxt_thread_t *thr)
{
    nxt_int_t           ret;
    nxt_task_t          *task;
    nxt_port_t          *port;
    nxt_work_t          *posted;
    nxt_event_engine_t  current, foreign;

    task = thr->task;
    task->thread = thr;

    ret = NXT_ERROR;

    nxt_memzero(&current, sizeof(current));
    nxt_work_queue_cache_create(&current.work_queue_cache, 1024);
    current.fast_work_queue.cache = &current.work_queue_cache;
    nxt_work_queue_name(&current.fast_work_queue, "fast");

    nxt_memzero(&foreign, sizeof(foreign));
    foreign.task.thread = thr;
    foreign.task.log = thr->log;
    foreign.event.signal = nxt_port_fail_test_engine_signal;
    foreign.event.enable_write = nxt_port_fail_test_enable_write;

    thr->engine = &current;

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        goto done;
    }

    port->engine = &foreign;

    /* Any descriptor but -1: the stub engine never touches the number. */
    port->pair[1] = 0;

    nxt_port_fail_test_signals = 0;
    nxt_port_fail_test_rearms = 0;

    posted = &port->rearm_work;

    nxt_port_rearm(task, port);

    if (nxt_slow_path(nxt_port_fail_test_rearms != 0)) {
        nxt_log_alert(thr->log, "port fail test: the cross-engine re-arm ran "
                      "on the calling thread");
        goto done;
    }

    if (nxt_slow_path(foreign.locked_work_queue.head != posted)) {
        nxt_log_alert(thr->log, "port fail test: the posted item is not the "
                      "port's embedded re-arm work");
        goto done;
    }

    if (nxt_slow_path(nxt_port_fail_test_signals != 1)) {
        nxt_log_alert(thr->log, "port fail test: the cross-engine re-arm did "
                      "not signal the target engine (%ui)",
                      nxt_port_fail_test_signals);
        goto done;
    }

    if (nxt_slow_path(port->use_count != 2 || port->rearm_pending != 1)) {
        nxt_log_alert(thr->log, "port fail test: the posted re-arm holds no "
                      "reference (use_count %A pending %A)", port->use_count,
                      port->rearm_pending);
        goto done;
    }

    /* A second attempt while the first is queued must be a no-op. */

    nxt_port_rearm(task, port);

    if (nxt_slow_path(nxt_port_fail_test_signals != 1
                      || port->use_count != 2))
    {
        nxt_log_alert(thr->log, "port fail test: a second re-arm posted the "
                      "same item again (signals %ui use_count %A)",
                      nxt_port_fail_test_signals, port->use_count);
        goto done;
    }

    /* The handler runs on the port's engine, so this thread becomes it. */

    thr->engine = &foreign;

    nxt_locked_work_queue_move(thr, &foreign.locked_work_queue,
                               &current.fast_work_queue);

    nxt_port_fail_test_drain_wq(&current.fast_work_queue);

    if (nxt_slow_path(nxt_port_fail_test_rearms != 1)) {
        nxt_log_alert(thr->log, "port fail test: the posted item re-armed the "
                      "write event %ui times, expected once",
                      nxt_port_fail_test_rearms);
        goto done;
    }

    if (nxt_slow_path(port->use_count != 1 || port->rearm_pending != 0)) {
        nxt_log_alert(thr->log, "port fail test: the re-arm handler did not "
                      "give its reference back (use_count %A pending %A)",
                      port->use_count, port->rearm_pending);
        goto done;
    }

    /*
     * A same-engine re-arm while a cross-engine post is still queued: it does
     * its own work and leaves the queued item exactly as it was.  Clearing the
     * flag here would let a second poster link the same item again -- and
     * severing its ->next would drop whatever the locked queue held behind it.
     */

    thr->engine = &current;

    nxt_port_rearm(task, port);

    thr->engine = &foreign;

    nxt_port_rearm(task, port);

    if (nxt_slow_path(port->rearm_pending != 1
                      || foreign.locked_work_queue.head != posted
                      || posted->next != NULL))
    {
        nxt_log_alert(thr->log, "port fail test: a same-engine re-arm "
                      "disturbed a post already in flight (pending %A)",
                      port->rearm_pending);
        goto done;
    }

    thr->engine = &foreign;

    nxt_locked_work_queue_move(thr, &foreign.locked_work_queue,
                               &current.fast_work_queue);

    nxt_port_fail_test_drain_wq(&current.fast_work_queue);

    if (nxt_slow_path(port->rearm_pending != 0 || port->use_count != 1)) {
        nxt_log_alert(thr->log, "port fail test: the queued post did not "
                      "settle (pending %A use_count %A)", port->rearm_pending,
                      port->use_count);
        goto done;
    }

    nxt_port_fail_test_rearms = 1;

    /* Closed while the next one is in flight: no enable, reference returned. */

    thr->engine = &current;

    nxt_port_rearm(task, port);

    port->pair[1] = -1;

    thr->engine = &foreign;

    nxt_locked_work_queue_move(thr, &foreign.locked_work_queue,
                               &current.fast_work_queue);

    nxt_port_fail_test_drain_wq(&current.fast_work_queue);

    if (nxt_slow_path(nxt_port_fail_test_rearms != 1)) {
        nxt_log_alert(thr->log, "port fail test: the re-arm enabled the write "
                      "event of a closed port");
        goto done;
    }

    if (nxt_slow_path(port->use_count != 1 || port->rearm_pending != 0)) {
        nxt_log_alert(thr->log, "port fail test: a re-arm that found the port "
                      "closed kept its reference (use_count %A pending %A)",
                      port->use_count, port->rearm_pending);
        goto done;
    }

    ret = NXT_OK;

done:

    if (port != NULL) {
        /* nxt_port_mp_cleanup() asserts on this in a debug build. */
        port->pair[1] = -1;

        thr->engine = &foreign;
        nxt_port_use(task, port, -1);
    }

    thr->engine = &current;

    nxt_work_queue_cache_destroy(&current.work_queue_cache);

    return ret;
}


/*
 * A caller gets its buffer back when the port refuses the message.
 *
 * src/nxt_port.h says the port layer takes fd, fd2 and b only on NXT_OK, so on
 * any other answer the caller still owns them.  A caller that does not act on
 * that leaks: nothing else completes the buffer, and for an engine pool that
 * memory is never reclaimed while the process runs.
 *
 * nxt_port_send_port() stands in for the eight sites that hand a buffer over.
 * It is the awkward one -- its buffer never leaves the function, so no caller
 * of it could clean up even if it wanted to -- and it allocates from the
 * engine pool, which makes nxt_mp_is_empty() a decisive check: the pool holds
 * the buffer, and nothing else.
 */

static nxt_int_t
nxt_port_fail_test_caller_owns_buf(nxt_thread_t *thr)
{
    nxt_mp_t            *mp;
    nxt_int_t           ret;
    nxt_task_t          *task;
    nxt_port_t          *port, *new_port;
    nxt_event_engine_t  engine, *saved_engine;

    task = thr->task;
    task->thread = thr;

    ret = NXT_ERROR;
    port = NULL;
    new_port = NULL;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    /* The buffer comes from here, which is what makes the check decisive. */
    engine.mem_pool = mp;

    saved_engine = thr->engine;
    thr->engine = &engine;

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        goto done;
    }

    new_port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(new_port == NULL)) {
        goto done;
    }

    if (nxt_slow_path(!nxt_mp_is_empty(mp))) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the fixture pool is not empty "
                      "before the write");
        goto done;
    }

    nxt_port_test_msg_alloc_failures(1);

    ret = nxt_port_send_port(task, port, new_port, 0);

    nxt_port_test_msg_alloc_failures(0);

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the refused NEW_PORT answered "
                      "NXT_OK");
        ret = NXT_ERROR;
        goto done;
    }

    ret = NXT_ERROR;

    /* The completion is queued, as nxt_port_msg_drop() queues its own. */

    nxt_port_fail_test_drain_wq(&engine.fast_work_queue);

    if (nxt_slow_path(!nxt_mp_is_empty(mp))) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the buffer of a refused message was "
                      "left in the caller's pool");
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_port_test_msg_alloc_failures(0);

    if (new_port != NULL) {
        nxt_port_use(task, new_port, -1);
    }

    if (port != NULL) {
        nxt_port_use(task, port, -1);
    }

    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_mp_destroy(mp);

    return ret;
}


static nxt_int_t
nxt_port_fail_test_queued(nxt_locked_work_queue_t *lwq, nxt_work_t *item)
{
    nxt_int_t   n, steps;
    nxt_work_t  *w;

    n = 0;

    for (w = lwq->head, steps = 0; w != NULL; w = w->next, steps++) {

        if (steps >= NXT_PORT_FAIL_TEST_QUEUE_LIMIT) {
            return -1;
        }

        if (w == item) {
            n++;
        }
    }

    return n;
}


/*
 * A drop can carry more than one reference at once: nxt_port_socket_write()
 * and nxt_port_error_handler() batch theirs into use_delta
 * (src/nxt_port_socket.c:601, src/nxt_port_socket.c:1500), so a cross-engine
 * drop of -2 or more can be the last one.
 *
 * The deferral must then leave exactly one reference standing, whatever the
 * size of the batch, because the posted handler drops exactly one.  A
 * hand-over that left the whole batch behind would strand the port, its
 * memory pool and the process reference it holds -- the very leak this
 * change exists to remove, reintroduced through the other door.
 */

static nxt_int_t
nxt_port_fail_test_cross_engine_batch(nxt_thread_t *thr)
{
    nxt_task_t          *task;
    nxt_port_t          *port;
    nxt_event_engine_t  current, foreign;

    task = thr->task;
    task->thread = thr;

    nxt_memzero(&current, sizeof(current));
    nxt_work_queue_cache_create(&current.work_queue_cache, 1024);
    current.fast_work_queue.cache = &current.work_queue_cache;
    nxt_work_queue_name(&current.fast_work_queue, "fast");

    nxt_memzero(&foreign, sizeof(foreign));
    foreign.task.thread = thr;
    foreign.task.log = thr->log;
    foreign.event.signal = nxt_port_fail_test_engine_signal;

    thr->engine = &current;

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        goto fail_engine;
    }

    port->engine = &foreign;

    if (nxt_slow_path(nxt_mp_cleanup(port->mem_pool,
                                     nxt_port_fail_test_released,
                                     task, port, NULL) != NXT_OK))
    {
        nxt_port_use(task, port, -1);
        goto fail_engine;
    }

    nxt_port_fail_test_releases = 0;

    /*
     * Two references, given up by one drop, from a thread that is not the
     * port's engine.
     */

    nxt_port_use(task, port, 1);

    nxt_port_use(task, port, -2);

    if (nxt_slow_path(nxt_port_fail_test_releases != 0)) {
        nxt_log_alert(thr->log, "port fail test: a batched cross-engine "
                      "release ran on the calling thread");
        goto fail_engine;
    }

    if (nxt_slow_path(foreign.locked_work_queue.head != &port->release_work)) {
        nxt_log_alert(thr->log, "port fail test: a batched cross-engine last "
                      "drop did not post the port's embedded release work");
        goto fail_engine;
    }

    if (nxt_slow_path(port->use_count != 1)) {
        nxt_log_alert(thr->log, "port fail test: use_count is %A after a "
                      "batched cross-engine last drop, expected 1",
                      port->use_count);
        goto fail_port;
    }

    thr->engine = &foreign;

    nxt_locked_work_queue_move(thr, &foreign.locked_work_queue,
                               &current.fast_work_queue);

    nxt_port_fail_test_drain_wq(&current.fast_work_queue);

    if (nxt_slow_path(nxt_port_fail_test_releases != 1)) {
        nxt_log_alert(thr->log, "port fail test: draining the target engine "
                      "released the batched port %ui times, expected 1",
                      nxt_port_fail_test_releases);
        goto fail_engine;
    }

    nxt_work_queue_cache_destroy(&current.work_queue_cache);
    thr->engine = NULL;

    return NXT_OK;

fail_port:

    nxt_port_use(task, port, -1);

fail_engine:

    nxt_work_queue_cache_destroy(&current.work_queue_cache);
    thr->engine = NULL;

    return NXT_ERROR;
}


static void
nxt_port_fail_test_engine_signal(nxt_event_engine_t *engine, nxt_uint_t signo)
{
    nxt_port_fail_test_signals++;
}


static void
nxt_port_fail_test_released(nxt_task_t *task, void *obj, void *data)
{
    nxt_port_fail_test_releases++;
}


static void
nxt_port_fail_test_mp_completion(nxt_task_t *task, void *obj, void *data)
{
    nxt_mp_t  *mp = data;

    nxt_port_fail_test_completions++;

    if (mp != NULL) {
        nxt_mp_release(mp);
    }
}


static void
nxt_port_fail_test_completion(nxt_task_t *task, void *obj, void *data)
{
    nxt_port_fail_test_completions++;
}


static void
nxt_port_fail_test_drain_wq(nxt_work_queue_t *wq)
{
    void                *obj, *data;
    nxt_task_t          *t;
    nxt_work_handler_t  handler;

    while (wq->head != NULL) {
        handler = nxt_work_queue_pop(wq, &t, &obj, &data);
        handler(t, obj, data);
    }
}


static nxt_int_t
nxt_port_fail_test_fd_count(void)
{
#if (NXT_LINUX)
    nxt_int_t       count;
    DIR             *dir;
    struct dirent   *de;

    dir = opendir("/proc/self/fd");
    if (dir == NULL) {
        return -1;
    }

    count = 0;

    for ( ;; ) {
        de = readdir(dir);

        if (de == NULL) {
            break;
        }

        if (nxt_strcmp(de->d_name, ".") != 0
            && nxt_strcmp(de->d_name, "..") != 0)
        {
            count++;
        }
    }

    (void) closedir(dir);

    return count;
#else
    return -1;
#endif
}


/*
 * One payload into the shared ring, with the wake-up that follows failing
 * with "err" -- and with no memory to queue the marker when "no_memory" is
 * set.  The answer must be NXT_OK and the payload completed exactly once
 * whatever became of the wake-up: it is in the ring, and the caller must
 * not be told to reclaim it.  The ring is drained first so that this
 * enqueue is the 0-to-1 transition that raises the wake-up at all.
 */

#if (NXT_HAVE_EPOLL_EDGE)

static nxt_int_t
nxt_port_fail_test_enqueue(nxt_task_t *task, nxt_port_t *port, nxt_mp_t *mp,
    nxt_err_t err, nxt_uint_t fails, nxt_bool_t no_memory, nxt_bool_t *queued)
{
    nxt_buf_t  *buf;
    nxt_int_t  ret;
    u_char     block[NXT_PORT_QUEUE_MSG_SIZE];

    while (nxt_port_queue_recv(port->queue, block) > 0) {
        /* void */
    }

    buf = nxt_buf_mem_alloc(mp, 1, 0);
    if (nxt_slow_path(buf == NULL)) {
        return NXT_ERROR;
    }

    buf->completion_handler = nxt_port_fail_test_completion;
    buf->mem.free++;

    port->socket.write_ready = 1;

    nxt_socketpair_test_send_fail(err, fails);

    if (no_memory) {
        nxt_port_test_msg_alloc_failures(1);
    }

    ret = nxt_port_socket_write(task, port, NXT_PORT_MSG_DATA, -1, 0, 0, buf);

    nxt_port_test_msg_alloc_failures(0);

    /*
     * The send hook clears itself once its count is spent, and the inline
     * write above spends one.  A leg that arms more than one clears what
     * is left when it is done, so that a retry can be made to fail too.
     */

    if (ret != NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, task->log,
                      "port failure test: a payload that reached the shared "
                      "queue answered %d after sendmsg() error %d, expected "
                      "NXT_OK", (int) ret, (int) err);
        return NXT_ERROR;
    }

    /*
     * Read before the drain, because the drain destroys the answer: an
     * error raised by the write queues nxt_port_error_handler(), which
     * empties port->messages whatever is in it.  A caller that tested the
     * queue after this returned would find it empty either way.
     */

    if (queued != NULL) {
        *queued = !nxt_queue_is_empty(&port->messages);
    }

    nxt_port_fail_test_drain_wq(&task->thread->engine->fast_work_queue);

    if (nxt_port_fail_test_completions != 1) {
        nxt_log_error(NXT_LOG_NOTICE, task->log,
                      "port failure test: the enqueued payload was completed "
                      "%d times after sendmsg() error %d, expected once",
                      (int) nxt_port_fail_test_completions, (int) err);
        return NXT_ERROR;
    }

    return NXT_OK;
}

#endif


/*
 * The wake-up that fails with an errno the socket recovers from (#392).
 *
 * A payload that fits the shared ring goes in there, and the inline write
 * is only the READ_QUEUE marker that tells the peer to look.
 * nxt_port_queue_send() raises "notify" on the 0-to-1 transition alone, so
 * if that one marker is lost, no later enqueue on the port raises another:
 * the peer stays unaware of this message and of everything after it.
 *
 * nxt_socketpair_send() answers NXT_AGAIN for EAGAIN and ENOBUFS, and
 * nxt_port_write_msgs() then keeps the marker -- queued on the port, or
 * owed in port->announce when even that allocation fails.  A kernel ENOMEM
 * is the same transient condition, and used to fall into the arm for
 * errors the socket does not recover from: the marker was dropped, the
 * error handler was raised against a live peer, and the write answered
 * NXT_OK with nothing owed.
 *
 * NXT_AGAIN also clears write_ready, so keeping the marker is only half of
 * it: the re-arm has to bring the write handler back on its own, with no
 * later write from a caller to prompt it.  So this runs on a real epoll
 * engine, edge-triggered as the product's is, and the marker is read off
 * the peer's end after one poll.  Twice: once with the write event never
 * armed, and once after the drain that sent the first marker has taken
 * the event down again -- which used to leave it blocked rather than
 * disabled, so the second re-arm touched no registration and the marker
 * waited on an edge that never came.
 *
 * Then the same ENOMEM with no memory to queue the marker, which must be
 * owed and announced from the stack; and EPIPE, where the peer is gone and
 * nothing may be owed or armed.  The errno is injected through
 * nxt_socketpair_test_send_fail(), since a live pair does not produce
 * either on demand.
 */

static nxt_int_t
nxt_port_fail_test_wakeup_errno(nxt_thread_t *thr)
{
#if (NXT_HAVE_EPOLL_EDGE)
    ssize_t              n;
    nxt_mp_t             *mp;
    nxt_fd_t             pair[2];
    nxt_int_t            ret;
    nxt_uint_t           i;
    nxt_bool_t           queued;
    nxt_task_t           *task;
    nxt_port_t           *port;
    nxt_event_engine_t   *engine, *saved_engine;
    nxt_port_queue_t     *queue;
    nxt_port_send_msg_t  *msg;
    u_char               block[NXT_PORT_QUEUE_MSG_SIZE];

    task = thr->task;
    task->thread = thr;

    ret = NXT_ERROR;
    pair[0] = -1;
    pair[1] = -1;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    saved_engine = thr->engine;

    engine = nxt_event_engine_create(task, &nxt_epoll_edge_engine, NULL, 0, 0);
    if (nxt_slow_path(engine == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    thr->engine = engine;

    port = nxt_port_fail_test_port(task);
    if (nxt_slow_path(port == NULL)) {
        goto fail;
    }

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: socketpair failed");
        goto done;
    }

    if (nxt_slow_path(fcntl(pair[1], F_SETFL, O_NONBLOCK) == -1)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: O_NONBLOCK failed");
        goto done;
    }

    port->pair[0] = pair[0];
    port->pair[1] = pair[1];
    port->socket.task = task;
    port->max_size = 1024;
    port->max_share = 1024;

    /* The real thing: handlers, engine, write_ready, and no event armed. */

    nxt_port_write_enable(task, port);

    port->socket.log = thr->log;

    queue = nxt_mp_zalloc(mp, sizeof(nxt_port_queue_t));
    if (nxt_slow_path(queue == NULL)) {
        goto done;
    }

    nxt_port_queue_init(queue);
    port->queue = queue;

    /*
     * Legs 1 and 2: ENOMEM on the wake-up, with memory to queue the marker.
     * The first arms a write event that was never registered; the second
     * finds it blocked by the drain that sent the first marker, and the
     * enable then touches no epoll registration.  Edge-triggered, the
     * second is the one that would stall if the re-arm did not re-check.
     */

    for (i = 1; i <= 2; i++) {

        /*
         * Quiescent first.  The peer's read of the previous marker wakes
         * this end's writers, and epoll keeps that as a ready entry for a
         * registered descriptor; a poll here consumes it, so the leg
         * measures the re-arm and not a wake-up left over from the last
         * leg.
         */

        engine->event.poll(engine, 0);

        nxt_port_fail_test_drain_wq(&engine->fast_work_queue);

        nxt_port_fail_test_completions = 0;

        if (nxt_port_fail_test_enqueue(task, port, mp, NXT_ENOMEM, 1, 0, NULL)
            != NXT_OK)
        {
            goto done;
        }

        if (nxt_queue_is_empty(&port->messages)) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "port failure test: leg %ui: ENOMEM on the wake-up "
                          "dropped the queue marker (announce %d); the peer "
                          "is never told to read the ring", i,
                          (int) port->announce);
            goto done;
        }

        msg = nxt_queue_link_data(nxt_queue_first(&port->messages),
                                  nxt_port_send_msg_t, link);

        if (msg->port_msg.type != _NXT_PORT_MSG_READ_QUEUE) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "port failure test: leg %ui: the message kept "
                          "after ENOMEM is of type %d, expected READ_QUEUE",
                          i, (int) msg->port_msg.type);
            goto done;
        }

        if (port->socket.write_ready != 0
            || !nxt_fd_event_is_active(port->socket.write))
        {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "port failure test: leg %ui: after ENOMEM "
                          "write_ready is %d and the write event state is "
                          "%d; expected 0 and an active event", i,
                          (int) port->socket.write_ready,
                          (int) port->socket.write);
            goto done;
        }

        /* One poll, nothing else: the re-arm alone must bring it back. */

        engine->event.poll(engine, 0);

        nxt_port_fail_test_drain_wq(&engine->fast_work_queue);

        n = recv(pair[0], block, sizeof(block), MSG_DONTWAIT);

        if (n != (ssize_t) sizeof(nxt_port_msg_t)
            || ((nxt_port_msg_t *) block)->type != _NXT_PORT_MSG_READ_QUEUE)
        {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "port failure test: leg %ui: the peer read %d bytes "
                          "after the re-arm and one poll, expected a %d byte "
                          "READ_QUEUE header (write event state %d)", i,
                          (int) n, (int) sizeof(nxt_port_msg_t),
                          (int) port->socket.write);
            goto done;
        }

        if (!nxt_queue_is_empty(&port->messages) || port->announce != 0) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "port failure test: leg %ui: the marker went out "
                          "but is still held (announce %d)", i,
                          (int) port->announce);
            goto done;
        }

        if (nxt_fd_event_is_active(port->socket.write)) {
            nxt_log_error(NXT_LOG_NOTICE, thr->log,
                          "port failure test: leg %ui: the drain left the "
                          "write event in state %d, expected it disabled", i,
                          (int) port->socket.write);
            goto done;
        }
    }

    /*
     * Leg 3: ENOMEM on the wake-up, and ENOMEM again when the write event
     * retries the queued marker.  The first failure is what legs 1 and 2
     * cover: the inline pass re-arms a disabled event and the retry runs
     * on the next poll.  The second finds the event already active, so
     * nothing re-arms it, and the socket never filled, so no edge is
     * coming either.  The retry pass has to force the readiness re-check
     * itself; without it the marker, and every message queued behind it,
     * waits for ever.
     */

    engine->event.poll(engine, 0);

    nxt_port_fail_test_drain_wq(&engine->fast_work_queue);

    nxt_port_fail_test_completions = 0;

    if (nxt_port_fail_test_enqueue(task, port, mp, NXT_ENOMEM, 2, 0, NULL)
        != NXT_OK)
    {
        goto done;
    }

    if (nxt_queue_is_empty(&port->messages) || port->socket.write_ready != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: leg 3: after the first ENOMEM the "
                      "marker is not queued, or write_ready is %d",
                      (int) port->socket.write_ready);
        goto done;
    }

    /* The first poll runs the retry, which is made to fail again. */

    engine->event.poll(engine, 0);

    nxt_port_fail_test_drain_wq(&engine->fast_work_queue);

    if (nxt_queue_is_empty(&port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: leg 3: the retry did not fail; "
                      "the marker left the queue after one poll");
        goto done;
    }

    /* The second poll has only the re-check to bring the retry back. */

    engine->event.poll(engine, 0);

    nxt_port_fail_test_drain_wq(&engine->fast_work_queue);

    nxt_socketpair_test_send_fail(0, 0);

    n = recv(pair[0], block, sizeof(block), MSG_DONTWAIT);

    if (n != (ssize_t) sizeof(nxt_port_msg_t)
        || ((nxt_port_msg_t *) block)->type != _NXT_PORT_MSG_READ_QUEUE)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: leg 3: the peer read %d bytes after "
                      "a retry that ran out of memory and two polls, expected "
                      "a %d byte READ_QUEUE header; the queued marker is "
                      "stranded (write event state %d, write_ready %d)",
                      (int) n, (int) sizeof(nxt_port_msg_t),
                      (int) port->socket.write,
                      (int) port->socket.write_ready);
        goto done;
    }

    if (!nxt_queue_is_empty(&port->messages) || port->announce != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: leg 3: the marker went out but is "
                      "still held (announce %d)", (int) port->announce);
        goto done;
    }

    if (nxt_fd_event_is_active(port->socket.write)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: leg 3: the drain left the write "
                      "event in state %d, expected it disabled",
                      (int) port->socket.write);
        goto done;
    }

    /* Leg 4: ENOMEM on the wake-up, and no memory to queue the marker. */

    nxt_port_fail_test_completions = 0;

    if (nxt_port_fail_test_enqueue(task, port, mp, NXT_ENOMEM, 1, 1, NULL)
        != NXT_OK)
    {
        goto done;
    }

    /*
     * Owed, and sent from the stack by the re-arm inside the same write,
     * before any poll: the socket is writable, so nxt_port_announce() runs
     * on the spot and the count is back to 0 by the time the write answers.
     */

    if (!nxt_queue_is_empty(&port->messages)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: ENOMEM with no memory to queue the "
                      "marker queued one anyway");
        goto done;
    }

    n = recv(pair[0], block, sizeof(block), MSG_DONTWAIT);

    if (port->announce != 0 || n != (ssize_t) sizeof(nxt_port_msg_t)
        || ((nxt_port_msg_t *) block)->type != _NXT_PORT_MSG_READ_QUEUE)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: the owed marker was not announced "
                      "(announce %d, %d bytes read)", (int) port->announce,
                      (int) n);
        goto done;
    }

    /* Leg 5: EPIPE.  The peer is gone; nothing is owed and nothing sent. */

    nxt_port_fail_test_completions = 0;

    if (nxt_port_fail_test_enqueue(task, port, mp, NXT_EPIPE, 1, 0, &queued)
        != NXT_OK)
    {
        goto done;
    }

    if (queued || port->announce != 0) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: EPIPE on the wake-up kept a marker "
                      "for a peer that is gone (queued %d, announce %d)",
                      (int) queued, (int) port->announce);
        goto done;
    }

    engine->event.poll(engine, 0);

    nxt_port_fail_test_drain_wq(&engine->fast_work_queue);

    n = recv(pair[0], block, sizeof(block), MSG_DONTWAIT);

    if (n != -1) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "port failure test: %d bytes reached the peer after "
                      "EPIPE", (int) n);
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_socketpair_test_send_fail(0, 0);
    nxt_port_test_msg_alloc_failures(0);

    nxt_port_fail_test_drain_wq(&engine->fast_work_queue);

    /* See nxt_port_fail_test_inline_drop() for both of these. */

    port->queue = NULL;

    nxt_port_close(task, port);

    if (pair[0] != -1 && nxt_test_fd_is_open(pair[0])) {
        nxt_fd_close(pair[0]);
    }

    nxt_port_use(task, port, -1);

fail:

    thr->engine = saved_engine;

    nxt_event_engine_free(engine);
    nxt_mp_destroy(mp);

    return ret;

#else

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "port failure test: wake-up errno leg needs epoll, skipped");

    return NXT_OK;

#endif
}
