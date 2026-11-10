/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_process_use(): a drop that leaves a reference takes no lock.  A
 * second thread holds rt->processes_mutex while this thread drops two of
 * three references.  Before the change each drop waited for the mutex, and
 * the alarm killed the child.  The lookup then still finds the process and
 * takes a reference under the mutex.  Runs in a child, so a crash or a hang
 * is a failure.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include "nxt_tests.h"


typedef struct {
    nxt_runtime_t  *rt;
    int            locked[2];
    int            go[2];
} nxt_process_use_test_t;


static void *
nxt_process_use_test_holder(void *data)
{
    char                    c;
    nxt_process_use_test_t  *t;

    t = data;

    /* Not nxt_thread_mutex_lock(): its debug log needs an nxt_thread_t. */
    (void) pthread_mutex_lock(&t->rt->processes_mutex);

    c = 'l';
    (void) write(t->locked[1], &c, 1);
    (void) read(t->go[0], &c, 1);

    (void) pthread_mutex_unlock(&t->rt->processes_mutex);

    return NULL;
}


static int
nxt_process_use_test_child(void *data)
{
    char                    c;
    pthread_t               holder;
    nxt_task_t              *task;
    nxt_thread_t            *thr;
    nxt_process_t           *process, *found;
    nxt_process_use_test_t  t;

    t.rt = data;
    thr = nxt_thread();
    thr->runtime = t.rt;
    thr->engine = NULL;
    task = thr->task;

    process = nxt_mp_zalloc(t.rt->mem_pool, sizeof(nxt_process_t));
    if (process == NULL) {
        return 2;
    }

    process->pid = nxt_pid + 71;
    process->use_count = 3;
    nxt_queue_init(&process->ports);

    nxt_runtime_process_add(task, process);

    if (pipe(t.locked) != 0 || pipe(t.go) != 0) {
        return 2;
    }

    if (pthread_create(&holder, NULL, nxt_process_use_test_holder, &t) != 0) {
        return 2;
    }

    if (read(t.locked[0], &c, 1) != 1) {
        return 2;
    }

    /* SIGALRM kills the child if a drop waits for the held mutex. */
    alarm(10);

    nxt_process_use(task, process, -1);
    nxt_process_use(task, process, -1);

    alarm(0);

    c = 'g';
    (void) write(t.go[1], &c, 1);
    (void) pthread_join(holder, NULL);

    if (nxt_atomic_load(&process->use_count) != 1) {
        return 3;
    }

    found = nxt_runtime_process_ref(t.rt, process->pid);

    if (found != process || nxt_atomic_load(&process->use_count) != 2) {
        return 4;
    }

    nxt_process_use(task, process, -1);

    return (nxt_atomic_load(&process->use_count) != 1) ? 5 : 0;
}


nxt_int_t
nxt_process_use_test(nxt_thread_t *thr)
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

    rc = nxt_test_in_child(thr, "process use test",
                           nxt_process_use_test_child, rt);

    nxt_mp_destroy(mp);

    NXT_TEST_CHECK(thr->log, rc == 0, "process use test failed at step %d",
                   rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "process use test passed");

    return NXT_OK;
}
