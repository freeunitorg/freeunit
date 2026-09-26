
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Fragment reassembly limits at a receiving port (#394): streams per port,
 * bytes per stream and bytes per port.  A stream past a limit is dropped
 * whole, so its handler never runs.  Fragment buffers point into a
 * PROT_NONE reservation that nothing reads, so a 128 MB stream is free.
 * A fragment counts for the buffer it came in, so empty fragments are
 * bounded too.
 *
 * An mmap fragment that carries no shared memory record -- empty, a partial
 * record, or one that names no segment -- is refused: its read buffer used
 * to be kept in the stream and also given back to port->free_bufs.
 *
 * An mmap fragment of empty records keeps a buffer per record while its
 * payload is next to nothing; the buffers count too, or a port's total
 * admits millions of them.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_memory_int.h>
#include <nxt_runtime.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"

#include <sys/mman.h>


#define NXT_FRAG_TEST_MB      (1024 * 1024)
#define NXT_FRAG_TEST_STREAM  0x46524700    /* distinct from other tests */


static u_char      *nxt_frag_test_space;
static nxt_uint_t  nxt_frag_test_calls;
static uint32_t    nxt_frag_test_last_stream;
static size_t      nxt_frag_test_last_size;
static nxt_bool_t  nxt_frag_test_oom;

static nxt_task_t  *nxt_frag_test_task;
static nxt_port_t  *nxt_frag_test_port;
static nxt_bool_t  nxt_frag_test_aliased;

static nxt_pid_t       nxt_frag_test_sender;
static nxt_chunk_id_t  nxt_frag_test_chunk;


typedef struct {
    size_t      size;       /* of the mmap fragment's payload */
    nxt_uint_t  pos;        /* of the mmap fragment in a stream of three */
} nxt_frag_test_mmap_case_t;


static const nxt_frag_test_mmap_case_t  nxt_frag_test_mmap_cases[] = {
    { 0, 0 }, { 0, 1 }, { 0, 2 },
    { 5, 0 }, { 5, 1 }, { 5, 2 },
    { sizeof(nxt_port_mmap_msg_t), 0 },
    { sizeof(nxt_port_mmap_msg_t), 1 },
    { sizeof(nxt_port_mmap_msg_t), 2 },
};


static void
nxt_frag_test_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_frag_test_calls++;
    nxt_frag_test_last_stream = msg->port_msg.stream;
    nxt_frag_test_last_size = msg->size;
}


static void
nxt_frag_test_send(nxt_task_t *task, nxt_port_t *port, uint32_t stream,
    size_t size, nxt_bool_t first, nxt_bool_t last)
{
    nxt_buf_t            *b;
    nxt_port_recv_msg_t  msg;

    b = nxt_mp_zalloc(port->mem_pool, sizeof(nxt_buf_t));
    if (nxt_slow_path(b == NULL)) {
        nxt_frag_test_oom = 1;
        return;
    }

    b->mem.start = nxt_frag_test_space;
    b->mem.pos = nxt_frag_test_space;
    b->mem.free = nxt_frag_test_space;
    b->mem.end = nxt_frag_test_space + size;

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.port = port;
    msg.buf = b;
    msg.size = sizeof(nxt_port_msg_t) + size;
    msg.fd[0] = -1;
    msg.fd[1] = -1;
    msg.port_msg.stream = stream;
    msg.port_msg.pid = nxt_pid;
    msg.port_msg.type = _NXT_PORT_MSG_STATUS;
    msg.port_msg.nf = !first;
    msg.port_msg.mf = !last;

    nxt_port_test_run_read_msg_process(task, port, &msg);
}


/* A stream of "n" fragments of "mb" MB: delivered whole, or not at all. */
static nxt_int_t
nxt_frag_test_expect(nxt_task_t *task, nxt_port_t *port, const char *name,
    uint32_t stream, nxt_uint_t n, size_t mb, nxt_bool_t expect)
{
    nxt_uint_t  i, calls;

    calls = nxt_frag_test_calls;

    for (i = 0; i < n; i++) {
        nxt_frag_test_send(task, port, stream, mb * NXT_FRAG_TEST_MB, i == 0,
                           i == n - 1);
    }

    NXT_TEST_CHECK(task->log, (nxt_frag_test_calls != calls) == expect,
                   "port frag test: %s: %s", name,
                   expect ? "dropped" : "delivered");
    NXT_TEST_CHECK(task->log, !expect
                   || (nxt_frag_test_last_stream == stream
                       && nxt_frag_test_last_size == n * mb * NXT_FRAG_TEST_MB),
                   "port frag test: %s: %uz bytes", name,
                   nxt_frag_test_last_size);
    return NXT_OK;
}


static nxt_int_t
nxt_frag_test_run(nxt_task_t *task, nxt_port_t *port)
{
    size_t      mb, half;
    uint32_t    s;
    nxt_uint_t  i, n, calls;

    s = NXT_FRAG_TEST_STREAM;
    mb = NXT_PORT_FRAG_SIZE_MAX / NXT_FRAG_TEST_MB;

    /* Per stream: at the limit; past it on the last or a middle fragment. */

    if (nxt_frag_test_expect(task, port, "at the size limit", s++, mb, 1, 1)
        || nxt_frag_test_expect(task, port, "past it, last", s++, mb + 1, 1, 0)
        || nxt_frag_test_expect(task, port, "past it, middle", s++, mb + 2, 1,
                                0)
        || nxt_frag_test_expect(task, port, "after a drop", s++, 3, 1, 1))
    {
        return NXT_ERROR;
    }

    /* Per port, streams: open the most there may be, then one more. */

    for (i = 0; i < NXT_PORT_FRAG_STREAMS_MAX; i++) {
        nxt_frag_test_send(task, port, s + i, 1, 1, 0);
    }

    calls = nxt_frag_test_calls;

    nxt_frag_test_send(task, port, s + i, 1, 1, 0);
    nxt_frag_test_send(task, port, s + i, 1, 0, 1);

    NXT_TEST_CHECK(task->log, nxt_frag_test_calls == calls,
                   "port frag test: a stream past the most was accepted");

    /* The open ones still complete, and free their slots as they do. */

    for (i = 0; i < NXT_PORT_FRAG_STREAMS_MAX; i++) {
        nxt_frag_test_send(task, port, s + i, 1, 0, 1);
    }

    NXT_TEST_CHECK(task->log,
                   nxt_frag_test_calls == calls + NXT_PORT_FRAG_STREAMS_MAX,
                   "port frag test: open streams did not complete");

    s += NXT_PORT_FRAG_STREAMS_MAX + 1;

    /* Per port, bytes: two open halves fill it; a third stream is dropped. */

    half = NXT_PORT_FRAG_TOTAL_MAX / 2 / NXT_FRAG_TEST_MB * NXT_FRAG_TEST_MB;

    nxt_frag_test_send(task, port, s, half, 1, 0);
    nxt_frag_test_send(task, port, s + 1, half, 1, 0);

    if (nxt_frag_test_expect(task, port, "past the port's total", s + 2, 2,
                             1, 0))
    {
        return NXT_ERROR;
    }

    calls = nxt_frag_test_calls;

    nxt_frag_test_send(task, port, s, 0, 0, 1);
    nxt_frag_test_send(task, port, s + 1, 0, 0, 1);

    NXT_TEST_CHECK(task->log, nxt_frag_test_calls == calls + 2,
                   "port frag test: streams within the total did not "
                   "complete");

    /* Everything is accounted back: a fresh stream at the limit passes. */

    if (nxt_frag_test_expect(task, port, "at the limit, again", s + 3, mb, 1,
                             1))
    {
        return NXT_ERROR;
    }

    /*
     * Empty fragments: each one holds a buffer of port->max_size, so the
     * port's total admits NXT_PORT_FRAG_TOTAL_MAX / max_size of them.
     */

    port->max_size = 16 * 1024;
    n = NXT_PORT_FRAG_TOTAL_MAX / port->max_size;

    if (nxt_frag_test_expect(task, port, "empty fragments up to the total",
                             s + 4, n + 1, 0, 1)
        || nxt_frag_test_expect(task, port, "empty fragments past the total",
                                s + 5, n + 2, 0, 0))
    {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(task->log, !nxt_frag_test_oom,
                   "port frag test: out of memory");
    return NXT_OK;
}


/* Would the assembled message hand over a buffer the port already reuses? */
static void
nxt_frag_test_mmap_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_buf_t  *b, *f;

    nxt_frag_test_calls++;

    for (b = msg->buf; b != NULL; b = b->next) {
        for (f = msg->port->free_bufs; f != NULL; f = f->next) {
            if (f == b) {
                nxt_frag_test_aliased = 1;
            }
        }
    }
}


/*
 * Sends one fragment the way nxt_port_read_handler() receives it: in a
 * buffer of its own, given back to port->free_bufs afterwards unless the
 * dispatch took it.
 */
static nxt_int_t
nxt_frag_test_mmap_send(nxt_port_t *port, const u_char *data, size_t size,
    nxt_bool_t mmap, nxt_bool_t first, nxt_bool_t last)
{
    nxt_buf_t            *b;
    nxt_port_recv_msg_t  msg;

    b = nxt_buf_mem_alloc(port->mem_pool, 64, 0);
    if (b == NULL) {
        return NXT_ERROR;
    }

    nxt_memcpy(b->mem.start, data, size);

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.port = port;
    msg.buf = b;
    msg.size = sizeof(nxt_port_msg_t) + size;
    msg.fd[0] = -1;
    msg.fd[1] = -1;
    msg.port_msg.stream = NXT_FRAG_TEST_STREAM - 1;
    msg.port_msg.pid = nxt_pid;
    msg.port_msg.type = _NXT_PORT_MSG_STATUS;
    msg.port_msg.nf = !first;
    msg.port_msg.mf = !last;
    msg.port_msg.mmap = mmap;

    nxt_port_test_run_read_msg_process(nxt_frag_test_task, port, &msg);

    if (msg.buf == b) {
        b->next = port->free_bufs;
        port->free_bufs = b;
    }

    return NXT_OK;
}


/* In a child: the last position used to complete plain buffers as mmap. */
static int
nxt_frag_test_mmap_child(void *data)
{
    u_char                           payload[64];
    uint32_t                         streams, size;
    nxt_uint_t                       i;
    nxt_port_t                       *port;
    nxt_port_mmap_msg_t              rec;
    const nxt_frag_test_mmap_case_t  *tc;

    tc = data;
    port = nxt_frag_test_port;

    /* A whole record, for a segment the sender never shared. */
    rec.mmap_id = 0;
    rec.chunk_id = 0;
    rec.size = 100;

    nxt_memset(payload, 'x', sizeof(payload));
    nxt_memcpy(payload, &rec, sizeof(rec));

    port->handler = nxt_frag_test_mmap_handler;
    nxt_frag_test_calls = 0;
    nxt_frag_test_aliased = 0;

    streams = port->frag_streams;
    size = port->frag_size;

    for (i = 0; i < 3; i++) {
        if (nxt_frag_test_mmap_send(port, payload,
                                    (i == tc->pos) ? tc->size : 16,
                                    i == tc->pos, i == 0, i == 2)
            != NXT_OK)
        {
            return 8;
        }
    }

    /* 1: delivered; 2: with a buffer the port reuses; 4: not given back. */
    return (nxt_frag_test_calls != 0)
           | (nxt_frag_test_aliased << 1)
           | ((port->frag_streams != streams || port->frag_size != size) << 2);
}


static nxt_int_t
nxt_frag_test_mmap_run(nxt_thread_t *thr, nxt_port_t *port)
{
    int            status;
    nxt_int_t      ret;
    nxt_uint_t     i;
    nxt_runtime_t  *rt, *saved_rt;

    /* A runtime that knows no sender, so no record names a segment. */
    rt = nxt_mp_zalloc(port->mem_pool, sizeof(nxt_runtime_t));
    if (rt == NULL
        || nxt_thread_mutex_create(&rt->processes_mutex) != NXT_OK)
    {
        return NXT_ERROR;
    }

    saved_rt = thr->runtime;
    thr->runtime = rt;

    nxt_frag_test_task = thr->task;
    nxt_frag_test_port = port;

    ret = NXT_OK;

    for (i = 0; i < nxt_nitems(nxt_frag_test_mmap_cases); i++) {
        status = nxt_test_in_child(thr, "port frag test",
                                   nxt_frag_test_mmap_child,
                                   (void *) &nxt_frag_test_mmap_cases[i]);
        if (status != 0) {
            nxt_log_alert(thr->log, "port frag test: an mmap fragment of "
                          "%uz bytes at position %ui was not refused (%d)",
                          nxt_frag_test_mmap_cases[i].size,
                          nxt_frag_test_mmap_cases[i].pos, status);
            ret = NXT_ERROR;
        }
    }

    thr->runtime = saved_rt;

    nxt_thread_mutex_destroy(&rt->processes_mutex);

    return ret;
}


/*
 * In a child: with the port's total all taken but "room", open a stream of
 * mmap fragments, each a max_size read buffer full of empty records for a
 * segment the sender did share, and send until the stream is dropped.
 * Every record keeps a buffer of at least sizeof(nxt_buf_t), so what the
 * stream held must fit in the room it had.
 */
static int
nxt_frag_test_records_child(void *data)
{
    size_t                  room, nrecs;
    uint32_t                streams;
    nxt_buf_t               *b;
    nxt_uint_t              i, j, n;
    nxt_port_t              *port;
    nxt_work_queue_t        wq;
    nxt_port_mmap_msg_t     *rec;
    nxt_port_recv_msg_t     msg;
    nxt_work_queue_cache_t  cache;

    port = nxt_frag_test_port;
    port->handler = nxt_frag_test_handler;
    port->max_size = 16 * 1024;

    /* Where a dropped stream's shared memory buffers complete. */
    nxt_memzero(&wq, sizeof(nxt_work_queue_t));
    nxt_work_queue_cache_create(&cache, 1024);
    wq.cache = &cache;
    port->socket.read_work_queue = &wq;

    nrecs = port->max_size / sizeof(nxt_port_mmap_msg_t);
    room = 64 * port->max_size;
    n = room / port->max_size + 2;

    streams = port->frag_streams;
    port->frag_size = NXT_PORT_FRAG_TOTAL_MAX - room;

    for (i = 0; i < n; i++) {
        b = nxt_buf_mem_alloc(port->mem_pool, port->max_size, 0);
        if (b == NULL) {
            return 8;
        }

        rec = (nxt_port_mmap_msg_t *) b->mem.start;

        for (j = 0; j < nrecs; j++) {
            rec[j].mmap_id = 0;
            rec[j].chunk_id = nxt_frag_test_chunk;
            rec[j].size = 0;
        }

        nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

        msg.port = port;
        msg.buf = b;
        msg.size = sizeof(nxt_port_msg_t) + nrecs * sizeof(*rec);
        msg.fd[0] = -1;
        msg.fd[1] = -1;
        msg.port_msg.stream = NXT_FRAG_TEST_STREAM - 2;
        msg.port_msg.pid = nxt_frag_test_sender;
        msg.port_msg.type = _NXT_PORT_MSG_STATUS;
        msg.port_msg.nf = (i != 0);
        msg.port_msg.mf = 1;
        msg.port_msg.mmap = 1;

        nxt_port_test_run_read_msg_process(nxt_frag_test_task, port, &msg);

        if (msg.buf == b) {
            b->next = port->free_bufs;
            port->free_bufs = b;
        }

        if (port->frag_streams == streams) {
            break;
        }
    }

    /* 1: held more than its room; 2: never dropped; 4: not given back. */
    return (i * nrecs * sizeof(nxt_buf_t) > room)
           | ((i == n) << 1)
           | ((port->frag_size != NXT_PORT_FRAG_TOTAL_MAX - room) << 2);
}


/*
 * The runtime knows one sender, with a segment in its incoming array, so
 * nxt_port_mmap_read() turns each of its records into a buffer.
 */
static nxt_int_t
nxt_frag_test_records_run(nxt_thread_t *thr, nxt_port_t *port)
{
    int                      status;
    nxt_buf_t                *seg;
    nxt_task_t               *task;
    nxt_process_t            *process;
    nxt_runtime_t            *rt, *saved_rt;
    nxt_event_engine_t       engine, *saved_engine;
    nxt_port_mmap_handler_t  *mmap_handler;

    task = thr->task;

    rt = nxt_mp_zalloc(port->mem_pool, sizeof(nxt_runtime_t));
    process = nxt_mp_zalloc(port->mem_pool, sizeof(nxt_process_t));

    if (rt == NULL || process == NULL
        || nxt_thread_mutex_create(&rt->processes_mutex) != NXT_OK)
    {
        return NXT_ERROR;
    }

    rt->mem_pool = port->mem_pool;

    nxt_memzero(&engine, sizeof(engine));
    engine.mem_pool = port->mem_pool;
    engine.task.thread = thr;
    engine.task.log = thr->log;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;
    thr->runtime = rt;
    thr->engine = &engine;

    status = -1;

    process->pid = nxt_pid + 43;
    process->use_count = 1;
    nxt_queue_init(&process->ports);

    if (nxt_thread_mutex_create(&process->incoming.mutex) != NXT_OK) {
        goto done;
    }

    nxt_runtime_process_add(task, process);

    seg = nxt_port_mmap_get_buf(task, &process->incoming,
                                PORT_MMAP_CHUNK_SIZE);
    if (seg == NULL) {
        goto done;
    }

    mmap_handler = seg->parent;

    nxt_frag_test_task = task;
    nxt_frag_test_port = port;
    nxt_frag_test_sender = process->pid;
    nxt_frag_test_chunk = nxt_port_mmap_chunk_id(mmap_handler->hdr,
                                                 seg->mem.pos);

    status = nxt_test_in_child(thr, "port frag test",
                               nxt_frag_test_records_child, NULL);
    if (status != 0) {
        nxt_log_alert(thr->log, "port frag test: mmap fragments of empty "
                      "records passed the port's total (%d)", status);
    }

done:

    /* The segment stays mapped, like other port fixtures. */

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    return (status == 0) ? NXT_OK : NXT_ERROR;
}


nxt_int_t
nxt_port_frag_test(nxt_thread_t *thr)
{
    size_t              reserve;
    nxt_int_t           ret;
    nxt_task_t          *task;
    nxt_port_t          *port;

    nxt_thread_time_update(thr);

    task = thr->task;
    task->thread = thr;

    reserve = NXT_PORT_FRAG_TOTAL_MAX + 16 * NXT_FRAG_TEST_MB;

    nxt_frag_test_space = mmap(NULL, reserve, PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                               -1, 0);
    if (nxt_frag_test_space == MAP_FAILED) {
        nxt_log_alert(thr->log, "port frag test: mmap() failed %E", nxt_errno);
        return NXT_ERROR;
    }

    port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_ROUTER);
    if (nxt_slow_path(port == NULL)) {
        (void) munmap(nxt_frag_test_space, reserve);
        return NXT_ERROR;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;
    port->handler = nxt_frag_test_handler;

    ret = nxt_frag_test_run(task, port);

    if (ret == NXT_OK) {
        ret = nxt_frag_test_mmap_run(thr, port);
    }

    if (ret == NXT_OK) {
        ret = nxt_frag_test_records_run(thr, port);
    }

    /*
     * The port's pool holds everything the streams allocated, the buffers
     * on port->free_bufs included; nothing else refers to the port.
     */
    nxt_port_use(task, port, -1);

    (void) munmap(nxt_frag_test_space, reserve);

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port frag test passed");
    }

    return ret;
}
