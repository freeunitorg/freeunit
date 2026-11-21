
/*
 * Copyright (C) FreeUnit Community
 */

/*
 * The records of the schedules in the main process
 * (src/nxt_schedule_main.c) and the rules R1 to R3 of src/nxt_schedule.h.
 *
 * The test is the main process here.  It forks the process of a run
 * itself, a plain child, and gives its pid to the code that main runs
 * after its own fork.  The slot, the deadline, the kill and the reap are
 * the code of main.  The deadline runs from a real timer: the test moves
 * the clock of the engine past the timeout.  The test reaps each child
 * with waitpid() and gives the status to the function that the SIGCHLD
 * handler of main calls.
 *
 * A child of the test makes a process group of its own, as a run does,
 * and can fork one more member of the group that it does not wait for.
 * Both ignore SIGTERM and hold the write end of a pipe.  The test holds
 * the read end, so the end of file there means that every member of the
 * group is gone.
 *
 * Cases:
 *
 *   - R3: a second run of a schedule is refused while the first is not
 *     reaped, and counted as skipped.  Another schedule is not affected.
 *   - R2: at the timeout the whole group is killed, also the member that
 *     the leader did not wait for.
 *   - R1: the kill does not free the slot.  A run is still refused until
 *     the status of waitpid() is given, and it starts right after.
 *   - A killed run counts as timed out, not as failed, and main knows that
 *     the SIGKILL was its own.
 *   - A process that is not main does not act on a deadline: a child of
 *     main inherits its timers.
 *   - A run that exits with a status other than zero counts as failed.
 *   - A schedule that leaves the configuration keeps its slot while its
 *     run is not reaped, and its record goes away with the reap.
 *   - When main exits, the group of a run is killed, and that run counts
 *     neither as failed nor as timed out.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_event_engine.h>
#include <nxt_schedule.h>
#include "nxt_tests.h"

#include <poll.h>
#include <sys/wait.h>


static nxt_str_t  nxt_schedule_main_test_a = nxt_string("a");
static nxt_str_t  nxt_schedule_main_test_b = nxt_string("b.B_b-1");


/* See nxt_router_app_timeout_test_tick(). */

static void
nxt_schedule_main_test_tick(nxt_task_t *task, nxt_event_engine_t *engine,
    nxt_msec_t advance)
{
    nxt_task_t          *wq_task;
    nxt_work_handler_t  handler;
    void                *obj, *data;

    (void) nxt_timer_find(engine);

    nxt_timer_expire(engine, engine->timers.now + advance);

    /* nxt_work_queue_pop() dereferences the head unconditionally. */

    while (engine->fast_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine->fast_work_queue, &wq_task, &obj,
                                     &data);

        if (handler == NULL) {
            continue;
        }

        handler(wq_task != NULL ? wq_task : task, obj, data);
    }
}


/*
 * Fork a child that stands for a run.  With "member" it forks one more
 * process of its group.  With "code" below zero the child waits to be
 * killed; otherwise it exits with that status once the test has it.
 * "*fd" is the read end of the pipe that all of them hold.
 */
static nxt_pid_t
nxt_schedule_main_test_fork(nxt_thread_t *thr, nxt_bool_t member, int code,
    int *fd)
{
    int        pp[2];
    char       ch;
    ssize_t    n;
    nxt_pid_t  pid;

    if (nxt_slow_path(pipe(pp) != 0)) {
        nxt_log_alert(thr->log, "schedule main test: pipe() failed %E",
                      nxt_errno);
        return -1;
    }

    pid = fork();

    if (nxt_slow_path(pid < 0)) {
        nxt_log_alert(thr->log, "schedule main test: fork() failed %E",
                      nxt_errno);
        (void) close(pp[0]);
        (void) close(pp[1]);

        return -1;
    }

    if (pid == 0) {
        (void) close(pp[0]);

        (void) signal(SIGTERM, SIG_IGN);

        if (setpgid(0, 0) != 0) {
            _exit(100);
        }

        if (member) {
            pid = fork();

            if (pid < 0) {
                _exit(101);
            }

            if (pid == 0) {
                for ( ;; ) {
                    (void) pause();
                }
            }
        }

        /* The group exists now. */

        if (write(pp[1], "r", 1) != 1) {
            _exit(102);
        }

        if (code >= 0) {
            _exit(code);
        }

        for ( ;; ) {
            (void) pause();
        }
    }

    (void) close(pp[1]);

    do {
        n = read(pp[0], &ch, 1);
    } while (n == -1 && nxt_errno == NXT_EINTR);

    if (nxt_slow_path(n != 1)) {
        nxt_log_alert(thr->log, "schedule main test: child %PI did not "
                      "start", pid);
        (void) kill(pid, SIGKILL);
        (void) waitpid(pid, NULL, 0);
        (void) close(pp[0]);

        return -1;
    }

    *fd = pp[0];

    return pid;
}


/* Whether every holder of the write end is gone within "msec". */

static nxt_bool_t
nxt_schedule_main_test_gone(int fd, int msec)
{
    char           ch;
    ssize_t        n;
    struct pollfd  pfd;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (poll(&pfd, 1, msec) <= 0) {
        return 0;
    }

    n = read(fd, &ch, 1);

    return (n == 0);
}


static nxt_int_t
nxt_schedule_main_test_wait(nxt_thread_t *thr, nxt_pid_t pid, int *status)
{
    nxt_pid_t  ret;

    do {
        ret = waitpid(pid, status, 0);
    } while (ret == -1 && nxt_errno == NXT_EINTR);

    if (nxt_slow_path(ret != pid)) {
        nxt_log_alert(thr->log, "schedule main test: waitpid(%PI) failed %E",
                      pid, nxt_errno);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/* Kill what a failed case can leave, and forget the record. */

static void
nxt_schedule_main_test_cleanup(nxt_task_t *task, nxt_pid_t pid, int fd)
{
    int  status;

    if (pid > 1) {
        (void) kill(-pid, SIGKILL);
        (void) kill(pid, SIGKILL);

        if (waitpid(pid, &status, 0) == pid) {
            (void) nxt_schedule_main_exited(task, pid, status);
        }
    }

    if (fd != -1) {
        (void) close(fd);
    }
}


/* R3, R2 and R1 with a run that its timeout ends. */

static nxt_int_t
nxt_schedule_main_test_timeout(nxt_thread_t *thr, nxt_task_t *task,
    nxt_event_engine_t *engine)
{
    int                          fd, fd2, status;
    nxt_int_t                    ret;
    nxt_pid_t                    pid, pid2;
    const nxt_str_t              *name;
    const nxt_schedule_status_t  *st;

    name = &nxt_schedule_main_test_a;

    ret = NXT_ERROR;
    fd = -1;
    fd2 = -1;
    pid2 = -1;

    pid = nxt_schedule_main_test_fork(thr, 1, -1, &fd);
    if (pid == -1) {
        return NXT_ERROR;
    }

    if (!nxt_schedule_main_test_start(task, name, 1, pid)) {
        nxt_log_alert(thr->log, "schedule main test: the first run of a "
                      "schedule was refused");
        goto done;
    }

    st = nxt_schedule_main_test_status(name);

    if (st == NULL || st->pid != pid || !st->running || st->runs != 1
        || st->skipped != 0 || st->last_start == 0)
    {
        nxt_log_alert(thr->log, "schedule main test: bad state of a run "
                      "that has started");
        goto done;
    }

    /* R3: no second run of this schedule; another schedule is free. */

    if (nxt_schedule_main_test_start(task, name, 1, pid)
        || st->skipped != 1 || st->runs != 1 || st->pid != pid)
    {
        nxt_log_alert(thr->log, "schedule main test: R3: a second run of a "
                      "schedule started while the first was not reaped "
                      "(skipped %uL, runs %uL)", st->skipped, st->runs);
        goto done;
    }

    pid2 = nxt_schedule_main_test_fork(thr, 0, 0, &fd2);
    if (pid2 == -1) {
        goto done;
    }

    if (!nxt_schedule_main_test_start(task, &nxt_schedule_main_test_b, 0,
                                      pid2))
    {
        nxt_log_alert(thr->log, "schedule main test: a run of one schedule "
                      "held the slot of another");
        goto done;
    }

    /* Before the timeout nothing is killed. */

    nxt_schedule_main_test_tick(task, engine, 999);

    if (nxt_schedule_main_test_gone(fd, 100)) {
        nxt_log_alert(thr->log, "schedule main test: the run was killed "
                      "before its timeout");
        goto done;
    }

    /* R2: at the timeout the whole group is killed. */

    nxt_schedule_main_test_tick(task, engine, 1);

    if (!nxt_schedule_main_test_gone(fd, 5000)) {
        nxt_log_alert(thr->log, "schedule main test: R2: a process of the "
                      "group of the run is alive 5 s after the timeout");
        goto done;
    }

    /* R1: the kill is not the reap; the slot is still busy. */

    if (st->pid != pid || !st->running
        || nxt_schedule_main_test_start(task, name, 1, pid)
        || st->skipped != 2)
    {
        nxt_log_alert(thr->log, "schedule main test: R1: the slot was free "
                      "after the kill and before waitpid() (pid %PI, "
                      "running %d, skipped %uL)", st->pid, (int) st->running,
                      st->skipped);
        goto done;
    }

    if (nxt_schedule_main_test_wait(thr, pid, &status) != NXT_OK) {
        goto done;
    }

    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL
        || !nxt_schedule_main_killed(pid, status))
    {
        nxt_log_alert(thr->log, "schedule main test: the run did not end on "
                      "the SIGKILL of main (status 0x%xd)", status);
        goto done;
    }

    if (!nxt_schedule_main_exited(task, pid, status)) {
        nxt_log_alert(thr->log, "schedule main test: the reaped process %PI "
                      "was not taken as a run", pid);
        goto done;
    }

    pid = -1;

    if (st->pid != 0 || st->running || st->timed_out != 1 || st->failed != 0
        || st->last_exit != -1 || st->last_signal != SIGKILL)
    {
        nxt_log_alert(thr->log, "schedule main test: bad state after a "
                      "timeout: pid %PI, timed_out %uL, failed %uL, exit %D, "
                      "signal %D", st->pid, st->timed_out, st->failed,
                      st->last_exit, st->last_signal);
        goto done;
    }

    /* The other schedule: its run exited with 0 by itself. */

    if (nxt_schedule_main_test_wait(thr, pid2, &status) != NXT_OK
        || !nxt_schedule_main_exited(task, pid2, status))
    {
        goto done;
    }

    pid2 = -1;

    st = nxt_schedule_main_test_status(&nxt_schedule_main_test_b);

    if (st == NULL || st->pid != 0 || st->failed != 0 || st->timed_out != 0
        || st->last_exit != 0 || st->last_signal != 0)
    {
        nxt_log_alert(thr->log, "schedule main test: bad state after a run "
                      "that exited with 0");
        goto done;
    }

    /* R1: and no longer.  The slot is free right after the reap. */

    (void) close(fd);
    fd = -1;

    pid = nxt_schedule_main_test_fork(thr, 0, 3, &fd);
    if (pid == -1) {
        goto done;
    }

    if (!nxt_schedule_main_test_start(task, name, 1, pid)) {
        nxt_log_alert(thr->log, "schedule main test: R1: the slot was busy "
                      "after waitpid()");
        goto done;
    }

    if (nxt_schedule_main_test_wait(thr, pid, &status) != NXT_OK
        || !nxt_schedule_main_exited(task, pid, status))
    {
        goto done;
    }

    pid = -1;

    st = nxt_schedule_main_test_status(name);

    /* Status 3: failed, and the timer of this run must not fire later. */

    nxt_schedule_main_test_tick(task, engine, 5000);

    if (st == NULL || st->runs != 2 || st->failed != 1 || st->timed_out != 1
        || st->last_exit != 3 || st->last_signal != 0 || st->pid != 0)
    {
        nxt_log_alert(thr->log, "schedule main test: bad state after a run "
                      "that exited with 3");
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_schedule_main_test_cleanup(task, pid, fd);
    nxt_schedule_main_test_cleanup(task, pid2, fd2);

    return ret;
}


/* A deadline in a process that is not main does nothing. */

static nxt_int_t
nxt_schedule_main_test_not_main(nxt_thread_t *thr, nxt_task_t *task,
    nxt_event_engine_t *engine, nxt_runtime_t *rt)
{
    int                          fd, status;
    nxt_int_t                    ret;
    nxt_pid_t                    pid;
    const nxt_str_t              *name;
    const nxt_schedule_status_t  *st;

    name = &nxt_schedule_main_test_a;

    ret = NXT_ERROR;
    fd = -1;

    pid = nxt_schedule_main_test_fork(thr, 0, -1, &fd);
    if (pid == -1) {
        return NXT_ERROR;
    }

    if (!nxt_schedule_main_test_start(task, name, 2, pid)) {
        goto done;
    }

    rt->type = NXT_PROCESS_PROTOTYPE;

    nxt_schedule_main_test_tick(task, engine, 2000);

    rt->type = NXT_PROCESS_MAIN;

    if (nxt_schedule_main_test_gone(fd, 300)) {
        nxt_log_alert(thr->log, "schedule main test: a process that is not "
                      "main killed a run at its deadline");
        goto done;
    }

    /* Main exits: the run is killed, and it is no failure and no timeout. */

    nxt_schedule_main_stop(task);

    if (!nxt_schedule_main_test_gone(fd, 5000)) {
        nxt_log_alert(thr->log, "schedule main test: a run is alive 5 s "
                      "after main started to exit");
        goto done;
    }

    if (!nxt_schedule_main_running()) {
        nxt_log_alert(thr->log, "schedule main test: main does not wait for "
                      "a run that it killed");
        goto done;
    }

    if (nxt_schedule_main_test_wait(thr, pid, &status) != NXT_OK) {
        goto done;
    }

    if (!nxt_schedule_main_killed(pid, status)
        || !nxt_schedule_main_exited(task, pid, status))
    {
        nxt_log_alert(thr->log, "schedule main test: the run that main "
                      "stopped was not taken as its own");
        goto done;
    }

    pid = -1;

    st = nxt_schedule_main_test_status(name);

    if (st == NULL || st->failed != 1 || st->timed_out != 1
        || nxt_schedule_main_running())
    {
        nxt_log_alert(thr->log, "schedule main test: a run that main "
                      "stopped was counted as failed or timed out");
        goto done;
    }

    ret = NXT_OK;

done:

    rt->type = NXT_PROCESS_MAIN;

    nxt_schedule_main_test_set_stopping(0);

    nxt_schedule_main_test_cleanup(task, pid, fd);

    return ret;
}


/* A deadline that comes due after main stopped a run is not a timeout. */

static nxt_int_t
nxt_schedule_main_test_stopped(nxt_thread_t *thr, nxt_task_t *task,
    nxt_event_engine_t *engine)
{
    int                          fd, status;
    nxt_int_t                    ret;
    nxt_pid_t                    pid;
    nxt_uint_t                   timed_out;
    const nxt_str_t              *name;
    const nxt_schedule_status_t  *st;

    name = &nxt_schedule_main_test_a;

    ret = NXT_ERROR;
    fd = -1;

    pid = nxt_schedule_main_test_fork(thr, 0, -1, &fd);
    if (pid == -1) {
        return NXT_ERROR;
    }

    if (!nxt_schedule_main_test_start(task, name, 2, pid)) {
        goto done;
    }

    st = nxt_schedule_main_test_status(name);
    timed_out = (st != NULL) ? st->timed_out : 0;

    /* Main exits: the run is killed, and its deadline is still armed. */

    nxt_schedule_main_stop(task);

    /* The deadline comes due before main reaps the run. */

    nxt_schedule_main_test_tick(task, engine, 2000);

    if (nxt_schedule_main_test_wait(thr, pid, &status) != NXT_OK) {
        goto done;
    }

    if (!nxt_schedule_main_killed(pid, status)
        || !nxt_schedule_main_exited(task, pid, status))
    {
        nxt_log_alert(thr->log, "schedule main test: a run stopped before "
                      "its deadline was not taken as stopped");
        goto done;
    }

    pid = -1;

    st = nxt_schedule_main_test_status(name);

    if (st == NULL || st->timed_out != timed_out) {
        nxt_log_alert(thr->log, "schedule main test: a deadline after a stop "
                      "was counted as a timeout");
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_schedule_main_test_set_stopping(0);

    nxt_schedule_main_test_cleanup(task, pid, fd);

    return ret;
}


/* A schedule that leaves the configuration while its run is not reaped. */

static nxt_int_t
nxt_schedule_main_test_removed(nxt_thread_t *thr, nxt_task_t *task,
    nxt_event_engine_t *engine)
{
    int              fd, status;
    nxt_int_t        ret;
    nxt_pid_t        pid;
    const nxt_str_t  *name;

    static u_char  only_a[] = "a";

    name = &nxt_schedule_main_test_b;

    ret = NXT_ERROR;
    fd = -1;

    pid = nxt_schedule_main_test_fork(thr, 0, -1, &fd);
    if (pid == -1) {
        return NXT_ERROR;
    }

    if (!nxt_schedule_main_test_start(task, name, 0, pid)) {
        goto done;
    }

    /* The configuration now has "a" only. */

    nxt_schedule_main_test_conf(task, only_a, only_a + sizeof(only_a));

    if (nxt_schedule_main_test_status(name) == NULL
        || nxt_schedule_main_test_status(&nxt_schedule_main_test_a) == NULL)
    {
        nxt_log_alert(thr->log, "schedule main test: the record of a "
                      "schedule with a run, or of a schedule that stays, "
                      "went away with the configuration");
        goto done;
    }

    (void) kill(pid, SIGKILL);

    if (nxt_schedule_main_test_wait(thr, pid, &status) != NXT_OK
        || !nxt_schedule_main_exited(task, pid, status))
    {
        goto done;
    }

    pid = -1;

    /* The record went with the reap; its memory after the timer changes. */

    nxt_schedule_main_test_tick(task, engine, 1);

    if (nxt_schedule_main_test_status(name) != NULL) {
        nxt_log_alert(thr->log, "schedule main test: the record of a "
                      "removed schedule stayed after its run was reaped");
        goto done;
    }

    /* A record without a run goes at once. */

    nxt_schedule_main_test_conf(task, only_a, only_a);
    nxt_schedule_main_test_tick(task, engine, 1);

    if (nxt_schedule_main_test_status(&nxt_schedule_main_test_a) != NULL) {
        nxt_log_alert(thr->log, "schedule main test: the record of a "
                      "removed schedule without a run stayed");
        goto done;
    }

    if (nxt_schedule_main_exited(task, 1, 0)
        || nxt_schedule_main_killed(1, SIGKILL))
    {
        nxt_log_alert(thr->log, "schedule main test: a process that is not "
                      "a run was taken as one");
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_schedule_main_test_cleanup(task, pid, fd);

    return ret;
}


nxt_int_t
nxt_schedule_main_test(nxt_thread_t *thr)
{
    nxt_int_t           ret;
    nxt_task_t          *task;
    nxt_runtime_t       rt, *saved_rt;
    nxt_event_engine_t  engine, *saved_engine;

    static u_char  none[] = "";

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "schedule main test started");

    task = thr->task;
    task->thread = thr;

    /* No router port: main has nobody to send the states to. */

    nxt_memzero(&rt, sizeof(nxt_runtime_t));
    rt.type = NXT_PROCESS_MAIN;

    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    engine.task.thread = thr;
    engine.task.log = thr->log;

    if (nxt_slow_path(nxt_timers_init(&engine.timers, 64) != NXT_OK)) {
        nxt_work_queue_cache_destroy(&engine.work_queue_cache);
        return NXT_ERROR;
    }

    saved_rt = thr->runtime;
    saved_engine = thr->engine;

    thr->runtime = &rt;
    thr->engine = &engine;

    ret = nxt_schedule_main_test_timeout(thr, task, &engine);

    if (ret == NXT_OK) {
        ret = nxt_schedule_main_test_not_main(thr, task, &engine, &rt);
    }

    if (ret == NXT_OK) {
        ret = nxt_schedule_main_test_stopped(thr, task, &engine);
    }

    if (ret == NXT_OK) {
        ret = nxt_schedule_main_test_removed(thr, task, &engine);
    }

    /* Forget every record, also after a case that failed. */

    nxt_schedule_main_test_conf(task, none, none);
    nxt_schedule_main_test_tick(task, &engine, 1);

    thr->engine = saved_engine;
    thr->runtime = saved_rt;

    nxt_free(engine.timers.changes);
    nxt_work_queue_cache_destroy(&engine.work_queue_cache);

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "schedule main test passed");
    }

    return ret;
}
