
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * NXT_PORT_MMAPS_MAX on the router side (src/nxt_port_memory.c).  The
 * array of segments is grown to hold the id it is asked for, and an
 * incoming id is the peer's.  nxt_port_mmap_at() used to grow the array for
 * any id below UINT32_MAX, so one message with mmap_id 100000000 cost an
 * allocation and initialisation of that many slots.  Now the last id below
 * the limit is accepted, the limit and everything past it are refused
 * before any growth, and nxt_port_new_port_mmap() does not create a segment
 * whose id would reach the limit.
 *
 * Cost: the boundary cases grow the array to NXT_PORT_MMAPS_MAX slots of
 * one pointer, 512 KiB, and create one real segment of PORT_MMAP_SIZE
 * (about 10 MiB) so that the last allowed id is really created.  The
 * segment is a sparse memfd or shm file and only its header page is
 * touched, so the test takes well under a millisecond and no measurable
 * RSS; it needs 10 MiB of /dev/shm on paper only.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_memory_int.h>
#include "nxt_tests.h"


nxt_int_t
nxt_port_mmaps_max_test(nxt_thread_t *thr)
{
    nxt_int_t                ret;
    nxt_task_t               *task;
    nxt_port_mmap_t          *port_mmap;
    nxt_port_mmaps_t         mmaps;
    nxt_port_mmap_handler_t  *mmap_handler;

    nxt_thread_time_update(thr);

    task = thr->task;
    task->thread = thr;

    nxt_memzero(&mmaps, sizeof(mmaps));

    if (nxt_slow_path(nxt_thread_mutex_create(&mmaps.mutex) != NXT_OK)) {
        return NXT_ERROR;
    }

    ret = NXT_ERROR;

    /* A huge id is refused before the array is touched. */

    port_mmap = nxt_port_test_mmap_at(&mmaps, 100000000);

    if (port_mmap != NULL || mmaps.elts != NULL || mmaps.cap != 0
        || mmaps.size != 0)
    {
        nxt_log_alert(thr->log, "port mmaps max test: id 100000000 was "
                      "accepted, or the array was grown for it");
        goto done;
    }

    port_mmap = nxt_port_test_mmap_at(&mmaps, NXT_PORT_MMAPS_MAX);

    if (port_mmap != NULL || mmaps.elts != NULL || mmaps.cap != 0) {
        nxt_log_alert(thr->log, "port mmaps max test: id %uD was accepted",
                      NXT_PORT_MMAPS_MAX);
        goto done;
    }

    port_mmap = nxt_port_test_mmap_at(&mmaps, UINT32_MAX);

    if (port_mmap != NULL || mmaps.elts != NULL || mmaps.cap != 0) {
        nxt_log_alert(thr->log, "port mmaps max test: id UINT32_MAX was "
                      "accepted");
        goto done;
    }

    /* An id just below the limit grows the array to MAX - 1 slots. */

    port_mmap = nxt_port_test_mmap_at(&mmaps, NXT_PORT_MMAPS_MAX - 2);

    if (port_mmap == NULL || mmaps.size != NXT_PORT_MMAPS_MAX - 1
        || port_mmap != mmaps.elts + NXT_PORT_MMAPS_MAX - 2
        || port_mmap->mmap_handler != NULL)
    {
        nxt_log_alert(thr->log, "port mmaps max test: id %uD was refused",
                      NXT_PORT_MMAPS_MAX - 2);
        goto done;
    }

    /*
     * The segment created now gets id NXT_PORT_MMAPS_MAX - 1, the last one
     * allowed.
     */

    mmap_handler = nxt_port_test_new_port_mmap(task, &mmaps, 1);

    if (mmap_handler == NULL || mmaps.size != NXT_PORT_MMAPS_MAX
        || mmap_handler->hdr->id != NXT_PORT_MMAPS_MAX - 1
        || mmaps.elts[NXT_PORT_MMAPS_MAX - 1].mmap_handler != mmap_handler)
    {
        nxt_log_alert(thr->log, "port mmaps max test: segment %uD was not "
                      "created", NXT_PORT_MMAPS_MAX - 1);
        goto done;
    }

    /* The next one would get the limit as its id: refused, nothing grows. */

    mmap_handler = nxt_port_test_new_port_mmap(task, &mmaps, 1);

    if (mmap_handler != NULL || mmaps.size != NXT_PORT_MMAPS_MAX) {
        nxt_log_alert(thr->log, "port mmaps max test: segment %uD was "
                      "created", NXT_PORT_MMAPS_MAX);
        goto done;
    }

    port_mmap = nxt_port_test_mmap_at(&mmaps, NXT_PORT_MMAPS_MAX);

    if (port_mmap != NULL || mmaps.size != NXT_PORT_MMAPS_MAX) {
        nxt_log_alert(thr->log, "port mmaps max test: id %uD was accepted "
                      "into a full array", NXT_PORT_MMAPS_MAX);
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port mmaps max test passed");

    ret = NXT_OK;

done:

    nxt_port_mmaps_destroy(&mmaps, 1);

    nxt_thread_mutex_destroy(&mmaps.mutex);

    return ret;
}
