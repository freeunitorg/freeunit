/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for descriptor ownership in nxt_port_new_port_handler()
 * and nxt_port_mmap_handler() (src/nxt_port.c).
 *
 * A port message can carry two descriptors whatever its type normally uses
 * -- nxt_socket_msg_oob_get() accepts an SCM_RIGHTS payload of one or two --
 * and the dispatcher does not reclaim them once the handler returns.  Every
 * handler therefore has to release what it does not consume, and clear the
 * slot of what it does.  Both handlers here ignored fd[1] entirely, and
 * nxt_port_mmap_handler() returned from its "invalid fd" guard without
 * closing anything at all, so a peer attaching descriptors to forged
 * messages could exhaust the descriptor table of the router or, through the
 * NEW_PORT path, of the main process.
 *
 * This lives beside nxt_port_ready_test.c rather than inside it: that test
 * builds a fixture for a single handler and walks its reject paths, while
 * these two handlers need a port hash and a message body instead.  The
 * dispatcher (nxt_port_handler()) is covered here too: a type past the
 * table, and an in-range type whose slot the handler table never filled,
 * both have to close what the message carried without calling anything.
 *
 * The one path this cannot reach is nxt_runtime_process_port_create()
 * returning NULL, which needs an allocation failure the test harness has no
 * way to inject.
 *
 * nxt_port_fd_test_tail_close() and nxt_port_fd_test_frag_merge() go one
 * level up, into the static nxt_port_read_msg_process() itself (via the
 * NXT_TESTS wrapper nxt_port_test_run_read_msg_process()), for the two
 * closes nothing above exercises: the tail close that reclaims whatever a
 * dispatch left behind, and the close in the last-fragment merge that runs
 * before msg->fd[0] is overwritten by the assembled message's own
 * descriptors.  nxt_router_response_ready_handler() stays uncovered here
 * too, for the same reason as above.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_queue.h>
#include <nxt_runtime.h>
#include "nxt_tests.h"

#include <fcntl.h>
#include <sys/mman.h>


static nxt_uint_t  nxt_port_fd_test_dispatched;


static void
nxt_port_fd_test_enable_read_stub(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev)
{
    /* The fixture engine polls nothing. */
}


/*
 * A handler that honours the dispatch contract: consume or close what the
 * message carries.  Used to pin that the NULL-slot guard does not swallow
 * types whose slot is set.
 */
static void
nxt_port_fd_test_dispatch_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_port_fd_test_dispatched++;

    nxt_port_recv_msg_close_fds(msg);
}


/*
 * A handler that takes fd[0] -- blanking its slot without closing it, the
 * way a handler that keeps a descriptor is required to -- and releases
 * fd[1] as usual.  Used to pin that nxt_port_read_msg_process() leaves a
 * kept descriptor alone: the tail close and the fragment-merge close both
 * have to treat "-1" as "already spoken for", not "nothing was here".
 */
static void
nxt_port_fd_test_keep_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    msg->fd[0] = -1;

    if (msg->fd[1] != -1) {
        nxt_fd_close(msg->fd[1]);
        msg->fd[1] = -1;
    }
}


/*
 * A handler that ignores the message entirely -- the shape GET_MMAP and
 * OOSM have, and the shape every handler had before the fix.  It never
 * touches msg->fd[], so whatever nxt_port_read_msg_process() does with a
 * dispatch's leftovers is the only thing standing between this and a leak.
 */
static void
nxt_port_fd_test_ignore_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
}


static const nxt_port_handlers_t  nxt_port_fd_test_handlers = {
    .quit = nxt_port_fd_test_dispatch_handler,
    .status = nxt_port_fd_test_keep_handler,
    .oosm = nxt_port_fd_test_ignore_handler,
};


static nxt_bool_t
nxt_port_fd_test_is_open(nxt_fd_t fd)
{
    return fcntl(fd, F_GETFD) != -1;
}


static void
nxt_port_fd_test_close(nxt_fd_t fd)
{
    if (fd != -1 && nxt_port_fd_test_is_open(fd)) {
        (void) close(fd);
    }
}


/*
 * Attach two descriptors to the message, run the handler, and expect it to
 * have released both: neither of these handlers keeps a descriptor of its
 * own beyond the call.
 */
static nxt_int_t
nxt_port_fd_test_closed(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_recv_msg_t *msg,
    void (*handler)(nxt_task_t *task, nxt_port_recv_msg_t *msg),
    nxt_bool_t first_fd, const char *name)
{
    nxt_fd_t  fd0, fd1;

    fd0 = first_fd ? open("/dev/null", O_RDONLY) : -1;
    fd1 = open("/dev/null", O_RDONLY);

    if ((first_fd && fd0 == -1) || fd1 == -1) {
        nxt_log_alert(thr->log, "port fd test failed to open /dev/null");
        goto fail;
    }

    msg->fd[0] = fd0;
    msg->fd[1] = fd1;

    handler(task, msg);

    if ((first_fd && nxt_port_fd_test_is_open(fd0))
        || nxt_port_fd_test_is_open(fd1))
    {
        nxt_log_alert(thr->log, "port fd test: %s leaked a descriptor", name);
        goto fail;   /* closes only what is still open */
    }

    if (msg->fd[0] != -1 || msg->fd[1] != -1) {
        nxt_log_alert(thr->log, "port fd test: %s left fds in the message",
                      name);
        return NXT_ERROR;
    }

    return NXT_OK;

fail:

    /*
     * Close only what the handler left open: a descriptor it consumed is
     * gone already, and closing the number again could reach an unrelated
     * descriptor that has reused it.
     */
    nxt_port_fd_test_close(fd0);
    nxt_port_fd_test_close(fd1);

    return NXT_ERROR;
}


/*
 * Runs a single, non-fragmented message through nxt_port_read_msg_process()
 * and checks what the dispatcher's tail close left behind: this pins the
 * close at the tail of the function, which runs after the handler and
 * covers whatever it did not take -- distinct from nxt_port_fd_test_closed()
 * above, which only exercises the dispatcher's own NULL-slot guard.
 *
 * "keep_fd0" selects the handler dispatched to: nxt_port_fd_test_handlers
 * .oosm ignores the message outright (keep_fd0 false), .status blanks fd[0]
 * without closing it (keep_fd0 true).  Either way fd[1] is never taken, so
 * it is always expected closed.
 */
static nxt_int_t
nxt_port_fd_test_tail_close(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_t *port, nxt_bool_t keep_fd0, const char *name)
{
    int                   pipe0[2], pipe1[2];
    nxt_buf_t             *b;
    nxt_port_recv_msg_t   msg;

    if (nxt_slow_path(pipe(pipe0) != 0 || pipe(pipe1) != 0)) {
        nxt_log_alert(thr->log, "port fd test failed to create a pipe");
        return NXT_ERROR;
    }

    /* Only the read end of each pipe travels in the message. */
    (void) close(pipe0[1]);
    (void) close(pipe1[1]);

    b = nxt_mp_zalloc(port->mem_pool, sizeof(nxt_buf_t));
    if (nxt_slow_path(b == NULL)) {
        (void) close(pipe0[0]);
        (void) close(pipe1[0]);
        return NXT_ERROR;
    }

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.port = port;
    msg.buf = b;
    msg.size = sizeof(nxt_port_msg_t);
    msg.fd[0] = pipe0[0];
    msg.fd[1] = pipe1[0];
    msg.port_msg.type = keep_fd0 ? _NXT_PORT_MSG_STATUS : _NXT_PORT_MSG_OOSM;

    nxt_port_test_run_read_msg_process(task, port, &msg);

    if (nxt_slow_path(keep_fd0 != nxt_port_fd_test_is_open(pipe0[0]))) {
        nxt_log_alert(thr->log, "port fd test: %s left fd[0] %s", name,
                      keep_fd0 ? "closed" : "open");
        goto fail;
    }

    if (nxt_slow_path(nxt_port_fd_test_is_open(pipe1[0]))) {
        nxt_log_alert(thr->log, "port fd test: %s leaked fd[1]", name);
        goto fail;
    }

    if (nxt_slow_path(msg.fd[0] != -1 || msg.fd[1] != -1)) {
        nxt_log_alert(thr->log, "port fd test: %s left fds in the message",
                      name);
        goto fail;
    }

    if (keep_fd0) {
        /* Ours now: the dispatcher correctly left it alone. */
        (void) close(pipe0[0]);
    }

    return NXT_OK;

fail:

    nxt_port_fd_test_close(pipe0[0]);
    nxt_port_fd_test_close(pipe1[0]);

    return NXT_ERROR;
}


/*
 * Runs a first fragment carrying descriptor A and a last fragment carrying
 * a different descriptor B through the same (stream, pid), and checks the
 * close in the merge that runs just before msg->fd[0] is overwritten by the
 * assembled message's own descriptors (fmsg->fd[0]).  Without that close,
 * B -- the last fragment's own descriptor, never seen by any handler -- is
 * dropped on the floor when the overwrite happens.
 *
 * "keep_fd0" selects the same two handlers as nxt_port_fd_test_tail_close():
 * ignoring the message (A ends up closed by the tail close, since nothing
 * took it) or blanking fd[0] (A is kept open).  B is never handed to a
 * handler at all -- it is always expected closed.
 */
static nxt_int_t
nxt_port_fd_test_frag_merge(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_t *port, nxt_bool_t keep_fd0, const char *name)
{
    int                   pipe_a[2], pipe_b[2];
    nxt_buf_t             *b;
    nxt_port_recv_msg_t   msg;

    if (nxt_slow_path(pipe(pipe_a) != 0 || pipe(pipe_b) != 0)) {
        nxt_log_alert(thr->log, "port fd test failed to create a pipe");
        return NXT_ERROR;
    }

    (void) close(pipe_a[1]);
    (void) close(pipe_b[1]);

    /* First fragment: nf=0, mf=1, carries A. */

    b = nxt_mp_zalloc(port->mem_pool, sizeof(nxt_buf_t));
    if (nxt_slow_path(b == NULL)) {
        (void) close(pipe_a[0]);
        (void) close(pipe_b[0]);
        return NXT_ERROR;
    }

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.port = port;
    msg.buf = b;
    msg.size = sizeof(nxt_port_msg_t);
    msg.fd[0] = pipe_a[0];
    msg.fd[1] = -1;
    msg.port_msg.stream = 0x46524147; /* "FRAG" -- distinct from other tests */
    msg.port_msg.pid = nxt_pid;
    msg.port_msg.mf = 1;
    msg.port_msg.type = keep_fd0 ? _NXT_PORT_MSG_STATUS : _NXT_PORT_MSG_OOSM;

    nxt_port_test_run_read_msg_process(task, port, &msg);

    /* Last fragment: nf=1, mf=0, carries a *different* descriptor, B. */

    b = nxt_mp_zalloc(port->mem_pool, sizeof(nxt_buf_t));
    if (nxt_slow_path(b == NULL)) {
        nxt_port_fd_test_close(pipe_a[0]);
        (void) close(pipe_b[0]);
        return NXT_ERROR;
    }

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.port = port;
    msg.buf = b;
    msg.size = sizeof(nxt_port_msg_t);
    msg.fd[0] = pipe_b[0];
    msg.fd[1] = -1;
    msg.port_msg.stream = 0x46524147;
    msg.port_msg.pid = nxt_pid;
    msg.port_msg.nf = 1;

    nxt_port_test_run_read_msg_process(task, port, &msg);

    if (nxt_slow_path(keep_fd0 != nxt_port_fd_test_is_open(pipe_a[0]))) {
        nxt_log_alert(thr->log, "port fd test: %s left A %s", name,
                      keep_fd0 ? "closed" : "open");
        goto fail;
    }

    if (nxt_slow_path(nxt_port_fd_test_is_open(pipe_b[0]))) {
        nxt_log_alert(thr->log, "port fd test: %s leaked B, the last "
                      "fragment's own descriptor", name);
        goto fail;
    }

    if (keep_fd0) {
        /* Ours now: the handler kept it, the merge did not touch it. */
        (void) close(pipe_a[0]);
    }

    return NXT_OK;

fail:

    nxt_port_fd_test_close(pipe_a[0]);
    nxt_port_fd_test_close(pipe_b[0]);

    return NXT_ERROR;
}


nxt_int_t
nxt_port_fd_test(nxt_thread_t *thr)
{
    nxt_mp_t                 *mp;
    nxt_buf_t                *b;
    nxt_fd_t                 fd0, fd1;
    nxt_task_t               *task;
    nxt_int_t                ret;
    nxt_port_t               *port, *existing;
    nxt_runtime_t            *rt, *saved_rt;
    nxt_port_recv_msg_t      msg;
    nxt_event_engine_t       *engine, *saved_engine;
    nxt_port_msg_new_port_t  new_port_msg;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port fd test started");

    task = thr->task;
    task->thread = thr;

    /* Defined before the first goto done: the cleanup there releases them. */
    port = NULL;
    existing = NULL;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    engine = nxt_mp_zalloc(mp, sizeof(nxt_event_engine_t));
    b = nxt_mp_zalloc(mp, sizeof(nxt_buf_t));

    if (nxt_slow_path(rt == NULL || engine == NULL || b == NULL)) {
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
    saved_engine = thr->engine;

    thr->runtime = rt;

    /*
     * nxt_port_write_enable() takes the address of a work queue inside the
     * engine, so a zeroed one is enough to reach the end of the handler.
     */
    thr->engine = engine;

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    /*
     * The mmap handler refuses a message without a segment descriptor.  The
     * refusal used to return without closing the second descriptor, which
     * the sender is free to attach regardless of the message type.
     */
    ret = nxt_port_fd_test_closed(thr, task, &msg, nxt_port_mmap_handler, 0,
                                  "mmap without a segment fd");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * A segment announced by a process the runtime does not know: the
     * handler releases the segment descriptor on that path but used to keep
     * the second one.
     */
    msg.port_msg.pid = nxt_pid + 1;

    ret = nxt_port_fd_test_closed(thr, task, &msg, nxt_port_mmap_handler, 1,
                                  "mmap from an unknown process");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * NEW_PORT naming a port that exists but has no queue yet.  This is
     * the normal path in the main process -- WHOAMI creates every
     * application port before its NEW_PORT arrives -- so the socket is
     * refused, but the queue descriptor has to reach the caller: closing
     * it here would leave main's worker ports queueless and strand every
     * CHANGE_FILE and QUIT main sends through them.
     */
    nxt_memzero(&new_port_msg, sizeof(nxt_port_msg_new_port_t));

    new_port_msg.pid = nxt_pid + 2;
    new_port_msg.id = 7;
    new_port_msg.type = NXT_PROCESS_APP;

    b->mem.pos = (u_char *) &new_port_msg;
    msg.buf = b;

    existing = nxt_runtime_process_port_create(task, rt, new_port_msg.pid,
                                               new_port_msg.id,
                                               new_port_msg.type);
    if (nxt_slow_path(existing == NULL)) {
        ret = NXT_ERROR;
        goto done;
    }

    existing->pair[0] = -1;
    existing->pair[1] = -1;
    existing->socket.fd = -1;

    fd0 = open("/dev/null", O_RDONLY);
    fd1 = open("/dev/null", O_RDONLY);

    if (nxt_slow_path(fd0 == -1 || fd1 == -1)) {
        nxt_log_alert(thr->log, "port fd test failed to open /dev/null");
        nxt_port_fd_test_close(fd0);
        nxt_port_fd_test_close(fd1);
        ret = NXT_ERROR;
        goto done;
    }

    msg.fd[0] = fd0;
    msg.fd[1] = fd1;
    msg.u.new_port = NULL;

    nxt_port_new_port_handler(task, &msg);

    if (nxt_slow_path(msg.u.new_port != existing)) {
        nxt_log_alert(thr->log, "port fd test: an existing port was not "
                      "reported back to the caller");
        nxt_port_fd_test_close(fd0);
        nxt_port_fd_test_close(fd1);
        ret = NXT_ERROR;
        goto done;
    }

    if (nxt_slow_path(msg.fd[0] != -1 || nxt_port_fd_test_is_open(fd0))) {
        nxt_log_alert(thr->log, "port fd test: an existing port did not "
                      "refuse the socket");
        nxt_port_fd_test_close(fd1);
        ret = NXT_ERROR;
        goto done;
    }

    if (nxt_slow_path(msg.fd[1] != fd1 || !nxt_port_fd_test_is_open(fd1))) {
        nxt_log_alert(thr->log, "port fd test: the queue descriptor of a "
                      "queueless existing port was not left to the caller");
        ret = NXT_ERROR;
        goto done;
    }

    nxt_port_fd_test_close(fd1);
    msg.fd[1] = -1;

    /*
     * NEW_PORT naming a port whose queue is already installed.  Here the
     * queue descriptor is refused with the socket: a caller that mapped
     * fd[1] would re-point a live port's queue at memory this message
     * chose.  The mapping is a real one so that the cleanup below releases
     * it the same way nxt_port_close() releases any port queue.
     */
    existing->queue = mmap(NULL, sizeof(nxt_port_queue_t),
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
    if (nxt_slow_path(existing->queue == MAP_FAILED)) {
        existing->queue = NULL;
        nxt_log_alert(thr->log, "port fd test failed to map a queue");
        ret = NXT_ERROR;
        goto done;
    }

    msg.u.new_port = NULL;

    ret = nxt_port_fd_test_closed(thr, task, &msg, nxt_port_new_port_handler,
                                  1, "new port that already has a queue");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    if (nxt_slow_path(msg.u.new_port != existing)) {
        nxt_log_alert(thr->log, "port fd test: an existing port was not "
                      "reported back to the caller");
        ret = NXT_ERROR;
        goto done;
    }

    /*
     * The one path that keeps a descriptor: a port is created, it adopts
     * the socket, and the queue descriptor is deliberately left in the
     * message for the caller to map or discard.  Pinned here because every
     * caller now treats "fd[1] still set" as "this one is mine".
     */
    new_port_msg.id = 8;

    fd0 = open("/dev/null", O_RDONLY);
    fd1 = open("/dev/null", O_RDONLY);

    if (nxt_slow_path(fd0 == -1 || fd1 == -1)) {
        nxt_log_alert(thr->log, "port fd test failed to open /dev/null");
        nxt_port_fd_test_close(fd0);
        nxt_port_fd_test_close(fd1);
        ret = NXT_ERROR;
        goto done;
    }

    msg.fd[0] = fd0;
    msg.fd[1] = fd1;
    msg.u.new_port = NULL;

    nxt_port_new_port_handler(task, &msg);

    port = msg.u.new_port;

    if (nxt_slow_path(port == NULL || port == existing)) {
        nxt_log_alert(thr->log, "port fd test: a new port was not created");
        nxt_port_fd_test_close(fd0);
        nxt_port_fd_test_close(fd1);
        ret = NXT_ERROR;
        goto done;
    }

    if (nxt_slow_path(msg.fd[0] != -1 || port->pair[1] != fd0)) {
        nxt_log_alert(thr->log, "port fd test: the new port did not take the "
                      "socket over");
        nxt_port_fd_test_close(fd1);
        ret = NXT_ERROR;
        goto done;
    }

    if (nxt_slow_path(msg.fd[1] != fd1 || !nxt_port_fd_test_is_open(fd1))) {
        nxt_log_alert(thr->log, "port fd test: the queue descriptor was not "
                      "left to the caller");
        ret = NXT_ERROR;
        goto done;
    }

    nxt_port_fd_test_close(fd1);
    msg.fd[1] = -1;

    /*
     * The dispatcher itself.  The message type is a byte off the wire, so a
     * peer can name one past the table -- or, just as easily, an in-range
     * one whose slot this process never filled: the tables are designated
     * initializers over a struct of named members, and most set only a few.
     * Neither may call anything, and both have to close what the message
     * carried, since no handler runs to take the descriptors.
     *
     * nxt_port_enable() installs the (static) dispatcher on the port; the
     * fixture engine gets a no-op enable_read so that the read side of the
     * enabling touches nothing real.
     */
    engine->event.enable_read = nxt_port_fd_test_enable_read_stub;

    nxt_port_enable(task, port, &nxt_port_fd_test_handlers);

    msg.port = port;

    msg.port_msg.type = NXT_PORT_MSG_MAX;

    ret = nxt_port_fd_test_closed(thr, task, &msg, port->handler, 1,
                                  "message type past the handler table");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    msg.port_msg.type = _NXT_PORT_MSG_NEW_PORT;    /* slot not set above */

    ret = nxt_port_fd_test_closed(thr, task, &msg, port->handler, 1,
                                  "message type with an empty handler slot");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    if (nxt_slow_path(nxt_port_fd_test_dispatched != 0)) {
        nxt_log_alert(thr->log, "port fd test: a refused type reached a "
                      "handler");
        ret = NXT_ERROR;
        goto done;
    }

    /* And a slot that is set still dispatches. */
    msg.port_msg.type = _NXT_PORT_MSG_QUIT;

    ret = nxt_port_fd_test_closed(thr, task, &msg, port->handler, 1,
                                  "message type with its slot set");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    if (nxt_slow_path(nxt_port_fd_test_dispatched != 1)) {
        nxt_log_alert(thr->log, "port fd test: a handled type did not reach "
                      "its handler");
        ret = NXT_ERROR;
        goto done;
    }

    /*
     * nxt_port_read_msg_process() itself: the tail close that reclaims
     * whatever a dispatch left in msg->fd[], and the close in the
     * last-fragment merge that runs before msg->fd[0] is overwritten by the
     * assembled message's own descriptors.
     */

    ret = nxt_port_fd_test_tail_close(thr, task, port, 0,
                                      "dispatch to a handler that ignores "
                                      "the message");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_port_fd_test_tail_close(thr, task, port, 1,
                                      "dispatch to a handler that keeps "
                                      "fd[0]");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_port_fd_test_frag_merge(thr, task, port, 0,
                                      "fragment merge, handler ignores the "
                                      "message");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = nxt_port_fd_test_frag_merge(thr, task, port, 1,
                                      "fragment merge, handler keeps fd[0]");
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = NXT_OK;

done:

    /*
     * The ports hold what their handlers adopted -- the new port its socket
     * descriptor, the existing one its queue mapping -- and nothing else
     * owns them here: leaving them to the pool teardown would leak both.
     *
     * close() releases those, but not the port: each one carries its own
     * memory pool and a reference held by rt->ports, neither of which the
     * nxt_mp_destroy() below can reach.  remove() drops that reference,
     * which is the last one, and frees the port -- so it has to happen
     * while the fixture runtime is still installed.
     */
    if (existing != NULL) {
        nxt_port_close(task, existing);
        nxt_runtime_port_remove(task, existing);
    }

    if (port != NULL) {
        nxt_port_close(task, port);
        nxt_runtime_port_remove(task, port);
    }

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    if (nxt_fast_path(ret == NXT_OK)) {
        nxt_thread_time_update(thr);
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port fd test passed");
    }

    return ret;
}
