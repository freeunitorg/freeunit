/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for issue #257: nxt_main_start_process_handler()
 * (src/nxt_main_process.c) had exits above the fork() that returned
 * without answering the router.
 *
 * START_PROCESS carries the stream of an RPC the router has already armed
 * in nxt_router_start_app_process_handler(), having incremented
 * app->proto_port_requests for it.  Only a reply retires that RPC: with
 * none, neither nxt_router_app_port_ready() nor nxt_router_app_port_error()
 * ever runs, the counter is never cleared, and every later start for the
 * application parks on a prototype that is not coming.  The three exits
 * covered here -- no router port, a sender that is not the router, and a
 * process record that cannot be allocated -- all used to "goto close_fds",
 * which closes the descriptors and returns in silence.
 *
 * The reply is observed rather than inferred: the fixture ports carry no
 * queue and leave write_ready unset, so nxt_port_socket_write() takes the
 * nxt_port_msg_chk_insert() path and the message stays on port->messages
 * for the test to read -- the arrangement src/test/nxt_router_start_fail_
 * test.c and src/test/nxt_router_proto_wedge_test.c already rely on.  Each
 * case requires exactly one message, of type RPC_ERROR and on the stream
 * the incoming message named: a handler that answered twice would retire
 * an RPC that is no longer the caller's.
 *
 * The foreign-sender case additionally pins down where the answer goes.
 * The reply port is looked up by the kernel-validated sender pid, not by
 * the self-declared msg->port_msg.pid, so a forged START_PROCESS can only
 * ever cancel a stream of the forger's own; that case gives the two pids
 * different values and requires the router's port to stay untouched.
 *
 * The prototype's own START_PROCESS handler is covered here too.  It is the
 * other end of the same message -- the router sends one to main to get a
 * prototype and one to the prototype to get a worker
 * (src/nxt_router.c:469 and :3347) -- and until issue #270 it checked
 * nothing at all, although every worker it forks inherits the write end of
 * the prototype's port.  Its cases are refusals only, and each is run in a
 * forked child: a handler that does not refuse goes on to fork a worker
 * from a fixture that has no application loaded, and containing that keeps
 * one broken gate from taking the whole test binary down.  The parent
 * reports a child that died as the fall-through it is.
 *
 * Not covered: the nxt_mp_create() failure a few lines below the process
 * allocation.  Arming it needs a malloc-failure hook that does not exist,
 * and the sites are not independent -- both reach "failed:" through the
 * same "goto", so the process-allocation case already exercises the reply
 * they share.  What it does not exercise is the "process != NULL" guard at
 * "failed:", which only the mem_pool site can reach.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_port.h>
#include <nxt_port_hash.h>
#include <nxt_main_process.h>
#include <nxt_application.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/wait.h>


static nxt_bool_t
nxt_main_start_process_reply_test_fd_is_open(nxt_fd_t fd)
{
    return fcntl(fd, F_GETFD) != -1;
}


/*
 * Run the handler once and require that it answered: exactly one RPC_ERROR
 * on reply_port, carrying the stream of the incoming message, and nothing
 * at all on silent_port (which may be NULL when there is no second port to
 * rule out).  Both ports are left drained, so the next case starts from
 * empty queues.
 */
static nxt_int_t
nxt_main_start_process_reply_test_case(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_recv_msg_t *msg, nxt_port_t *reply_port, nxt_port_t *silent_port,
    const char *name)
{
    nxt_fd_t             fd0, fd1;
    nxt_int_t            ret;
    nxt_uint_t           n;
    nxt_queue_link_t     *link;
    nxt_port_send_msg_t  *sent;

    ret = NXT_ERROR;
    sent = NULL;
    n = 0;

    fd0 = open("/dev/null", O_RDONLY);
    fd1 = open("/dev/null", O_RDONLY);

    if (nxt_slow_path(fd0 == -1 || fd1 == -1)) {
        nxt_log_alert(thr->log, "main start process reply test: %s failed to "
                      "open the descriptors", name);
        goto done;
    }

    msg->fd[0] = fd0;
    msg->fd[1] = fd1;

    nxt_main_test_run_start_process_handler(task, msg);

    /*
     * Both exits own the descriptors the message carried, and both spellings
     * of the failure tail closed them -- so this is not what discriminates
     * the fix, only a guard against the cleanup below closing a number that
     * has already been reused.
     */
    if (msg->fd[0] == -1 && !nxt_main_start_process_reply_test_fd_is_open(fd0))
    {
        fd0 = -1;
    }

    if (msg->fd[1] == -1 && !nxt_main_start_process_reply_test_fd_is_open(fd1))
    {
        fd1 = -1;
    }

    if (nxt_slow_path(fd0 != -1 || fd1 != -1)) {
        nxt_log_alert(thr->log, "main start process reply test: %s leaked a "
                      "descriptor", name);
        goto done;
    }

    for (link = nxt_queue_first(&reply_port->messages);
         link != nxt_queue_tail(&reply_port->messages);
         link = nxt_queue_next(link))
    {
        n++;
        sent = nxt_queue_link_data(link, nxt_port_send_msg_t, link);
    }

    if (nxt_slow_path(n != 1)) {
        nxt_log_alert(thr->log, "main start process reply test: %s left %ui "
                      "replies on the reply port (expected 1) -- the "
                      "START_PROCESS RPC is stranded", name, n);
        goto done;
    }

    if (nxt_slow_path(sent->port_msg.type != _NXT_PORT_MSG_RPC_ERROR)) {
        nxt_log_alert(thr->log, "main start process reply test: %s answered "
                      "with message type %d (expected %d, RPC_ERROR)", name,
                      (int) sent->port_msg.type,
                      (int) _NXT_PORT_MSG_RPC_ERROR);
        goto done;
    }

    if (nxt_slow_path(sent->port_msg.last != 1)) {
        nxt_log_alert(thr->log, "main start process reply test: %s answered "
                      "with a non-final message, so the RPC stays armed",
                      name);
        goto done;
    }

    if (nxt_slow_path(sent->port_msg.stream != msg->port_msg.stream)) {
        nxt_log_alert(thr->log, "main start process reply test: %s answered "
                      "on stream %uD (expected %uD)", name,
                      sent->port_msg.stream, msg->port_msg.stream);
        goto done;
    }

    if (silent_port != NULL && !nxt_queue_is_empty(&silent_port->messages)) {
        nxt_log_alert(thr->log, "main start process reply test: %s answered "
                      "on a port of the process it was not sent by", name);
        goto done;
    }

    ret = NXT_OK;

done:

    /*
     * Close only what is still open: a consumed descriptor is gone already,
     * and closing its number again could reach an unrelated file.
     */
    if (fd0 != -1 && nxt_main_start_process_reply_test_fd_is_open(fd0)) {
        (void) close(fd0);
    }

    if (fd1 != -1 && nxt_main_start_process_reply_test_fd_is_open(fd1)) {
        (void) close(fd1);
    }

    /*
     * A queued message is malloc'd and takes a port reference
     * (nxt_port_msg_chk_insert()).  Left alone the port never reaches
     * nxt_port_mp_cleanup(), so its pool leaks and the assertions there
     * never run -- the same teardown src/test/nxt_router_proto_wedge_test.c
     * performs, and for the same reason.
     */
    nxt_port_test_run_error_handler(task, reply_port);

    if (silent_port != NULL) {
        nxt_port_test_run_error_handler(task, silent_port);
    }

    return ret;
}


/*
 * Run the handler once where there is no port to answer on: the sender is a
 * pid the runtime has never seen, so nxt_runtime_port_find() returns NULL
 * and the handler can only log.  Nothing may be queued on any port, and the
 * descriptors the message carried must still be released.
 */
static nxt_int_t
nxt_main_start_process_reply_test_no_reply_case(nxt_thread_t *thr,
    nxt_task_t *task, nxt_port_recv_msg_t *msg, nxt_port_t *port_a,
    nxt_port_t *port_b, const char *name)
{
    nxt_fd_t   fd0, fd1;
    nxt_int_t  ret;

    ret = NXT_ERROR;

    fd0 = open("/dev/null", O_RDONLY);
    fd1 = open("/dev/null", O_RDONLY);

    if (nxt_slow_path(fd0 == -1 || fd1 == -1)) {
        nxt_log_alert(thr->log, "main start process reply test: %s failed to "
                      "open the descriptors", name);
        goto done;
    }

    msg->fd[0] = fd0;
    msg->fd[1] = fd1;

    nxt_main_test_run_start_process_handler(task, msg);

    if (msg->fd[0] == -1 && !nxt_main_start_process_reply_test_fd_is_open(fd0))
    {
        fd0 = -1;
    }

    if (msg->fd[1] == -1 && !nxt_main_start_process_reply_test_fd_is_open(fd1))
    {
        fd1 = -1;
    }

    if (nxt_slow_path(fd0 != -1 || fd1 != -1)) {
        nxt_log_alert(thr->log, "main start process reply test: %s leaked a "
                      "descriptor", name);
        goto done;
    }

    if (nxt_slow_path(!nxt_queue_is_empty(&port_a->messages)
                      || (port_b != NULL
                          && !nxt_queue_is_empty(&port_b->messages))))
    {
        nxt_log_alert(thr->log, "main start process reply test: %s answered "
                      "on a port, but no port belongs to the sender", name);
        goto done;
    }

    ret = NXT_OK;

done:

    if (fd0 != -1 && nxt_main_start_process_reply_test_fd_is_open(fd0)) {
        (void) close(fd0);
    }

    if (fd1 != -1 && nxt_main_start_process_reply_test_fd_is_open(fd1)) {
        (void) close(fd1);
    }

    nxt_port_test_run_error_handler(task, port_a);

    if (port_b != NULL) {
        nxt_port_test_run_error_handler(task, port_b);
    }

    return ret;
}

#if (NXT_USE_CMSG_PID)

/*
 * One refusal case for nxt_proto_start_process_handler(), run inside a
 * forked child.
 *
 * The child does the whole check and reports through its exit status: 0 when
 * the handler refused, 1 when it was reached and left a trace it should not
 * have.  A handler with no gate does not return at all -- it allocates a
 * process and reads nxt_app_conf, which no fixture here sets -- so the
 * parent treats a signalled child as the failure it is, and names it.
 *
 * What "refused" means, in the order the child tests it: nothing was queued
 * on any port (a refusal must not answer, because the reply is addressed by
 * the sender-chosen msg->port_msg.pid and answering would let a forger
 * cancel a stream of its choosing), and both descriptors the message carried
 * were closed (this handler owns them; the port read loop reclaims nothing).
 */
static nxt_int_t
nxt_main_start_process_reply_test_proto_case(nxt_thread_t *thr,
    nxt_task_t *task, nxt_port_recv_msg_t *msg, nxt_port_t *port_a,
    nxt_port_t *port_b, const char *name)
{
    int        status;
    pid_t      child;
    nxt_fd_t   fd0, fd1;
    nxt_int_t  ret;

    fd0 = open("/dev/null", O_RDONLY);
    fd1 = open("/dev/null", O_RDONLY);

    if (nxt_slow_path(fd0 == -1 || fd1 == -1)) {
        nxt_log_alert(thr->log, "main start process reply test: %s failed to "
                      "open the descriptors", name);

        if (fd0 != -1) {
            (void) close(fd0);
        }

        if (fd1 != -1) {
            (void) close(fd1);
        }

        return NXT_ERROR;
    }

    msg->fd[0] = fd0;
    msg->fd[1] = fd1;

    child = fork();

    if (nxt_slow_path(child == -1)) {
        nxt_log_alert(thr->log, "main start process reply test: %s failed to "
                      "fork %E", name, nxt_errno);
        (void) close(fd0);
        (void) close(fd1);

        return NXT_ERROR;
    }

    if (child == 0) {
        ret = NXT_OK;

        nxt_proto_test_run_start_process_handler(task, msg);

        if (!nxt_queue_is_empty(&port_a->messages)
            || (port_b != NULL && !nxt_queue_is_empty(&port_b->messages)))
        {
            nxt_log_alert(thr->log, "main start process reply test: %s was "
                          "answered; a refused START_PROCESS must not reply, "
                          "because the reply is addressed by the pid the "
                          "sender chose", name);
            ret = NXT_ERROR;
        }

        if (msg->fd[0] != -1 || msg->fd[1] != -1
            || nxt_main_start_process_reply_test_fd_is_open(fd0)
            || nxt_main_start_process_reply_test_fd_is_open(fd1))
        {
            nxt_log_alert(thr->log, "main start process reply test: %s leaked "
                          "a descriptor", name);
            ret = NXT_ERROR;
        }

        /*
         * _exit(), not exit(): the child shares the parent's stdio and its
         * pools, and must run no destructor or atexit handler of theirs.
         */
        _exit(ret == NXT_OK ? 0 : 1);
    }

    /* The child owns them now; this frame only has to stop naming them. */
    (void) close(fd0);
    (void) close(fd1);

    msg->fd[0] = -1;
    msg->fd[1] = -1;

    while (waitpid(child, &status, 0) == -1) {
        if (nxt_errno != NXT_EINTR) {
            nxt_log_alert(thr->log, "main start process reply test: %s failed "
                          "to reap the child %E", name, nxt_errno);
            return NXT_ERROR;
        }
    }

    if (nxt_slow_path(!WIFEXITED(status))) {
        nxt_log_alert(thr->log, "main start process reply test: %s did not "
                      "refuse -- the handler ran past the sender check and "
                      "into the start path, and died there on signal %d",
                      name, WTERMSIG(status));
        return NXT_ERROR;
    }

    if (nxt_slow_path(WEXITSTATUS(status) != 0)) {
        return NXT_ERROR;
    }

    return NXT_OK;
}

#endif


nxt_int_t
nxt_main_start_process_reply_test(nxt_thread_t *thr)
{
    nxt_mp_t             *mp;
    nxt_int_t            ret;
    nxt_task_t           *task;
    nxt_port_t           *router_port, *foreign_port;
    nxt_runtime_t        *rt, *saved_rt;
    nxt_event_engine_t   engine, *saved_engine;
    nxt_port_recv_msg_t  msg;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "main start process reply test started");

    ret = NXT_ERROR;
    router_port = NULL;
    foreign_port = NULL;

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
     * Only the members the reached code touches are set; the engine is what
     * the queued-message teardown needs (see src/test/nxt_router_new_port_
     * test.c for the same fixture).
     */
    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");
    engine.mem_pool = mp;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;
    thr->runtime = rt;
    thr->engine = &engine;
    rt->main_engine = &engine;

    /*
     * The router's port: both the one the handler checks the sender
     * against, and -- since the router is the sender in every case but the
     * forgery below -- the one the reply is written to.  No queue and
     * write_ready unset, so a reply is queued rather than written.
     */

    router_port = nxt_port_new(task, 0, nxt_pid, NXT_PROCESS_ROUTER);
    if (nxt_slow_path(router_port == NULL)) {
        goto done;
    }

    router_port->pair[0] = -1;
    router_port->pair[1] = -1;
    router_port->socket.fd = -1;

    /*
     * What nxt_runtime_port_add() does, spelled out: that helper is static
     * to src/nxt_runtime.c, and it also takes a port reference the fixture
     * would then have to give back.  The port stays owned by this frame.
     */
    rt->port_by_type[NXT_PROCESS_ROUTER] = router_port;

    if (nxt_slow_path(nxt_port_hash_add(&rt->ports, router_port) != NXT_OK)) {
        goto done;
    }

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.port_msg.pid = nxt_pid;
    msg.port_msg.reply_port = router_port->id;
    msg.port_msg.type = _NXT_PORT_MSG_START_PROCESS;
#if (NXT_USE_CMSG_PID)
    msg.cmsg_pid = nxt_pid;
#endif

    /*
     * No router port at all.  The runtime still knows the sender's port, so
     * there is something to answer on -- the handler used to return without
     * doing so.
     */

    rt->port_by_type[NXT_PROCESS_ROUTER] = NULL;

    msg.port_msg.stream = 0x11111111;

    ret = nxt_main_start_process_reply_test_case(thr, task, &msg, router_port,
                                                 NULL, "a missing router "
                                                 "port");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    rt->port_by_type[NXT_PROCESS_ROUTER] = router_port;

#if (NXT_USE_CMSG_PID)

    /*
     * A sender that is not the router.  The message claims to come from the
     * router -- port_msg.pid is filled in by the sender and is not
     * authenticated -- while the credential the kernel attached names
     * somebody else.  The start is refused, and the refusal has to be
     * reported to the process that actually sent it: answering the pid the
     * message claimed would let a worker cancel a stream of the router's
     * choosing.
     */

    foreign_port = nxt_port_new(task, router_port->id, nxt_pid + 1,
                                NXT_PROCESS_APP);
    if (nxt_slow_path(foreign_port == NULL)) {
        ret = NXT_ERROR;
        goto done;
    }

    foreign_port->pair[0] = -1;
    foreign_port->pair[1] = -1;
    foreign_port->socket.fd = -1;

    if (nxt_slow_path(nxt_port_hash_add(&rt->ports, foreign_port) != NXT_OK)) {
        ret = NXT_ERROR;
        goto done;
    }

    msg.cmsg_pid = nxt_pid + 1;
    msg.port_msg.stream = 0x22222222;

    ret = nxt_main_start_process_reply_test_case(thr, task, &msg,
                                                 foreign_port, router_port,
                                                 "a foreign sender");

    msg.cmsg_pid = nxt_pid;

    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

#endif

    /*
     * The process record cannot be allocated.  This is the exit furthest
     * into the handler that still owes the router an answer, and the one
     * that leaves "process" NULL for the guard at "failed:".
     */

    nxt_main_test_process_new_failures(1);

    msg.port_msg.stream = 0x33333333;

    ret = nxt_main_start_process_reply_test_case(thr, task, &msg, router_port,
                                                 foreign_port,
                                                 "a failed process "
                                                 "allocation");

    nxt_main_test_process_new_failures(0);

    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * Nothing to answer on.  The sender is a pid the runtime knows no port
     * for, so the reply-port lookup fails; the handler logs and must leave
     * every queue untouched rather than answering somebody else.  Both pid
     * fields move together so the case holds however
     * nxt_recv_msg_cmsg_pid() is defined.
     */

    msg.port_msg.pid = nxt_pid + 2;
#if (NXT_USE_CMSG_PID)
    msg.cmsg_pid = nxt_pid + 2;
#endif
    msg.port_msg.stream = 0x44444444;

    ret = nxt_main_start_process_reply_test_no_reply_case(thr, task, &msg,
                                                          router_port,
                                                          foreign_port,
                                                          "an unknown "
                                                          "sender");

    msg.port_msg.pid = nxt_pid;
#if (NXT_USE_CMSG_PID)
    msg.cmsg_pid = nxt_pid;
#endif

    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

#if (NXT_USE_CMSG_PID)

    /*
     * The prototype's START_PROCESS handler, issue #270.  Each case is a
     * refusal; the fixture is the one above, read as the prototype's own
     * runtime -- rt->port_by_type[NXT_PROCESS_ROUTER] is the router port the
     * prototype inherited from main, which nxt_proc_keep_matrix[] keeps.
     */

    /*
     * A worker forging a START_PROCESS.  It claims to be the router in the
     * one field it controls, and the credential names it.  This is the whole
     * exposure: nxt_process_close_ports() keeps the parent's port
     * unconditionally, so every worker holds the write end of the port this
     * message arrived on.
     */

    msg.cmsg_pid = nxt_pid + 1;
    msg.port_msg.stream = 0x55555555;

    ret = nxt_main_start_process_reply_test_proto_case(thr, task, &msg,
                                                      router_port,
                                                      foreign_port,
                                                      "a foreign sender to "
                                                      "the prototype");

    msg.cmsg_pid = nxt_pid;

    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * No router port to compare against.  The prototype cannot tell who sent
     * this, and refusing is the only safe answer -- the same conclusion
     * nxt_main_start_process_handler() reaches, minus the reply it can
     * address and the prototype cannot.
     */

    rt->port_by_type[NXT_PROCESS_ROUTER] = NULL;

    msg.port_msg.stream = 0x66666666;

    ret = nxt_main_start_process_reply_test_proto_case(thr, task, &msg,
                                                      router_port,
                                                      foreign_port,
                                                      "a missing router port "
                                                      "in the prototype");

    rt->port_by_type[NXT_PROCESS_ROUTER] = router_port;

    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * A pid-isolated prototype.  There the router is in an ancestor pid
     * namespace and has no pid at all in this one, so the kernel writes a
     * credential of 0 and the router's global pid is the wrong thing to
     * compare against; the prototype's own workers, forked with no clone
     * flags of their own, are inside the namespace and carry non-zero
     * namespace-local pids.  This case is such a worker -- a credential that
     * is neither 0 nor the router's -- and pins the arm down from the side
     * that matters: a gate that simply skipped the check when isolated would
     * pass every other case here and fail this one.
     */

    rt->is_pid_isolated = 1;

    msg.cmsg_pid = 2;
    msg.port_msg.stream = 0x77777777;

    ret = nxt_main_start_process_reply_test_proto_case(thr, task, &msg,
                                                      router_port,
                                                      foreign_port,
                                                      "a worker inside a "
                                                      "pid-isolated "
                                                      "prototype");

    rt->is_pid_isolated = 0;
    msg.cmsg_pid = nxt_pid;

    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

#endif

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "main start process reply test passed");

    ret = NXT_OK;

done:

    /* Whatever a failed case left armed must not follow the test out. */
    nxt_main_test_process_new_failures(0);

    if (foreign_port != NULL) {
        (void) nxt_port_hash_remove(&rt->ports, foreign_port);

        nxt_port_use(task, foreign_port, -1);
    }

    if (router_port != NULL) {
        (void) nxt_port_hash_remove(&rt->ports, router_port);

        nxt_port_use(task, router_port, -1);
    }

    thr->engine = saved_engine;
    thr->runtime = saved_rt;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    return ret;
}
