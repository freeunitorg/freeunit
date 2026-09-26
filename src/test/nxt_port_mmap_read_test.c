/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_port_mmap_read() (src/nxt_port_memory.c) turns an untrusted peer's
 * array of nxt_port_mmap_msg_t into buffers over its shared memory.  It
 * used to read a partial last record past mem.free.  The records sit at the
 * end of a page followed by a PROT_NONE page, so an overrun faults in every
 * build; each case runs in a child.  The other rows pin the per-field checks.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_memory_int.h>
#include <nxt_runtime.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"

#include <sys/mman.h>


typedef struct {
    const char           *name;
    nxt_pid_t            pid_off;       /* 1: an unknown sender */
    nxt_uint_t           nrecords;
    size_t               tail;
    nxt_uint_t           expect_bufs;
    nxt_port_mmap_msg_t  records[2];
} nxt_mmap_read_case_t;


static const nxt_mmap_read_case_t  nxt_mmap_read_cases[] = {
    { "one record", 0, 1, 0, 1, { { 0, 0, 100 } } },
    { "two records", 0, 2, 0, 2,
      { { 0, 0, 100 }, { 0, 1, PORT_MMAP_CHUNK_SIZE } } },
    { "partial tail only", 0, 0, 5, 0, { { 0 } } },
    { "unknown mmap_id", 0, 1, 0, 0, { { 1, 0, 100 } } },
    { "huge mmap_id", 0, 1, 0, 0, { { 0xFFFFFFFF, 0, 100 } } },
    { "chunk_id past the data area", 0, 1, 0, 0,
      { { 0, PORT_MMAP_CHUNK_COUNT, 100 } } },
    { "size past the data area", 0, 1, 0, 0,
      { { 0, PORT_MMAP_CHUNK_COUNT - 1, PORT_MMAP_CHUNK_SIZE + 1 } } },
    { "size near UINT32_MAX", 0, 1, 0, 0, { { 0, 0, 0xFFFFFFFF } } },
    { "unknown sender", 1, 1, 0, 0, { { 0, 0, 100 } } },
};


static nxt_task_t  *nxt_mmap_read_task;
static nxt_port_t  *nxt_mmap_read_port;


static int
nxt_mmap_read_child(void *data)
{
    u_char                      *page, *start, *end;
    size_t                      psize, len;
    nxt_uint_t                  i, nbufs;
    nxt_buf_t                   *b, *in;
    nxt_port_recv_msg_t         msg;
    const nxt_mmap_read_case_t  *tc;

    tc = data;
    psize = getpagesize();

    page = mmap(NULL, 2 * psize, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED || mprotect(page + psize, psize, PROT_NONE) != 0) {
        return 1;
    }

    len = tc->nrecords * sizeof(nxt_port_mmap_msg_t);
    end = page + psize;
    start = end - len - tc->tail;

    /* A zero tail reads as mmap_id 0, which exists, so the overrun is real. */
    nxt_memcpy(start, tc->records, len);
    nxt_memzero(start + len, tc->tail);

    in = nxt_buf_mem_alloc(nxt_mmap_read_port->mem_pool, 0, 0);
    if (in == NULL) {
        return 1;
    }

    in->mem.start = start;
    in->mem.pos = start;
    in->mem.free = end;
    in->mem.end = end;

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));
    msg.port = nxt_mmap_read_port;
    msg.buf = in;
    msg.port_msg.pid = nxt_pid + 41 + tc->pid_off;
    msg.port_msg.mmap = 1;

    nxt_port_mmap_read(nxt_mmap_read_task, &msg);

    nbufs = 0;

    for (b = msg.buf; b != NULL && b != in; b = b->next) {
        nbufs++;
    }

    len = 0;

    for (i = 0; i < tc->expect_bufs; i++) {
        len += tc->records[i].size;
    }

    return nbufs != tc->expect_bufs || msg.size != len
           || (size_t) (in->mem.pos - start)
              != tc->expect_bufs * sizeof(nxt_port_mmap_msg_t);
}


nxt_int_t
nxt_port_mmap_read_test(nxt_thread_t *thr)
{
    nxt_mp_t                 *mp;
    nxt_int_t                ret;
    nxt_buf_t                *seg, *segs[3];
    nxt_uint_t               i, nsegs;
    nxt_bool_t               incoming_mutex;
    nxt_task_t               *task;
    nxt_process_t            *process;
    nxt_runtime_t            *rt, *saved_rt;
    nxt_chunk_id_t           c;
    nxt_event_engine_t       engine, *saved_engine;
    nxt_mmap_read_case_t     tc;
    nxt_port_mmap_handler_t  *mmap_handler;

    nxt_thread_time_update(thr);

    task = thr->task;
    task->thread = thr;
    nxt_mmap_read_task = task;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    process = nxt_mp_zalloc(mp, sizeof(nxt_process_t));

    if (nxt_slow_path(rt == NULL || process == NULL
                      || nxt_thread_mutex_create(&rt->processes_mutex)
                         != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    rt->mem_pool = mp;

    nxt_memzero(&engine, sizeof(engine));
    engine.mem_pool = mp;
    engine.task.thread = thr;
    engine.task.log = thr->log;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;
    thr->runtime = rt;
    thr->engine = &engine;

    ret = NXT_ERROR;
    nsegs = 0;
    incoming_mutex = 0;

    process->pid = nxt_pid + 41;
    process->use_count = 1;
    nxt_queue_init(&process->ports);

    if (nxt_slow_path(nxt_thread_mutex_create(&process->incoming.mutex)
                      != NXT_OK))
    {
        goto done;
    }

    incoming_mutex = 1;

    nxt_runtime_process_add(task, process);

    /* The sender's segment 0, allocated through the same array. */
    seg = nxt_port_mmap_get_buf(task, &process->incoming,
                                2 * PORT_MMAP_CHUNK_SIZE);
    if (nxt_slow_path(seg == NULL)) {
        goto done;
    }

    segs[nsegs++] = seg;

    mmap_handler = seg->parent;
    c = nxt_port_mmap_chunk_id(mmap_handler->hdr, seg->mem.pos);

    nxt_mmap_read_port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_ROUTER);
    if (nxt_slow_path(nxt_mmap_read_port == NULL)) {
        goto done;
    }

    nxt_mmap_read_port->pair[0] = -1;
    nxt_mmap_read_port->pair[1] = -1;
    nxt_mmap_read_port->socket.fd = -1;

    for (i = 0; i < nxt_nitems(nxt_mmap_read_cases); i++) {
        if (nxt_test_in_child(thr, nxt_mmap_read_cases[i].name,
                              nxt_mmap_read_child,
                              (void *) &nxt_mmap_read_cases[i])
            != 0)
        {
            nxt_log_alert(thr->log, "port mmap read test \"%s\" failed",
                          nxt_mmap_read_cases[i].name);
            goto done;
        }
    }

    /* A whole record followed by every possible partial tail. */

    tc = nxt_mmap_read_cases[0];

    for (tc.tail = 1; tc.tail < sizeof(nxt_port_mmap_msg_t); tc.tail++) {
        if (nxt_test_in_child(thr, "partial tail", nxt_mmap_read_child, &tc)
            != 0)
        {
            nxt_log_alert(thr->log, "port mmap read test: a %uz-byte tail "
                          "failed", tc.tail);
            goto done;
        }
    }

    /*
     * The peer maps the segment writable, so the busy sentinel past the
     * last chunk may read as free: nxt_port_mmap_increase_buf() must still
     * not grow a buffer past the data area.
     */
    seg = nxt_port_mmap_get_buf(task, &process->incoming,
                                (PORT_MMAP_CHUNK_COUNT - c - 2)
                                * PORT_MMAP_CHUNK_SIZE);
    if (seg != NULL) {
        segs[nsegs++] = seg;
    }

    if (nxt_slow_path(seg == NULL || seg->parent != mmap_handler)) {
        goto done;
    }

    for (i = 0; i < 4; i++) {
        nxt_port_mmap_set_chunk_free(mmap_handler->hdr->free_map,
                                     PORT_MMAP_CHUNK_COUNT + i);
    }

    (void) nxt_port_mmap_increase_buf(task, seg,
                                      nxt_buf_mem_free_size(&seg->mem)
                                      + 4 * PORT_MMAP_CHUNK_SIZE, 1);

    if (seg->mem.end > nxt_port_mmap_chunk_start(mmap_handler->hdr,
                                                 PORT_MMAP_CHUNK_COUNT))
    {
        nxt_log_alert(thr->log, "port mmap read test: increase_buf grew "
                      "past the data area");
        goto done;
    }

    /* Nor may nxt_port_mmap_get() continue from the last chunk past it. */
    nxt_port_mmap_set_chunk_free(mmap_handler->hdr->free_map,
                                 PORT_MMAP_CHUNK_COUNT - 1);

    seg = nxt_port_mmap_get_buf(task, &process->incoming,
                                2 * PORT_MMAP_CHUNK_SIZE);
    if (seg != NULL) {
        segs[nsegs++] = seg;
    }

    if (seg != NULL && seg->parent == mmap_handler
        && seg->mem.end > nxt_port_mmap_chunk_start(mmap_handler->hdr,
                                                    PORT_MMAP_CHUNK_COUNT))
    {
        nxt_log_alert(thr->log, "port mmap read test: get_buf ran past "
                      "the data area");
        goto done;
    }

    ret = NXT_OK;

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port mmap read test passed");

done:

    /*
     * Each nxt_port_mmap_get_buf() took a reference on its segment's
     * handler, and process->incoming holds one more.  The buffers are
     * never completed here, so their references are dropped by hand
     * (nxt_port_mmap_handler_use() is private to nxt_port_memory.c); the
     * array's reference, dropped last by nxt_port_mmaps_destroy(), then
     * unmaps and frees each handler.  The buffers themselves
     * come from mp.  The port has its own pool, freed by the last
     * reference; it was never linked to a process.  nxt_runtime_process_add()
     * put the process in rt->processes, a malloc'd table outside mp.
     */
    for (i = 0; i < nsegs; i++) {
        mmap_handler = segs[i]->parent;
        (void) nxt_atomic_fetch_add(&mmap_handler->use_count, -1);
    }

    nxt_port_mmaps_destroy(&process->incoming, 1);

    if (nxt_mmap_read_port != NULL) {
        nxt_port_use(task, nxt_mmap_read_port, -1);
        nxt_mmap_read_port = NULL;
    }

    if (process->registered) {
        nxt_runtime_process_remove(rt, process);
    }

    if (incoming_mutex) {
        nxt_thread_mutex_destroy(&process->incoming.mutex);
    }

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    return ret;
}
