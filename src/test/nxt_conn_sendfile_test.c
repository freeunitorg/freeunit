
/*
 * Copyright (C) F5, Inc.
 */

/*
 * A file buffer write must not go past the write limit.  nxt_conn_io_write()
 * subtracts each result from a size_t limit of 10 MiB.  A larger result
 * wraps the limit, and the loop then does not yield to the event loop.
 *
 * The real leg calls nxt_conn_io_sendbuf() with a small limit, a temp file
 * and a socketpair.  The write loop leg replaces sendfile(2) with a hook
 * that accepts every byte, because a socket buffer cannot take 10 MiB.
 */

#include <nxt_main.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/socket.h>


#if (NXT_HAVE_EPOLL_EDGE)

#define NXT_CONN_SENDFILE_TEST_LIMIT  (10 * 1024 * 1024)
#define NXT_CONN_SENDFILE_TEST_BUFS   3


typedef struct {
    nxt_event_engine_t  *engine;
    nxt_event_engine_t  *saved_engine;
    nxt_mp_t            *mp;
    nxt_conn_t          *c;
    nxt_fd_t            peer;
    nxt_file_t          file;
} nxt_conn_sendfile_test_t;


static nxt_int_t nxt_conn_sendfile_test_real(nxt_thread_t *thr);
static nxt_int_t nxt_conn_sendfile_test_loop(nxt_thread_t *thr,
    size_t header, size_t file_size, nxt_uint_t nbufs, nxt_off_t want_sent,
    size_t want_max);
static nxt_int_t nxt_conn_sendfile_test_setup(nxt_thread_t *thr,
    nxt_conn_sendfile_test_t *t);
static nxt_int_t nxt_conn_sendfile_test_finish(nxt_thread_t *thr,
    nxt_conn_sendfile_test_t *t, nxt_int_t ret);
static ssize_t nxt_conn_sendfile_test_hook(int fd, int s, off_t pos,
    size_t size);
static void nxt_conn_sendfile_test_handler(nxt_task_t *task, void *obj,
    void *data);


static size_t  nxt_conn_sendfile_test_max;


static const nxt_conn_state_t  nxt_conn_sendfile_test_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_conn_sendfile_test_handler,
    .close_handler = nxt_conn_sendfile_test_handler,
    .error_handler = nxt_conn_sendfile_test_handler,
};

#endif


nxt_int_t
nxt_conn_sendfile_test(nxt_thread_t *thr)
{
#if (NXT_HAVE_EPOLL_EDGE)

    nxt_int_t  ret;

    const size_t  limit = NXT_CONN_SENDFILE_TEST_LIMIT;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "conn sendfile test started");

    ret = nxt_conn_sendfile_test_real(thr);

    /* A file of exactly the limit is sent in one call. */

    if (nxt_conn_sendfile_test_loop(thr, 0, limit, 1, limit, limit)
        != NXT_OK)
    {
        ret = NXT_ERROR;
    }

    /* One byte more stays for the next pass. */

    if (nxt_conn_sendfile_test_loop(thr, 0, limit + 1, 1, limit, limit)
        != NXT_OK)
    {
        ret = NXT_ERROR;
    }

    /* A header uses part of the limit before the file. */

    if (nxt_conn_sendfile_test_loop(thr, 100, limit, 1, limit, limit - 100)
        != NXT_OK)
    {
        ret = NXT_ERROR;
    }

    /* The second buffer gets the rest of the limit; the third waits. */

    if (nxt_conn_sendfile_test_loop(thr, 0, 6 * 1024 * 1024,
                                    NXT_CONN_SENDFILE_TEST_BUFS, limit,
                                    6 * 1024 * 1024)
        != NXT_OK)
    {
        ret = NXT_ERROR;
    }

    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "conn sendfile test passed");

#endif

    return NXT_OK;
}


#if (NXT_HAVE_EPOLL_EDGE)

/* The real sendfile(2) at a limit of 1000 bytes and of the file size. */

static nxt_int_t
nxt_conn_sendfile_test_real(nxt_thread_t *thr)
{
    ssize_t                   n;
    nxt_int_t                 ret;
    nxt_uint_t                i;
    nxt_buf_t                 *b;
    nxt_sendbuf_t             sb;
    nxt_conn_sendfile_test_t  t;
    u_char                    data[4096];
    char                      name[] = "/tmp/nxt_conn_sendfile_test.XXXXXX";

    static const struct {
        size_t     limit;
        nxt_off_t  file_end;
        ssize_t    want;
    } legs[] = {
        { 1000, 4096, 1000 },
        { 4096, 4096, 4096 },
        { 4095, 4096, 4095 },
    };

    if (nxt_conn_sendfile_test_setup(thr, &t) != NXT_OK) {
        return nxt_conn_sendfile_test_finish(thr, &t, NXT_ERROR);
    }

    t.file.fd = mkstemp(name);
    if (nxt_slow_path(t.file.fd == -1)) {
        nxt_log_alert(thr->log, "conn sendfile test: mkstemp() failed %E",
                      nxt_errno);
        return nxt_conn_sendfile_test_finish(thr, &t, NXT_ERROR);
    }

    (void) unlink(name);

    nxt_memset(data, 'a', sizeof(data));

    if (nxt_slow_path(write(t.file.fd, data, sizeof(data))
                      != (ssize_t) sizeof(data)))
    {
        nxt_log_alert(thr->log, "conn sendfile test: write() failed");
        return nxt_conn_sendfile_test_finish(thr, &t, NXT_ERROR);
    }

    ret = NXT_OK;

    for (i = 0; i < nxt_nitems(legs); i++) {
        b = nxt_buf_file_alloc(t.mp, 0, 0);
        if (nxt_slow_path(b == NULL)) {
            return nxt_conn_sendfile_test_finish(thr, &t, NXT_ERROR);
        }

        b->file = &t.file;
        b->file_end = legs[i].file_end;

        nxt_memzero(&sb, sizeof(nxt_sendbuf_t));
        sb.socket = t.c->socket.fd;
        sb.buf = b;
        sb.limit = legs[i].limit;
        sb.ready = 1;

        n = nxt_conn_io_sendbuf(thr->task, &sb);

        if (n != legs[i].want) {
            nxt_log_alert(thr->log, "conn sendfile test: limit %uz, "
                          "file %O: sent %z, want %z", legs[i].limit,
                          legs[i].file_end, n, legs[i].want);
            ret = NXT_ERROR;
        }

        /* Empty the peer, so that the next leg has room. */

        while (read(t.peer, data, sizeof(data)) > 0) { /* void */ }
    }

    return nxt_conn_sendfile_test_finish(thr, &t, ret);
}


/*
 * One nxt_conn_io_write() pass over an optional memory header and
 * "nbufs" file buffers of "file_size" bytes each.  The hook accepts every
 * byte, so only the limit stops the pass.
 */

static nxt_int_t
nxt_conn_sendfile_test_loop(nxt_thread_t *thr, size_t header,
    size_t file_size, nxt_uint_t nbufs, nxt_off_t want_sent, size_t want_max)
{
    nxt_int_t                 ret;
    nxt_uint_t                i;
    nxt_buf_t                 *b, **next;
    nxt_conn_t                *c;
    nxt_conn_sendfile_test_t  t;

    if (nxt_conn_sendfile_test_setup(thr, &t) != NXT_OK) {
        return nxt_conn_sendfile_test_finish(thr, &t, NXT_ERROR);
    }

    c = t.c;
    next = &c->write;

    if (header != 0) {
        b = nxt_buf_mem_alloc(t.mp, header, 0);
        if (nxt_slow_path(b == NULL)) {
            return nxt_conn_sendfile_test_finish(thr, &t, NXT_ERROR);
        }

        nxt_memset(b->mem.free, 'h', header);
        b->mem.free += header;

        *next = b;
        next = &b->next;
    }

    for (i = 0; i < nbufs; i++) {
        b = nxt_buf_file_alloc(t.mp, 0, 0);
        if (nxt_slow_path(b == NULL)) {
            return nxt_conn_sendfile_test_finish(thr, &t, NXT_ERROR);
        }

        b->file = &t.file;
        b->file_end = file_size;

        *next = b;
        next = &b->next;
    }

    nxt_conn_sendfile_test_max = 0;
    nxt_conn_test_sendfile = nxt_conn_sendfile_test_hook;

    nxt_conn_io_write(thr->task, c, NULL);

    nxt_conn_test_sendfile = NULL;

    ret = NXT_OK;

    if (c->sent != want_sent || nxt_conn_sendfile_test_max != want_max) {
        nxt_log_alert(thr->log, "conn sendfile test: header %uz, %ui x %uz: "
                      "sent %O, want %O; largest sendfile %uz, want %uz",
                      header, nbufs, file_size, c->sent, want_sent,
                      nxt_conn_sendfile_test_max, want_max);
        ret = NXT_ERROR;
    }

    /* The pass ended on the limit, so it yields through the write timer. */

    if (!c->write_timer.enabled) {
        nxt_log_alert(thr->log, "conn sendfile test: header %uz, %ui x %uz: "
                      "the write timer is not armed", header, nbufs,
                      file_size);
        ret = NXT_ERROR;
    }

    return nxt_conn_sendfile_test_finish(thr, &t, ret);
}


static nxt_int_t
nxt_conn_sendfile_test_setup(nxt_thread_t *thr, nxt_conn_sendfile_test_t *t)
{
    nxt_fd_t            pair[2];
    nxt_conn_t          *c;
    nxt_task_t          *task;
    nxt_event_engine_t  *engine;

    task = thr->task;
    task->thread = thr;

    nxt_memzero(t, sizeof(nxt_conn_sendfile_test_t));
    t->peer = -1;
    t->file.fd = -1;

    t->saved_engine = thr->engine;

    engine = nxt_event_engine_create(task, &nxt_epoll_edge_engine, NULL, 0, 0);
    if (nxt_slow_path(engine == NULL)) {
        return NXT_ERROR;
    }

    t->engine = engine;
    thr->engine = engine;

    t->mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(t->mp == NULL)) {
        return NXT_ERROR;
    }

    c = nxt_conn_create(t->mp, task);
    if (nxt_slow_path(c == NULL)) {
        return NXT_ERROR;
    }

    t->c = c;

    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)) {
        nxt_log_alert(thr->log, "conn sendfile test: socketpair() failed %E",
                      nxt_errno);
        return NXT_ERROR;
    }

    c->socket.fd = pair[0];
    t->peer = pair[1];

    if (nxt_slow_path(fcntl(pair[0], F_SETFL, O_NONBLOCK) == -1
                      || fcntl(pair[1], F_SETFL, O_NONBLOCK) == -1))
    {
        return NXT_ERROR;
    }

    c->socket.write_ready = 1;

    c->read_work_queue = &engine->fast_work_queue;
    c->write_work_queue = &engine->fast_work_queue;
    c->socket.read_work_queue = &engine->read_work_queue;
    c->socket.write_work_queue = &engine->write_work_queue;

    c->write_state = &nxt_conn_sendfile_test_state;

    return NXT_OK;
}


static nxt_int_t
nxt_conn_sendfile_test_finish(nxt_thread_t *thr, nxt_conn_sendfile_test_t *t,
    nxt_int_t ret)
{
    nxt_conn_t  *c;

    c = t->c;

    if (c != NULL) {
        if (c->socket.fd != -1) {
            nxt_fd_close(c->socket.fd);
            c->socket.fd = -1;
        }

        nxt_conn_free(thr->task, c);

    } else if (t->mp != NULL) {
        nxt_mp_destroy(t->mp);
    }

    if (t->peer != -1) {
        nxt_fd_close(t->peer);
    }

    if (t->file.fd != -1) {
        nxt_fd_close(t->file.fd);
    }

    if (t->engine != NULL) {
        nxt_event_engine_free(t->engine);
    }

    thr->engine = t->saved_engine;

    return ret;
}


static ssize_t
nxt_conn_sendfile_test_hook(int fd, int s, off_t pos, size_t size)
{
    if (size > nxt_conn_sendfile_test_max) {
        nxt_conn_sendfile_test_max = size;
    }

    return size;
}


static void
nxt_conn_sendfile_test_handler(nxt_task_t *task, void *obj, void *data)
{
}

#endif
