/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_rpc.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"

#if (NXT_LINUX)
#include <dirent.h>
#endif


static nxt_port_t *nxt_port_fail_test_port(nxt_task_t *task);
static nxt_int_t nxt_port_fail_test_socket_write(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_rpc_register(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_error_handler(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_mp_baseline(nxt_thread_t *thr);
static nxt_int_t nxt_port_fail_test_cross_engine_release(nxt_thread_t *thr);
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
static nxt_bool_t nxt_port_fail_test_fd_is_open(nxt_fd_t fd);
static nxt_int_t nxt_port_fail_test_fd_count(void);


static nxt_uint_t  nxt_port_fail_test_completions;
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

    if (!nxt_port_fail_test_fd_is_open(fd)) {
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

    if (fd != -1 && nxt_port_fail_test_fd_is_open(fd)) {
        nxt_fd_close(fd);
    }

fail_close_port:

    nxt_port_test_msg_alloc_failures(0);

fail:

    nxt_port_use(task, port, -1);
    nxt_mp_destroy(mp);

    return NXT_ERROR;
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

    if (nxt_port_fail_test_fd_is_open(fd)) {
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
    if (fd != -1 && nxt_port_fail_test_fd_is_open(fd)) {
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


static nxt_bool_t
nxt_port_fail_test_fd_is_open(nxt_fd_t fd)
{
    return fcntl(fd, F_GETFD) != -1;
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
