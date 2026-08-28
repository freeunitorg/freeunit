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
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_queue.h>
#include <nxt_runtime.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/mman.h>

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

    nxt_port_process_ready_handler(task, msg);

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

#endif


nxt_int_t
nxt_port_ready_test(nxt_thread_t *thr)
{
    nxt_mp_t             *mp;
    nxt_task_t           *task;
    nxt_int_t            ret;
    nxt_port_t           *port;
    nxt_runtime_t        *rt, *saved_rt;
    nxt_process_t        *process;
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

    /* Defined before the first goto done: the cleanup there releases it. */
    port = NULL;

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

    process->pid = nxt_pid + 2;
    process->state = NXT_PROCESS_STATE_CREATING;
    nxt_queue_init(&process->ports);

    nxt_runtime_process_add(task, process);

    msg.port_msg.pid = process->pid;

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

#if (NXT_HAVE_MEMFD_CREATE)

    /*
     * With a port on the process the handler takes the descriptor over and
     * maps it, and a second PROCESS_READY replaces that mapping instead of
     * leaking it -- a repeated message is not refused, so that a forged one
     * arriving first cannot lock the legitimate sender out.
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

    ret = nxt_port_ready_test_queue(thr, task, &msg, port, "first ready");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_port_ready_test_queue(thr, task, &msg, port, "replacing ready");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_port_ready_test_short_queue(thr, task, &msg, port);

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

    thr->runtime = saved_rt;

    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_thread_time_update(thr);
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port ready test passed");
    }

    return ret;
}
