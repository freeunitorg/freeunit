/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for descriptor ownership in
 * nxt_router_new_port_handler() (src/nxt_router.c).
 *
 * An application NEW_PORT carries the worker's socket in fd[0] and the
 * queue it wants the router to map in fd[1].  The router maps the queue
 * and closes fd[1] afterwards -- but the failure branch of that mapping
 * used to return before the close, leaking the descriptor.
 *
 * That branch used to need the router to be out of address space, which is
 * why it went unnoticed.  Since the queue object is validated for size, a
 * peer decides when it is taken: it hands over a short object and the
 * router refuses it.  The dispatcher reclaims nothing once a handler has
 * returned (see nxt_port_recv_msg_close_fds()), so each refusal cost the
 * router a descriptor, which is the exhaustion the validation exists to
 * prevent.
 *
 * The port keeps fd[0]: it is the port's socket, and the port outlives the
 * message.  Only fd[1] is the message's to lose.
 *
 * The refusal itself is checked here too: the port must come back without a
 * queue.  A descriptor accounted for proves nothing about the mapping, and
 * the size check is what keeps a short object off the port -- mmap()
 * succeeds on one, and the router faults on the first access past its last
 * page.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_queue.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/mman.h>

#if (NXT_HAVE_MEMFD_CREATE && NXT_HAVE_LINUX_MEMFD_H)
#include <linux/memfd.h>
#endif
#if (NXT_HAVE_MEMFD_CREATE)
#include <sys/syscall.h>
#endif


#if (NXT_HAVE_MEMFD_CREATE)

static nxt_bool_t
nxt_router_new_port_test_fd_is_open(nxt_fd_t fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

#endif


nxt_int_t
nxt_router_new_port_test(nxt_thread_t *thr)
{
#if !(NXT_HAVE_MEMFD_CREATE)

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router new port test skipped: no memfd_create()");
    return NXT_OK;

#else

    nxt_mp_t                 *mp;
    nxt_fd_t                 sock, queue;
    nxt_buf_t                buf;
    nxt_task_t               *task;
    nxt_int_t                ret;
    nxt_port_t               *port, *reply_port;
    nxt_runtime_t            *rt, *saved_rt;
    nxt_event_engine_t       engine;
    nxt_port_recv_msg_t      msg;
    nxt_port_msg_new_port_t  new_port;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router new port test started");

    ret = NXT_ERROR;
    sock = -1;
    queue = -1;
    port = NULL;
    reply_port = NULL;

    task = thr->task;
    task->thread = thr;

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

    /*
     * The minimal engine nxt_port_write_enable() needs, as
     * nxt_port_fail_test.c injects one: only fast_work_queue is set up, so
     * a path that reaches another member would read garbage.  The handler
     * under test does not get that far.
     */
    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");

    saved_rt = thr->runtime;
    thr->runtime = rt;
    thr->engine = &engine;

    /*
     * Releasing the port's last reference reaches nxt_runtime_process_release(),
     * which frees inline when task->thread->engine is rt->main_engine and
     * otherwise posts the free to that engine -- NULL here, and a posted work
     * item would outlive this stack frame anyway.  Pointing it at the fixture's
     * own engine keeps the teardown synchronous.
     */
    rt->main_engine = &engine;

    /*
     * The port the message arrived on.  An application NEW_PORT that answers
     * a pending start carries a stream, and the handler dispatches such a
     * message through nxt_port_rpc_handler(), which reads this port; the
     * fixture registers no stream, so the dispatch is a lookup that finds
     * nothing and returns.
     *
     * The stream is what keeps the path under test short.  With stream 0 a
     * handler that took the queue would run on into the main app port lookup
     * -- an assertion the fixture cannot satisfy -- and then into a PORT_ACK
     * written through the queue it just took, faulting on the short mapping.
     * Either would kill the binary instead of failing the checks below.
     */
    reply_port = nxt_runtime_process_port_create(task, rt, nxt_pid, 0,
                                                 NXT_PROCESS_ROUTER);
    if (nxt_slow_path(reply_port == NULL)) {
        goto done;
    }

    /*
     * nxt_port_new() zeroes the port, and 0 is a descriptor teardown must
     * not close.
     */
    reply_port->pair[0] = -1;
    reply_port->pair[1] = -1;
    reply_port->socket.fd = -1;

    /*
     * A short queue: mmap() of the whole object over it would succeed and
     * fault later, so the size check has to refuse it here.
     */
    queue = syscall(SYS_memfd_create, "nxt_router_new_port_test", MFD_CLOEXEC);
    sock = open("/dev/null", O_RDONLY);

    if (queue == -1 || sock == -1) {
        nxt_log_alert(thr->log, "router new port test failed to open the "
                      "descriptors");
        goto done;
    }

    if (ftruncate(queue, nxt_pagesize) == -1) {
        nxt_log_alert(thr->log, "router new port test failed to size the "
                      "queue");
        goto done;
    }

    nxt_memzero(&new_port, sizeof(nxt_port_msg_new_port_t));

    new_port.pid = nxt_pid + 1;
    new_port.id = 1;
    new_port.type = NXT_PROCESS_APP;
    new_port.max_size = 1024;
    new_port.max_share = 1024;

    nxt_memzero(&buf, sizeof(nxt_buf_t));

    buf.mem.pos = (u_char *) &new_port;
    buf.mem.free = buf.mem.pos + sizeof(nxt_port_msg_new_port_t);

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.buf = &buf;
    msg.fd[0] = sock;
    msg.fd[1] = queue;

    msg.port = reply_port;
    msg.port_msg.stream = 1;
    msg.port_msg.pid = new_port.pid;

    nxt_router_new_port_handler(task, &msg);

    if (msg.fd[1] != -1 || nxt_router_new_port_test_fd_is_open(queue)) {
        nxt_log_alert(thr->log, "router new port test: a refused queue "
                      "leaked its descriptor");
        goto done;
    }

    queue = -1;

    if (msg.fd[0] != -1) {
        nxt_log_alert(thr->log, "router new port test: the port did not take "
                      "the socket");
        goto done;
    }

    /*
     * The port owns the socket now, so the test must not close it directly
     * -- but the port itself is the fixture's to release: it is registered
     * in rt->ports and holds both the descriptor and its own pool, and
     * destroying only the runtime pool below would leak them for the rest
     * of the test binary.
     */
    sock = -1;
    port = msg.u.new_port;

    /*
     * The point of the refusal: the short object must not be on the port.
     * The descriptor checks above say only that nothing leaked, and would
     * hold just as well for a handler that mapped the object and kept it.
     */
    if (port->queue != NULL) {
        nxt_log_alert(thr->log, "router new port test: a refused queue was "
                      "installed on the port");
        goto done;
    }

    ret = NXT_OK;

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router new port test passed");

done:

    if (queue != -1) {
        (void) close(queue);
    }

    if (sock != -1) {
        (void) close(sock);
    }

    if (port != NULL) {
        /*
         * Through the ownership path, not by destroying the pool directly:
         * nxt_port_mp_cleanup() asserts that the descriptors are gone and
         * the use count is zero, so a direct nxt_mp_destroy() aborts a debug
         * build even though it looks clean in a release one.  close() clears
         * pair[1]; remove() drops the rt->ports reference, which is the last
         * one, and the port is freed there.
         */
        nxt_port_close(task, port);
        nxt_runtime_port_remove(task, port);
    }

    if (reply_port != NULL) {
        nxt_port_close(task, reply_port);
        nxt_runtime_port_remove(task, reply_port);
    }

    thr->runtime = saved_rt;
    thr->engine = NULL;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    return ret;

#endif
}
