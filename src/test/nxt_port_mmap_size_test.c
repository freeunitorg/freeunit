/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * The segment size check in nxt_port_incoming_port_mmap()
 * (src/nxt_port_memory.c).  A peer truncates a segment to exactly
 * PORT_MMAP_SIZE, but macOS rounds a shm object up to a whole page, so
 * fstat() reports 10502144 there instead of 10489856 (issue 595).  A segment
 * that size, or larger, must be accepted.  A shorter one must be refused.
 *
 * Each segment is a sparse memfd or shm file; only its header page is
 * touched.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_memory_int.h>
#include "nxt_tests.h"


static nxt_int_t
nxt_port_mmap_size_test_one(nxt_task_t *task, nxt_process_t *process,
    size_t size, uint32_t id, nxt_bool_t accept)
{
    void                     *mem;
    nxt_fd_t                 fd;
    nxt_port_mmap_header_t   *hdr;
    nxt_port_mmap_handler_t  *mmap_handler;

    fd = nxt_shm_open(task, size);
    if (fd == -1) {
        return NXT_ERROR;
    }

    mem = nxt_mem_mmap(NULL, PORT_MMAP_HEADER_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        nxt_fd_close(fd);
        return NXT_ERROR;
    }

    hdr = mem;
    hdr->id = id;
    hdr->src_pid = process->pid;
    hdr->dst_pid = nxt_pid;

    nxt_mem_munmap(mem, PORT_MMAP_HEADER_SIZE);

    mmap_handler = nxt_port_incoming_port_mmap(task, process, fd);

    nxt_fd_close(fd);

    if ((mmap_handler != NULL) != accept) {
        nxt_log_alert(task->log, "port mmap size test: a segment of %uz "
                      "bytes was %s", size, accept ? "refused" : "accepted");
        return NXT_ERROR;
    }

    return NXT_OK;
}


nxt_int_t
nxt_port_mmap_size_test(nxt_thread_t *thr)
{
    nxt_int_t      ret;
    nxt_task_t     *task;
    nxt_process_t  process;

    nxt_thread_time_update(thr);

    task = thr->task;
    task->thread = thr;

    nxt_memzero(&process, sizeof(process));
    process.pid = nxt_pid;

    if (nxt_thread_mutex_create(&process.incoming.mutex) != NXT_OK) {
        return NXT_ERROR;
    }

    ret = NXT_ERROR;

    /*
     * The short segment is a whole 64 KiB page short, not one byte: a shm
     * object is rounded up to a page on macOS, and a one-byte gap would
     * vanish there.  10424320 rounds up to at most 10485760 for any page
     * up to 64 KiB, still below PORT_MMAP_SIZE.
     */
    if (nxt_port_mmap_size_test_one(task, &process, PORT_MMAP_SIZE - 65536,
                                    1, 0)
        != NXT_OK
        || nxt_port_mmap_size_test_one(task, &process, PORT_MMAP_SIZE, 2, 1)
           != NXT_OK
        || nxt_port_mmap_size_test_one(task, &process,
                                       nxt_align_size(PORT_MMAP_SIZE, 16384),
                                       3, 1)
           != NXT_OK
        || nxt_port_mmap_size_test_one(task, &process, 2 * PORT_MMAP_SIZE, 4,
                                       1)
           != NXT_OK)
    {
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port mmap size test passed");

    ret = NXT_OK;

done:

    nxt_port_mmaps_destroy(&process.incoming, 1);

    nxt_thread_mutex_destroy(&process.incoming.mutex);

    return ret;
}
