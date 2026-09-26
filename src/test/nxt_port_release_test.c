
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_port_release() of a port linked into a process's list by hand, with
 * no port->process and no reference (#425): a debug build aborted and a
 * release build dereferenced NULL.  It must unlink the port and drop no
 * reference it does not hold.  Runs in a child, so a crash is a failure.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include "nxt_tests.h"


static int
nxt_port_release_test_child(void *data)
{
    nxt_task_t     *task;
    nxt_port_t     *port, *paired;
    nxt_thread_t   *thr;
    nxt_runtime_t  *rt;
    nxt_process_t  *process;

    rt = data;
    thr = nxt_thread();
    thr->runtime = rt;
    thr->engine = NULL;
    task = thr->task;

    process = nxt_mp_zalloc(rt->mem_pool, sizeof(nxt_process_t));
    if (process == NULL) {
        return 2;
    }

    process->pid = nxt_pid + 53;
    process->use_count = 1;
    nxt_queue_init(&process->ports);

    nxt_runtime_process_add(task, process);

    /* A correctly paired port first: its release drops its reference. */

    paired = nxt_port_new(task, 1, process->pid, NXT_PROCESS_APP);
    if (paired == NULL) {
        return 2;
    }

    paired->pair[0] = -1;
    paired->pair[1] = -1;
    paired->socket.fd = -1;

    nxt_process_port_add(task, process, paired);

    if (process->use_count != 2) {
        return 3;
    }

    nxt_port_use(task, paired, -1);

    if (process->use_count != 1 || !nxt_queue_is_empty(&process->ports)) {
        return 4;
    }

    /* Linked by hand: no port->process, no reference. */

    port = nxt_port_new(task, 2, process->pid, NXT_PROCESS_APP);
    if (port == NULL) {
        return 2;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    nxt_queue_insert_tail(&process->ports, &port->link);

    nxt_port_use(task, port, -1);

    /* A port still on the list would point into the freed pool. */
    return !nxt_queue_is_empty(&process->ports) ? 5
           : process->use_count != 1 ? 6 : 0;
}


nxt_int_t
nxt_port_release_test(nxt_thread_t *thr)
{
    int            rc;
    nxt_mp_t       *mp;
    nxt_runtime_t  *rt;

    thr->task->thread = thr;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));

    if (nxt_slow_path(rt == NULL
                      || nxt_thread_mutex_create(&rt->processes_mutex)
                         != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    rt->mem_pool = mp;

    rc = nxt_test_in_child(thr, "port release test",
                           nxt_port_release_test_child, rt);

    nxt_mp_destroy(mp);

    NXT_TEST_CHECK(thr->log, rc == 0, "port release test failed at step %d",
                   rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port release test passed");

    return NXT_OK;
}
