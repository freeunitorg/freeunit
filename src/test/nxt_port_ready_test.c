/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for descriptor ownership in
 * nxt_port_process_ready_handler() (src/nxt_port.c).
 *
 * PROCESS_READY normally carries the queue descriptor of the process that
 * announces itself, and the handler hands it to that process's port.  A
 * message naming a pid the runtime does not know took an early return that
 * closed nothing, and a repeated READY re-ran the mapping and dropped the
 * previous one -- both leak a descriptor of the receiver, which is the
 * main or the prototype process.  The dispatcher does not reclaim fds once
 * the handler returns (see nxt_port_recv_msg_close_fds()), so a peer that
 * attaches descriptors to forged messages can exhaust a privileged
 * process's descriptor table.
 *
 * A message can carry two descriptors regardless of how many its type
 * normally uses, so the test attaches both and expects both back.
 *
 * The handler also authenticates the sender against the isolated_pid of
 * the process the message names, wherever the platform hands the receiver
 * a kernel-validated pid (NXT_USE_CMSG_PID).  The fixture models a
 * pid-isolated process -- pid and isolated_pid deliberately differ -- so
 * that the cases below can tell the two apart: a credential equal to the
 * global pid has to be refused just like a foreign one.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_queue.h>
#include <nxt_runtime.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>

#if (NXT_HAVE_MEMFD_CREATE)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif


static nxt_bool_t
nxt_port_ready_test_fd_is_open(nxt_fd_t fd)
{
    return fcntl(fd, F_GETFD) != -1;
}


static nxt_int_t
nxt_port_ready_test_case(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_recv_msg_t *msg, const char *name)
{
    nxt_fd_t  fd0, fd1;

    fd0 = open("/dev/null", O_RDONLY);
    fd1 = open("/dev/null", O_RDONLY);

    if (fd0 == -1 || fd1 == -1) {
        nxt_log_alert(thr->log, "port ready test failed to open /dev/null");
        goto fail;
    }

    msg->fd[0] = fd0;
    msg->fd[1] = fd1;

    nxt_port_process_ready_handler(task, msg);

    if (nxt_port_ready_test_fd_is_open(fd0)
        || nxt_port_ready_test_fd_is_open(fd1))
    {
        nxt_log_alert(thr->log, "port ready test: %s leaked a descriptor",
                      name);
        goto fail;   /* closes only what is still open */
    }

    if (msg->fd[0] != -1 || msg->fd[1] != -1) {
        nxt_log_alert(thr->log, "port ready test: %s left fds in the message",
                      name);
        return NXT_ERROR;
    }

    return NXT_OK;

fail:

    /*
     * Close only what the handler left open: the descriptors it consumed
     * are gone already, and closing them again could reach an unrelated
     * descriptor that reused the number.
     */
    if (fd0 != -1 && nxt_port_ready_test_fd_is_open(fd0)) {
        (void) close(fd0);
    }

    if (fd1 != -1 && nxt_port_ready_test_fd_is_open(fd1)) {
        (void) close(fd1);
    }

    return NXT_ERROR;
}


#if (NXT_USE_CMSG_PID)

/*
 * A PROCESS_READY whose kernel-validated sender pid does not match the
 * isolated_pid of the process it names has to be refused outright: the
 * descriptors it carries are closed, the process keeps the state it had,
 * and the port keeps the queue it had.
 *
 * The message carries a mappable queue descriptor wherever one can be
 * made, so that a handler without the gate is caught installing it rather
 * than merely caught failing to map it -- which is what a /dev/null
 * descriptor would produce, and would pass these checks on a queue that is
 * already installed.
 */
static nxt_int_t
nxt_port_ready_test_reject(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_recv_msg_t *msg, nxt_process_t *process, nxt_port_t *port,
    nxt_pid_t cmsg_pid, const char *name)
{
    void                 *prev_queue;
    nxt_fd_t             fd0, fd1, prev_fd;
    nxt_pid_t            saved_cmsg_pid;
    nxt_process_state_t  prev_state;

#if (NXT_HAVE_MEMFD_CREATE)
    fd0 = syscall(SYS_memfd_create, "nxt_port_ready_test", MFD_CLOEXEC);

    if (fd0 != -1 && ftruncate(fd0, sizeof(nxt_port_queue_t)) == -1) {
        (void) close(fd0);
        fd0 = -1;
    }
#else
    fd0 = open("/dev/null", O_RDONLY);
#endif

    fd1 = open("/dev/null", O_RDONLY);

    if (fd0 == -1 || fd1 == -1) {
        nxt_log_alert(thr->log, "port ready test: %s failed to open the "
                      "descriptors", name);
        goto fail;
    }

    prev_state = process->state;
    prev_queue = port->queue;
    prev_fd = port->queue_fd;

    saved_cmsg_pid = msg->cmsg_pid;
    msg->cmsg_pid = cmsg_pid;

    msg->fd[0] = fd0;
    msg->fd[1] = fd1;

    nxt_port_process_ready_handler(task, msg);

    msg->cmsg_pid = saved_cmsg_pid;

    /*
     * The state and the queue are checked before the descriptors: a
     * handler that accepted the message owns fd0 through the port, and
     * the cleanup below would close it a second time.
     */
    if (process->state != prev_state) {
        nxt_log_alert(thr->log, "port ready test: %s changed the process "
                      "state", name);
        return NXT_ERROR;
    }

    if (port->queue != prev_queue || port->queue_fd != prev_fd) {
        nxt_log_alert(thr->log, "port ready test: %s replaced the queue",
                      name);
        return NXT_ERROR;
    }

    if (msg->fd[0] != -1 || msg->fd[1] != -1) {
        nxt_log_alert(thr->log, "port ready test: %s left fds in the message",
                      name);
        return NXT_ERROR;
    }

    if (nxt_port_ready_test_fd_is_open(fd0)
        || nxt_port_ready_test_fd_is_open(fd1))
    {
        nxt_log_alert(thr->log, "port ready test: %s leaked a descriptor",
                      name);
        goto fail;   /* closes only what is still open */
    }

    return NXT_OK;

fail:

    if (fd0 != -1 && nxt_port_ready_test_fd_is_open(fd0)) {
        (void) close(fd0);
    }

    if (fd1 != -1 && nxt_port_ready_test_fd_is_open(fd1)) {
        (void) close(fd1);
    }

    return NXT_ERROR;
}

#endif


#if (NXT_HAVE_MEMFD_CREATE)

/*
 * The size check in nxt_port_queue_mmap() (src/nxt_port.c) refuses only an
 * object shorter than the queue, and both edges of that rule are load
 * bearing.
 *
 * An object of exactly sizeof(nxt_port_queue_t) is what both producers ask
 * for -- nxt_shm_open() in the runtime and nxt_unit_shm_open() in libunit
 * -- so the comparison has to be "<": a "<=" would refuse every queue.
 *
 * A longer object is accepted on purpose.  A shm object is rounded up to a
 * page on some systems and the queue is not a whole number of pages, so
 * tightening the check to equality would refuse every legitimate queue
 * there and stall application port setup.  Only the first
 * sizeof(nxt_port_queue_t) bytes are mapped either way.
 *
 * Both cases touch the last item of the mapping the way
 * nxt_port_queue_send() would.  That probe is not a SIGBUS check: every page
 * of an accepted mapping is at least partially file-backed here, and a
 * partial page at EOF is zero-filled, so no size the helper accepts can
 * fault there.  What it catches is the helper handing back a short or
 * garbage mapping -- and it fixes the shape of the access, so that a future
 * change to what gets mapped is exercised rather than merely returned.  The
 * teeth of these two cases are the NULL checks above it.
 */
static nxt_int_t
nxt_port_ready_test_queue_size(nxt_thread_t *thr, nxt_task_t *task,
    size_t file_size, const char *name)
{
    void                       *mem;
    nxt_fd_t                   fd;
    volatile nxt_port_queue_t  *queue;

    fd = syscall(SYS_memfd_create, "nxt_port_ready_test", MFD_CLOEXEC);
    if (fd == -1) {
        nxt_log_alert(thr->log, "port ready test failed to create the queue");
        return NXT_ERROR;
    }

    if (ftruncate(fd, file_size) == -1) {
        nxt_log_alert(thr->log, "port ready test failed to size the queue");
        (void) close(fd);
        return NXT_ERROR;
    }

    mem = nxt_port_queue_mmap(task, fd, sizeof(nxt_port_queue_t));

    /* The mapping does not keep the descriptor. */
    (void) close(fd);

    if (mem == NULL) {
        nxt_log_alert(thr->log, "port ready test: %s queue was refused", name);
        return NXT_ERROR;
    }

    queue = mem;
    queue->items[NXT_PORT_QUEUE_SIZE - 1].size = 0;

    nxt_mem_munmap(mem, sizeof(nxt_port_queue_t));

    return NXT_OK;
}


static nxt_int_t
nxt_port_ready_test_queue_sizes(nxt_thread_t *thr, nxt_task_t *task)
{
    size_t  rounded;

    if (nxt_slow_path(nxt_port_ready_test_queue_size(thr, task,
                                                     sizeof(nxt_port_queue_t),
                                                     "an exact-size")
                      != NXT_OK))
    {
        return NXT_ERROR;
    }

    rounded = nxt_align_size(sizeof(nxt_port_queue_t), nxt_pagesize);

    return nxt_port_ready_test_queue_size(thr, task, rounded,
                                          "a page-rounded");
}


/*
 * Hand the handler a mappable descriptor and check that the port takes it
 * over: the queue is installed, whatever the port held before is released,
 * and the second descriptor of the message is closed.
 */
static nxt_int_t
nxt_port_ready_test_queue(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_recv_msg_t *msg, nxt_port_t *port, const char *name)
{
    void      *prev_queue;
    nxt_fd_t  fd, spare, prev_fd;

    /*
     * The syscall form, as nxt_unit_shm_open() and nxt_unit_port_recv_test.c
     * use: auto/shmem probes SYS_memfd_create, so a libc without the wrapper
     * still defines NXT_HAVE_MEMFD_CREATE and this file has to link.
     */
    fd = syscall(SYS_memfd_create, "nxt_port_ready_test", MFD_CLOEXEC);
    spare = open("/dev/null", O_RDONLY);

    if (fd == -1 || spare == -1) {
        nxt_log_alert(thr->log, "port ready test failed to create the queue");
        goto fail;
    }

    if (ftruncate(fd, sizeof(nxt_port_queue_t)) == -1) {
        nxt_log_alert(thr->log, "port ready test failed to size the queue");
        goto fail;
    }

    prev_fd = port->queue_fd;
    prev_queue = port->queue;

    msg->fd[0] = fd;
    msg->fd[1] = spare;

    nxt_port_process_ready_handler(task, msg);

    if (port->queue_fd != fd || port->queue == NULL
        || port->queue == MAP_FAILED)
    {
        nxt_log_alert(thr->log, "port ready test: %s did not install the "
                      "queue", name);
        return NXT_ERROR;
    }

    if (msg->fd[0] != -1 || msg->fd[1] != -1) {
        nxt_log_alert(thr->log, "port ready test: %s left fds in the message",
                      name);
        return NXT_ERROR;
    }

    if (nxt_port_ready_test_fd_is_open(spare)) {
        nxt_log_alert(thr->log, "port ready test: %s leaked the spare "
                      "descriptor", name);
        return NXT_ERROR;
    }

    if (prev_queue != NULL && nxt_port_ready_test_fd_is_open(prev_fd)) {
        nxt_log_alert(thr->log, "port ready test: %s did not release the "
                      "previous queue", name);
        return NXT_ERROR;
    }

    return NXT_OK;

fail:

    if (fd != -1) {
        (void) close(fd);
    }

    if (spare != -1) {
        (void) close(spare);
    }

    return NXT_ERROR;
}


/*
 * Hand the handler a descriptor too short for the queue.  mmap() succeeds
 * on it, so nothing fails until the first access past the object's last
 * page, which raises SIGBUS in the process that mapped it -- main or the
 * prototype.  The handler has to refuse the descriptor and keep the queue
 * it already had.
 */
static nxt_int_t
nxt_port_ready_test_short_queue(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_recv_msg_t *msg, nxt_port_t *port)
{
    void                       *prev_queue;
    nxt_fd_t                   fd, prev_fd;
    volatile nxt_port_queue_t  *queue;

    fd = syscall(SYS_memfd_create, "nxt_port_ready_test", MFD_CLOEXEC);
    if (fd == -1) {
        nxt_log_alert(thr->log, "port ready test failed to create the queue");
        return NXT_ERROR;
    }

    /*
     * One page: mmap() of the whole queue over it succeeds, and every byte
     * beyond it faults.
     */
    if (ftruncate(fd, nxt_pagesize) == -1) {
        nxt_log_alert(thr->log, "port ready test failed to size the queue");
        (void) close(fd);
        return NXT_ERROR;
    }

    prev_fd = port->queue_fd;
    prev_queue = port->queue;

    msg->fd[0] = fd;
    msg->fd[1] = -1;

    nxt_port_test_broadcasts = 0;

    nxt_port_process_ready_handler(task, msg);

    /*
     * The positive half of the broadcast assertion in the queueless case:
     * refusing a replacement is not a failed start, so this message still
     * reaches the announcement.  Without this, "not broadcast" there would
     * pass on a handler that never broadcasts at all.
     */
    if (nxt_port_test_broadcasts == 0) {
        nxt_log_alert(thr->log, "port ready test: a kept queue was not "
                      "broadcast");
        return NXT_ERROR;
    }

    if (port->queue != prev_queue || port->queue_fd != prev_fd) {
        nxt_log_alert(thr->log, "port ready test: short queue replaced the "
                      "installed one");
        return NXT_ERROR;
    }

    if (msg->fd[0] != -1 || nxt_port_ready_test_fd_is_open(fd)) {
        nxt_log_alert(thr->log, "port ready test: short queue leaked its "
                      "descriptor");
        return NXT_ERROR;
    }

    /*
     * Touch the last item of the queue the port holds, the way
     * nxt_port_queue_send() would, to prove the mapping the port kept is
     * backed all the way to its end.  The check above already catches a
     * handler that installs the short mapping, and catches it before this
     * access, which on such a handler takes SIGBUS and kills the test
     * binary -- verified by moving this access ahead of the check.
     */
    queue = port->queue;
    queue->items[NXT_PORT_QUEUE_SIZE - 1].size = 0;

    return NXT_OK;
}

/*
 * Issue #231: a first PROCESS_READY whose queue cannot be mapped.
 *
 * This is a different arm from nxt_port_ready_test_short_queue() above,
 * and the distinction is the whole point.  That case runs after a queue
 * has been installed, so the handler keeps the working mapping and the
 * process stays ready -- refusing a replacement is not a failed start.
 * Here the port has no queue at all, so there is nothing to fall back to:
 * the worker is unreachable on this port forever, and the start has to
 * fail rather than be marked ready.
 *
 * The kill target is a real forked child, never nxt_pid + N.  The suite
 * runs as root in CI, so a made-up pid is a live process on the machine;
 * only the pid fork() returned is known to be ours to signal.
 */
static nxt_int_t
nxt_port_ready_test_queueless(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_recv_msg_t *msg, nxt_process_t *process, nxt_port_t *port)
{
    int        status;
    pid_t      child, reaped;
    nxt_fd_t   fd;
    nxt_uint_t i;

    if (nxt_slow_path(port->queue != NULL)) {
        nxt_log_alert(thr->log, "port ready test: the queueless case needs a "
                      "port with no queue");
        return NXT_ERROR;
    }

    /*
     * A child that only waits to be killed.  _exit() rather than return so
     * that a child which somehow escapes pause() cannot run the rest of the
     * test suite as a second process.
     */
    child = fork();
    if (child == -1) {
        nxt_log_alert(thr->log, "port ready test failed to fork the kill "
                      "target");
        return NXT_ERROR;
    }

    if (child == 0) {
        for ( ;; ) {
            pause();
        }

        _exit(0);
    }

    process->isolated_pid = child;

#if (NXT_USE_CMSG_PID)
    /* The sender gate compares against isolated_pid, which is now the child. */
    msg->cmsg_pid = child;
#endif

    fd = syscall(SYS_memfd_create, "nxt_port_ready_test", MFD_CLOEXEC);
    if (fd == -1) {
        nxt_log_alert(thr->log, "port ready test failed to create the queue");
        goto fail;
    }

    /* One page: every byte past it faults, so the mapping must be refused. */
    if (ftruncate(fd, nxt_pagesize) == -1) {
        nxt_log_alert(thr->log, "port ready test failed to size the queue");
        (void) close(fd);
        goto fail;
    }

    msg->fd[0] = fd;
    msg->fd[1] = -1;

    nxt_port_test_broadcasts = 0;

    nxt_port_process_ready_handler(task, msg);

    if (msg->fd[0] != -1 || nxt_port_ready_test_fd_is_open(fd)) {
        nxt_log_alert(thr->log, "port ready test: the refused queue leaked "
                      "its descriptor");
        goto fail;
    }

    if (port->queue != NULL || port->queue_fd != -1) {
        nxt_log_alert(thr->log, "port ready test: a refused queue was "
                      "installed anyway");
        goto fail;
    }

#if (NXT_USE_CMSG_PID)

    /*
     * Never announce a port nothing can be delivered on: every peer would
     * hold the same unreachable copy, and a QUIT could never arrive.  The
     * kept-queue case above is what stops this passing vacuously.
     */
    if (nxt_port_test_broadcasts != 0) {
        nxt_log_alert(thr->log, "port ready test: a refused queue was "
                      "broadcast anyway");
        goto fail;
    }

    /*
     * Not ready: nothing about this worker ever became usable, and the
     * prototype's SIGCHLD notifies the others for any state but CREATING,
     * so leaving it at CREATED still reaches the start's failure.
     */
    if (process->state == NXT_PROCESS_STATE_READY) {
        nxt_log_alert(thr->log, "port ready test: a refused queue still "
                      "marked the process ready");
        goto fail;
    }

    if (!process->start_failed) {
        nxt_log_alert(thr->log, "port ready test: a refused queue did not "
                      "mark the start as failed");
        goto fail;
    }

    /*
     * The fixture enters at CREATING, as a worker whose WHOAMI has not been
     * handled yet would.  Killing it while it is still CREATING would leave
     * the prototype's SIGCHLD silent (nxt_application.c:907) and the start
     * pending -- the wedge, reached by a different road.
     */
    if (process->state != NXT_PROCESS_STATE_CREATED) {
        nxt_log_alert(thr->log, "port ready test: a refused queue left the "
                      "process at CREATING, so nothing will notify its death");
        goto fail;
    }

    /*
     * Polled rather than blocking: a handler that never signalled would
     * otherwise hang the suite instead of failing it.
     */
    for (i = 0; i < 100; i++) {
        reaped = waitpid(child, &status, WNOHANG);

        if (reaped == child) {
            break;
        }

        if (reaped == -1) {
            nxt_log_alert(thr->log, "port ready test: waitpid() failed %E",
                          nxt_errno);
            goto fail;
        }

        nxt_nanosleep(10 * 1000000);   /* 10ms, so at most a second. */
    }

    if (reaped != child) {
        nxt_log_alert(thr->log, "port ready test: the process whose queue "
                      "was refused was not killed");
        goto fail;
    }

    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        nxt_log_alert(thr->log, "port ready test: the killed process did not "
                      "die of SIGKILL (status %d)", status);
        return NXT_ERROR;
    }

    /*
     * The latch, tested for what it is actually for.  A retransmitted
     * PROCESS_READY must not signal a second time: the first pid has been
     * reaped and may by now name an unrelated process.  Pointing
     * isolated_pid at a second, live child makes that concrete -- without
     * the latch the handler re-enters the failure arm and kills it, so the
     * child's survival is the assertion.  Re-checking the first pid would
     * prove nothing, since it is already dead.
     */
    child = fork();
    if (child == -1) {
        nxt_log_alert(thr->log, "port ready test failed to fork the second "
                      "kill target");
        return NXT_ERROR;
    }

    if (child == 0) {
        for ( ;; ) {
            pause();
        }

        _exit(0);
    }

    process->isolated_pid = child;
    msg->cmsg_pid = child;

    fd = syscall(SYS_memfd_create, "nxt_port_ready_test", MFD_CLOEXEC);
    if (fd == -1 || ftruncate(fd, nxt_pagesize) == -1) {
        nxt_log_alert(thr->log, "port ready test failed to re-create the "
                      "queue");
        if (fd != -1) {
            (void) close(fd);
        }
        goto fail;
    }

    msg->fd[0] = fd;
    msg->fd[1] = -1;

    nxt_port_process_ready_handler(task, msg);

    if (msg->fd[0] != -1 || nxt_port_ready_test_fd_is_open(fd)) {
        nxt_log_alert(thr->log, "port ready test: the retransmitted READY "
                      "leaked its descriptor");
        goto fail;
    }

    if (process->state == NXT_PROCESS_STATE_READY) {
        nxt_log_alert(thr->log, "port ready test: the retransmitted READY "
                      "marked the process ready");
        goto fail;
    }

    /*
     * Still running: the latch refused the arm before it reached the kill.
     *
     * SIGKILL is delivered asynchronously, so a single WNOHANG poll here
     * would race a handler that did signal and read as "survived" -- the
     * assertion has to give a kill time to land before concluding none
     * happened.  Verified by removing the latch check: without this window
     * the mutation passes, with it the child is reaped and the test fails.
     */
    for (i = 0; i < 20; i++) {
        reaped = waitpid(child, &status, WNOHANG);

        if (reaped != 0) {
            nxt_log_alert(thr->log, "port ready test: a retransmitted READY "
                          "killed again");
            return NXT_ERROR;
        }

        nxt_nanosleep(10 * 1000000);   /* 10ms, so 200ms in total. */
    }

    (void) kill(child, SIGKILL);
    (void) waitpid(child, &status, 0);

#else

    /*
     * With no kernel-validated sender pid the start cannot be failed
     * safely, so the handler keeps the pre-#231 behaviour and announces the
     * process on a port that cannot be reached.  Asserting that here is what
     * keeps the guard honest: a change that quietly enabled the destructive
     * arm on an unauthenticated platform fails this.
     */
    (void) i;
    (void) reaped;

    if (process->state != NXT_PROCESS_STATE_READY
        || nxt_port_test_broadcasts == 0)
    {
        nxt_log_alert(thr->log, "port ready test: a platform with no sender "
                      "credential did not keep the announcing behaviour");
        goto fail;
    }

    if (process->start_failed) {
        nxt_log_alert(thr->log, "port ready test: a platform with no sender "
                      "credential latched a failed start");
        goto fail;
    }

    /* Nothing signalled it, so the child is ours to clean up. */
    (void) kill(child, SIGKILL);

    if (waitpid(child, &status, 0) != child) {
        nxt_log_alert(thr->log, "port ready test failed to reap the kill "
                      "target");
        return NXT_ERROR;
    }

#endif

    return NXT_OK;

fail:

    (void) kill(child, SIGKILL);
    (void) waitpid(child, &status, 0);

    return NXT_ERROR;
}

#endif


nxt_int_t
nxt_port_ready_test(nxt_thread_t *thr)
{
    nxt_mp_t             *mp;
    nxt_pid_t            isolated_pid;
    nxt_task_t           *task;
    nxt_int_t            ret;
    nxt_port_t           *port, *queueless_port;
    nxt_runtime_t        *rt, *saved_rt;
    nxt_process_t        *process;
#if (NXT_HAVE_MEMFD_CREATE)
    nxt_process_t        *queueless;
#endif
    nxt_port_recv_msg_t  msg;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port ready test started");

    task = thr->task;
    task->thread = thr;

#if (NXT_HAVE_MEMFD_CREATE)

    /*
     * The two accepted sizes, ahead of the handler fixtures and calling
     * nxt_port_queue_mmap() directly: nothing is allocated yet, and a
     * failure here names the rule that broke rather than the handler that
     * relied on it.
     */
    if (nxt_slow_path(nxt_port_ready_test_queue_sizes(thr, task) != NXT_OK)) {
        return NXT_ERROR;
    }

#endif

    /* Defined before the first goto done: the cleanup there releases them. */
    port = NULL;
    queueless_port = NULL;

    isolated_pid = nxt_pid + 3;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    if (nxt_slow_path(rt == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    rt->mem_pool = mp;

    if (nxt_slow_path(nxt_thread_mutex_create(&rt->processes_mutex) != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    saved_rt = thr->runtime;
    thr->runtime = rt;

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    /*
     * A pid the runtime does not know: the lookup fails and the handler
     * has to close what the message carried.
     */
    msg.port_msg.pid = nxt_pid + 1;

    ret = nxt_port_ready_test_case(thr, task, &msg, "unknown pid");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * A process that is ready already: the state check has to reject the
     * message rather than map the descriptor a second time.  The process
     * carries no ports, so reaching the mapping at all would be a bug --
     * the empty-ports guard is the second reject path covered here.
     */
    process = nxt_mp_zalloc(mp, sizeof(nxt_process_t));
    if (nxt_slow_path(process == NULL)) {
        ret = NXT_ERROR;
        goto done;
    }

    /*
     * A pid-isolated process: pid is the global pid the runtime hash is
     * keyed on, isolated_pid is the namespace-local pid the receiver reads
     * out of SCM_CREDENTIALS.  They differ on purpose -- with one value
     * every case would pass the sender gate whichever of the two it
     * compared, and the collision the gate exists to refuse could not be
     * expressed at all.
     */
    process->pid = nxt_pid + 2;
    process->isolated_pid = isolated_pid;
    process->state = NXT_PROCESS_STATE_CREATING;
    nxt_queue_init(&process->ports);

    nxt_runtime_process_add(task, process);

    msg.port_msg.pid = process->pid;
#if (NXT_USE_CMSG_PID)
    msg.cmsg_pid = process->isolated_pid;
#endif

    ret = nxt_port_ready_test_case(thr, task, &msg, "ready with no ports");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * The guard has to reject before the state is set, so that a message
     * the handler refuses leaves the process as it found it.
     */
    if (nxt_slow_path(process->state == NXT_PROCESS_STATE_READY)) {
        nxt_log_alert(thr->log, "port ready test: a rejected message marked "
                      "the process ready");
        ret = NXT_ERROR;
        goto done;
    }

    /*
     * From here on the process has a port, so the empty-ports guard can no
     * longer be what refuses a message, and a refusal has to come from the
     * sender gate itself.
     */
    port = nxt_port_new(task, 1, process->pid, NXT_PROCESS_APP);
    if (nxt_slow_path(port == NULL)) {
        ret = NXT_ERROR;
        goto done;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    nxt_queue_insert_tail(&process->ports, &port->link);

#if (NXT_USE_CMSG_PID)

    /* An unrelated sender announcing somebody else's process. */
    ret = nxt_port_ready_test_reject(thr, task, &msg, process, port,
                                     nxt_pid + 4, "a foreign sender");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * The global pid of the very process being announced.  Inside a pid
     * namespace it is not the pid the receiver sees for that process, and
     * another task's namespace-local pid can be made to equal it, so
     * accepting it as well would leave the gate bypassable.
     */
    ret = nxt_port_ready_test_reject(thr, task, &msg, process, port,
                                     process->pid, "the global pid");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * A record the receiver never forked keeps isolated_pid 0 --
     * nxt_runtime_process_get() fills only ->pid.  There is nothing to
     * authenticate against, so the credential of 0 that a plain equality
     * test would accept has to be refused as well.
     */
    process->isolated_pid = 0;

    ret = nxt_port_ready_test_reject(thr, task, &msg, process, port, 0,
                                     "a process record nothing forked");

    process->isolated_pid = isolated_pid;

    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

#endif

#if (NXT_HAVE_MEMFD_CREATE)

    /*
     * The handler takes the descriptor over and maps it, and a second
     * PROCESS_READY from the same, authenticated sender replaces that
     * mapping instead of leaking it: a process may re-announce a new queue,
     * and refusing the repeat would strand it on the mapping it has.
     */
    ret = nxt_port_ready_test_queue(thr, task, &msg, port, "first ready");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_port_ready_test_queue(thr, task, &msg, port, "replacing ready");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_port_ready_test_short_queue(thr, task, &msg, port);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * Issue #231 needs its own fixture: the process above is ready and its
     * port has a working queue, which is exactly the case that must NOT
     * fail a start.  A fresh record, never announced, is the only way to
     * reach the first-mapping arm.
     */
    queueless = nxt_mp_zalloc(mp, sizeof(nxt_process_t));
    if (nxt_slow_path(queueless == NULL)) {
        ret = NXT_ERROR;
        goto done;
    }

    queueless->pid = nxt_pid + 5;
    queueless->state = NXT_PROCESS_STATE_CREATING;
    nxt_queue_init(&queueless->ports);

    nxt_runtime_process_add(task, queueless);

    queueless_port = nxt_port_new(task, 1, queueless->pid, NXT_PROCESS_APP);
    if (nxt_slow_path(queueless_port == NULL)) {
        ret = NXT_ERROR;
        goto done;
    }

    queueless_port->pair[0] = -1;
    queueless_port->pair[1] = -1;
    queueless_port->socket.fd = -1;

    nxt_queue_insert_tail(&queueless->ports, &queueless_port->link);

    msg.port_msg.pid = queueless->pid;

    ret = nxt_port_ready_test_queueless(thr, task, &msg, queueless,
                                        queueless_port);

    /* Restore the message for anything that runs after this case. */
    msg.port_msg.pid = process->pid;
#if (NXT_USE_CMSG_PID)
    msg.cmsg_pid = process->isolated_pid;
#endif

    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

#if (NXT_USE_CMSG_PID)

    /*
     * The same forgery once a queue is installed and the process is ready:
     * this is the one that matters, because it is where an accepted message
     * would re-point a live port at shared memory of the sender's choosing.
     */
    ret = nxt_port_ready_test_reject(thr, task, &msg, process, port,
                                     nxt_pid + 4, "a foreign sender re-arming "
                                     "an installed queue");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

#endif

#endif

done:

    /*
     * The handler leaves the queue mapping and its descriptor on the port,
     * and nothing else owns them here: releasing the pool would leak both.
     * The fixture port never had a socket pair, so this only frees the queue.
     */
    if (port != NULL) {
        nxt_port_close(task, port);
    }

    if (queueless_port != NULL) {
        nxt_port_close(task, queueless_port);
    }

    thr->runtime = saved_rt;

    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_thread_time_update(thr);
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port ready test passed");
    }

    return ret;
}
