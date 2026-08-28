
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_port.h>
#include <nxt_router.h>
#include <nxt_app_queue.h>
#include <nxt_port_queue.h>


static void nxt_port_remove_pid(nxt_task_t *task, nxt_port_recv_msg_t *msg,
    nxt_pid_t pid);
static void nxt_port_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg);

static nxt_atomic_uint_t nxt_port_last_id = 1;


/*
 * Map a queue a peer sent over a port.
 *
 * mmap() succeeds when the descriptor refers to an object shorter than the
 * mapping, and the first access past the object's last page raises SIGBUS
 * in this process -- main, the prototype or the router, depending on which
 * handler took the message.  That is cheaper for a hostile peer than
 * exhausting the descriptor table, so the size has to be validated before
 * the mapping is made.
 *
 * Only a short object is refused.  Both senders do size the queue with
 * ftruncate() to exactly the size the receiver maps -- nxt_shm_open() in
 * the runtime and nxt_unit_shm_open() in libunit -- but fstat() does not
 * have to report that size back: a shm object is rounded up to a page on
 * some systems, and the queue is not a whole number of pages, so requiring
 * the exact size would refuse every legitimate queue there.  A larger
 * object is harmless -- only the first size bytes are mapped, and every
 * access uses fixed offsets derived from the same type.
 *
 * The check does not make a hostile peer harmless: the sender keeps the
 * descriptor and can shrink the object after the check, and neither
 * MAP_POPULATE nor a probe read would close that window -- both only touch
 * the pages before the shrink.  Sealing the object is not available to the
 * receiver, and is Linux-only.  What the check does close is the whole
 * class of short objects a peer can simply hand over, which is what the
 * receiver can decide on its own.
 */

void *
nxt_port_queue_mmap(nxt_task_t *task, nxt_fd_t fd, size_t size)
{
    void         *mem;
    struct stat  queue_stat;

    if (nxt_slow_path(fstat(fd, &queue_stat) == -1)) {
        nxt_log(task, NXT_LOG_WARN, "fstat(%FD) failed %E", fd, nxt_errno);

        return NULL;
    }

    /*
     * Only a short object is refused, not a differing one: the producers
     * size the queue exactly, but fstat() does not have to report that.
     * A shm object is rounded up to a page on some systems, and the queue
     * is not a whole number of pages, so requiring equality would refuse
     * every legitimate queue there.
     */
    if (nxt_slow_path(queue_stat.st_size < (off_t) size)) {
        nxt_log(task, NXT_LOG_WARN, "port queue on descriptor %FD is %O "
                "bytes, less than the %uz it must hold", fd,
                queue_stat.st_size, size);

        return NULL;
    }

    mem = nxt_mem_mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (nxt_slow_path(mem == MAP_FAILED)) {
        nxt_log(task, NXT_LOG_WARN, "mmap(%FD) failed %E", fd, nxt_errno);

        return NULL;
    }

    return mem;
}


static void
nxt_port_mp_cleanup(nxt_task_t *task, void *obj, void *data)
{
    nxt_mp_t    *mp;
    nxt_port_t  *port;

    port = obj;
    mp = data;

    nxt_assert(port->pair[0] == -1);
    nxt_assert(port->pair[1] == -1);

    nxt_assert(port->use_count == 0);
    nxt_assert(port->app_link.next == NULL);
    nxt_assert(port->idle_link.next == NULL);

    nxt_assert(nxt_queue_is_empty(&port->messages));
    nxt_assert(nxt_lvlhsh_is_empty(&port->rpc_streams));
    nxt_assert(nxt_lvlhsh_is_empty(&port->rpc_peers));

    nxt_thread_mutex_destroy(&port->write_mutex);

    nxt_mp_free(mp, port);
}


nxt_port_t *
nxt_port_new(nxt_task_t *task, nxt_port_id_t id, nxt_pid_t pid,
    nxt_process_type_t type)
{
    nxt_mp_t    *mp;
    nxt_port_t  *port;

    mp = nxt_mp_create(1024, 128, 256, 32);

    if (nxt_slow_path(mp == NULL)) {
        return NULL;
    }

    port = nxt_mp_zalloc(mp, sizeof(nxt_port_t));

    if (nxt_fast_path(port != NULL)) {
        port->id = id;
        port->pid = pid;
        port->type = type;
        port->mem_pool = mp;
        port->use_count = 1;

        nxt_mp_cleanup(mp, nxt_port_mp_cleanup, task, port, mp);

        nxt_queue_init(&port->messages);
        nxt_thread_mutex_create(&port->write_mutex);

        port->queue_fd = -1;

    } else {
        nxt_mp_destroy(mp);
    }

    nxt_thread_log_debug("port %p %d:%d new, type %d", port, pid, id, type);

    return port;
}


void
nxt_port_close(nxt_task_t *task, nxt_port_t *port)
{
    size_t  size;

    nxt_debug(task, "port %p %d:%d close, type %d", port, port->pid,
              port->id, port->type);

    if (port->pair[0] != -1) {
        nxt_port_rpc_close(task, port);

        nxt_fd_close(port->pair[0]);
        port->pair[0] = -1;
    }

    if (port->pair[1] != -1) {
        nxt_fd_close(port->pair[1]);
        port->pair[1] = -1;

        if (port->app != NULL) {
            nxt_router_app_port_close(task, port);
        }
    }

    if (port->queue_fd != -1) {
        nxt_fd_close(port->queue_fd);
        port->queue_fd = -1;
    }

    if (port->queue != NULL) {
        size = (port->id == (nxt_port_id_t) -1) ? sizeof(nxt_app_queue_t)
                                                : sizeof(nxt_port_queue_t);
        nxt_mem_munmap(port->queue, size);

        port->queue = NULL;
    }
}


static void
nxt_port_release(nxt_task_t *task, nxt_port_t *port)
{
    nxt_debug(task, "port %p %d:%d release, type %d", port, port->pid,
              port->id, port->type);

    port->app = NULL;

    if (port->link.next != NULL) {
        nxt_assert(port->process != NULL);

        nxt_process_port_remove(port);

        nxt_process_use(task, port->process, -1);
    }

    nxt_mp_release(port->mem_pool);
}


nxt_port_id_t
nxt_port_get_next_id(void)
{
    return nxt_atomic_fetch_add(&nxt_port_last_id, 1);
}


void
nxt_port_reset_next_id(void)
{
    nxt_port_last_id = 1;
}


void
nxt_port_enable(nxt_task_t *task, nxt_port_t *port,
    const nxt_port_handlers_t *handlers)
{
    port->pid = nxt_pid;
    port->handler = nxt_port_handler;
    port->data = (nxt_port_handler_t *) (handlers);

    nxt_port_read_enable(task, port);
}


/*
 * TODO(audit-V5-medium): sender-type ACL for privileged port messages.
 *
 * The audit recommends rejecting application workers / prototypes
 * that forge cert/script/socket/access-log/etc. messages to the
 * main process.  A first draft was implemented but caused
 * regressions in the isolation test suite — the kernel-validated
 * sender PID (SCM_CREDENTIALS) is not consistently available on
 * the existing socketpair setup, and the message-dispatch path
 * runs before the prototype's process registration completes.
 *
 * Reverted to the pre-audit dispatch path here and deferred the
 * ACL to a follow-up that introduces SO_PASSCRED on the relevant
 * socketpairs and updates the process-registration ordering.
 * See https://github.com/andypost/unit/pull/14 review thread.
 */


static void
nxt_port_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_port_handler_t  *handlers;

    if (nxt_fast_path(msg->port_msg.type < NXT_PORT_MSG_MAX)) {

        nxt_debug(task, "port %d: message type:%uD fds:%d,%d",
                  msg->port->socket.fd, msg->port_msg.type,
                  msg->fd[0], msg->fd[1]);

        handlers = msg->port->data;
        handlers[msg->port_msg.type](task, msg);

        return;
    }

    nxt_alert(task, "port %d: unknown message type:%uD",
              msg->port->socket.fd, msg->port_msg.type);
}


void
nxt_port_quit_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_runtime_quit(task, 0);
}


/* TODO join with process_ready and move to nxt_main_process.c */
nxt_inline void
nxt_port_send_new_port(nxt_task_t *task, nxt_runtime_t *rt,
    nxt_port_t *new_port, uint32_t stream)
{
    nxt_port_t     *port;
    nxt_process_t  *process;

    nxt_debug(task, "new port %d for process %PI",
              new_port->pair[1], new_port->pid);

    nxt_runtime_process_each(rt, process) {

        if (process->pid == new_port->pid || process->pid == nxt_pid) {
            continue;
        }

        port = nxt_process_port_first(process);

        if (nxt_proc_send_matrix[port->type][new_port->type]) {
            (void) nxt_port_send_port(task, port, new_port, stream);
        }

    } nxt_runtime_process_loop;
}


nxt_int_t
nxt_port_send_port(nxt_task_t *task, nxt_port_t *port, nxt_port_t *new_port,
    uint32_t stream)
{
    nxt_buf_t                *b;
    nxt_port_msg_new_port_t  *msg;

    b = nxt_buf_mem_ts_alloc(task, task->thread->engine->mem_pool,
                             sizeof(nxt_port_data_t));
    if (nxt_slow_path(b == NULL)) {
        return NXT_ERROR;
    }

    nxt_debug(task, "send port %FD to process %PI",
              new_port->pair[1], port->pid);

    b->mem.free += sizeof(nxt_port_msg_new_port_t);
    msg = (nxt_port_msg_new_port_t *) b->mem.pos;

    msg->id = new_port->id;
    msg->pid = new_port->pid;
    msg->max_size = port->max_size;
    msg->max_share = port->max_share;
    msg->type = new_port->type;

    return nxt_port_socket_write2(task, port, NXT_PORT_MSG_NEW_PORT,
                                  new_port->pair[1], new_port->queue_fd,
                                  stream, 0, b);
}


void
nxt_port_new_port_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_port_t               *port;
    nxt_runtime_t            *rt;
    nxt_port_msg_new_port_t  *new_port_msg;

    rt = task->thread->runtime;

    new_port_msg = (nxt_port_msg_new_port_t *) msg->buf->mem.pos;

    /* TODO check b size and make plain */

    nxt_debug(task, "new port %d received for process %PI:%d",
              msg->fd[0], new_port_msg->pid, new_port_msg->id);

    port = nxt_runtime_port_find(rt, new_port_msg->pid, new_port_msg->id);
    if (port != NULL) {
        nxt_debug(task, "port %PI:%d already exists", new_port_msg->pid,
              new_port_msg->id);

        msg->u.new_port = port;

        nxt_fd_close(msg->fd[0]);
        msg->fd[0] = -1;
        return;
    }

    port = nxt_runtime_process_port_create(task, rt, new_port_msg->pid,
                                           new_port_msg->id,
                                           new_port_msg->type);
    if (nxt_slow_path(port == NULL)) {
        return;
    }

    nxt_fd_nonblocking(task, msg->fd[0]);

    port->pair[0] = -1;
    port->pair[1] = msg->fd[0];

    /* The port owns the descriptor now. */
    msg->fd[0] = -1;

    port->max_size = new_port_msg->max_size;
    port->max_share = new_port_msg->max_share;

    port->socket.task = task;

    nxt_port_write_enable(task, port);

    msg->u.new_port = port;
}

/* TODO move to nxt_main_process.c */
void
nxt_port_process_ready_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    void           *mem;
    nxt_port_t     *port;
    nxt_process_t  *process;
    nxt_runtime_t  *rt;

    rt = task->thread->runtime;

    /* Unreferenced: main and prototype processes run a single engine. */

    /*
     * The lookup stays on msg->port_msg.pid.  The kernel-validated sender
     * PID cannot replace it here: the prototype installs this handler too,
     * and there SCM_CREDENTIALS carries the namespace-local pid of a
     * pid-isolated worker, while the runtime hash is keyed on the global
     * one (see nxt_proto_process_created_handler(), which uses the two as
     * distinct values, and nxt_process_created() refusing to register an
     * isolated pid).  Authenticating this handler needs to accept either
     * pid of the process it resolves; see the sender-ACL TODO above.
     */
    process = nxt_runtime_process_find(rt, msg->port_msg.pid);
    if (nxt_slow_path(process == NULL)) {
        nxt_port_recv_msg_close_fds(msg);
        return;
    }

    /*
     * A repeated PROCESS_READY is not rejected: the message is not
     * authenticated, so refusing it would let a forged one arriving first
     * make itself permanent and lock the legitimate sender out of its own
     * queue.  It replaces the mapping instead, as it did before this check
     * existed, and the previous mapping is released below rather than
     * leaked.  nxt_assert() is not used because it compiles out in release
     * builds, leaving no diagnostic at all.
     */
    if (nxt_slow_path(process->state == NXT_PROCESS_STATE_READY)) {
        nxt_log(task, NXT_LOG_WARN, "repeated PROCESS_READY claiming "
                "process %PI", msg->port_msg.pid);
    }

    /*
     * Both guards reject before the state is set: a message that is not
     * acted upon must leave no trace.  Marking a process ready and only
     * then rejecting it would make the next, legitimate PROCESS_READY trip
     * the guard above, and would let nxt_main_process_sigchld_handler()
     * clear the start stream of a process that never finished starting,
     * dropping the REMOVE_PID that cancels the pending start RPC.
     */
    if (nxt_slow_path(nxt_queue_is_empty(&process->ports))) {
        nxt_log(task, NXT_LOG_WARN, "PROCESS_READY claiming process %PI, "
                "which has no ports", msg->port_msg.pid);
        nxt_port_recv_msg_close_fds(msg);
        return;
    }

    process->state = NXT_PROCESS_STATE_READY;

    port = nxt_process_port_first(process);

    nxt_debug(task, "process %PI ready", msg->port_msg.pid);

    if (msg->fd[0] != -1) {
        mem = nxt_port_queue_mmap(task, msg->fd[0], sizeof(nxt_port_queue_t));

        if (nxt_fast_path(mem != NULL)) {
            /*
             * Release what a previous PROCESS_READY installed, so that
             * replacing the queue does not leak the old mapping and its
             * descriptor.  The size follows nxt_port_close() -- an app
             * queue is mapped for the port with the reserved id.
             */
            if (port->queue_fd != -1) {
                nxt_fd_close(port->queue_fd);
                port->queue_fd = -1;
            }

            if (port->queue != NULL) {
                nxt_mem_munmap(port->queue,
                               (port->id == (nxt_port_id_t) -1)
                                   ? sizeof(nxt_app_queue_t)
                                   : sizeof(nxt_port_queue_t));
                port->queue = NULL;
            }

            port->queue_fd = msg->fd[0];
            port->queue = mem;

            /* The port owns the descriptor now. */
            msg->fd[0] = -1;

        } else {
            /*
             * A refused queue leaves the port as it was.  For a repeated
             * PROCESS_READY that is the queue it already has, and the port
             * keeps working.  For the first one it is no queue at all, and
             * that is not a fallback to the socket: only libunit attaches
             * a queue descriptor to PROCESS_READY, and a libunit process
             * delivers a socket message only after dequeuing the
             * READ_SOCKET marker that nxt_port_socket_write2() emits under
             * port->queue != NULL.  A queueless sender cannot emit the
             * marker, so its messages are suspended undelivered on the
             * worker side, and the second one fails the worker's context
             * ("too many port socket messages").  The broadcast below
             * forwards queue_fd == -1, so every peer holds the same
             * unreachable copy; a QUIT can never arrive, while requests
             * still flow through the shared app queue.  See issue #231.
             *
             * The message is still not refused outright: it is not
             * authenticated, and refusing it would let a forged
             * PROCESS_READY carrying a bad descriptor keep the process
             * from ever being marked ready.  Failing the start is the
             * honest reaction to a genuine mmap failure, but it hands the
             * same forgery a way to kill a legitimate start, so it has to
             * wait for sender authentication (see the lookup comment
             * above).
             */
            if (port->queue == NULL) {
                nxt_alert(task, "process %PI ready: cannot map the queue; "
                          "the process cannot be reached on this port",
                          msg->port_msg.pid);

            } else {
                nxt_log(task, NXT_LOG_WARN, "process %PI ready: cannot map "
                        "the replacement queue, keeping the current one",
                        msg->port_msg.pid);
            }
        }
    }

    /*
     * Close whatever the sender attached and the port did not take over:
     * fd[1] is never used here, and fd[0] survives a failed mapping.
     */
    nxt_port_recv_msg_close_fds(msg);

    nxt_port_send_new_port(task, rt, port, msg->port_msg.stream);
}


void
nxt_port_mmap_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_runtime_t  *rt;
    nxt_process_t  *process;

    rt = task->thread->runtime;

    if (nxt_slow_path(msg->fd[0] == -1)) {
        nxt_log(task, NXT_LOG_WARN, "invalid fd passed with mmap message");

        return;
    }

    /*
     * Referenced, not just found: this handler is in the router worker port
     * handler table, so it runs on engines other than the one that can free
     * the process.  nxt_port_incoming_port_mmap() takes and releases
     * process->incoming.mutex without touching the reference count, so the
     * reference has to span the whole call.
     */

    process = nxt_runtime_process_ref(rt, msg->port_msg.pid);
    if (nxt_slow_path(process == NULL)) {
        nxt_log(task, NXT_LOG_WARN, "failed to get process #%PI",
                msg->port_msg.pid);

        goto fail_close;
    }

    nxt_port_incoming_port_mmap(task, process, msg->fd[0]);

    /*
     * Before fail_close: that label is shared with the process == NULL path,
     * where no reference was taken.
     */

    nxt_process_use(task, process, -1);

fail_close:

    nxt_fd_close(msg->fd[0]);
}


void
nxt_port_change_log_file(nxt_task_t *task, nxt_runtime_t *rt, nxt_uint_t slot,
    nxt_fd_t fd)
{
    nxt_buf_t      *b;
    nxt_port_t     *port;
    nxt_process_t  *process;

    nxt_debug(task, "change log file #%ui fd:%FD", slot, fd);

    nxt_runtime_process_each(rt, process) {

        if (nxt_pid == process->pid) {
            continue;
        }

        port = nxt_process_port_first(process);

        b = nxt_buf_mem_alloc(task->thread->engine->mem_pool,
                              sizeof(nxt_uint_t), 0);
        if (nxt_slow_path(b == NULL)) {
            continue;
        }

        b->mem.free = nxt_cpymem(b->mem.free, &slot, sizeof(nxt_uint_t));

        (void) nxt_port_socket_write(task, port, NXT_PORT_MSG_CHANGE_FILE,
                                     fd, 0, 0, b);

    } nxt_runtime_process_loop;
}


void
nxt_port_change_log_file_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    size_t         size;
    nxt_buf_t      *b;
    nxt_int_t      ret;
    nxt_uint_t     slot;
    nxt_file_t     *log_file;
    nxt_runtime_t  *rt;

    rt = task->thread->runtime;

    b = msg->buf;

    /*
     * The payload is whatever the sender chose to write: a message with a
     * short -- or empty -- buffer would have the slot read past its end.
     * The size the sender declared is what nxt_port_read_msg_process()
     * advanced b->mem.free by, so the used size is the only bound here.
     * nxt_assert() is not used because it compiles out in release builds,
     * which is exactly where this message arrives unauthenticated.
     */
    size = 0;

    if (nxt_fast_path(b != NULL)) {
        size = (size_t) nxt_buf_mem_used_size(&b->mem);
    }

    if (nxt_slow_path(size < sizeof(nxt_uint_t))) {
        nxt_log(task, NXT_LOG_WARN, "CHANGE_FILE with a %uz byte payload, "
                "which is too short to name a log file slot", size);

        nxt_port_recv_msg_close_fds(msg);
        return;
    }

    /* The payload is not aligned for a nxt_uint_t load. */
    nxt_memcpy(&slot, b->mem.pos, sizeof(nxt_uint_t));

    /*
     * nxt_list_elt() returns NULL past the end of the list, and every
     * process that installs this handler -- the discovery and prototype
     * processes, the router and the controller -- would then dereference
     * it.
     */
    log_file = nxt_list_elt(rt->log_files, slot);

    if (nxt_slow_path(log_file == NULL)) {
        nxt_log(task, NXT_LOG_WARN, "CHANGE_FILE claiming log file slot "
                "%ui, which does not exist", slot);

        nxt_port_recv_msg_close_fds(msg);
        return;
    }

    if (nxt_slow_path(msg->fd[0] == -1)) {
        nxt_log(task, NXT_LOG_WARN, "CHANGE_FILE claiming log file slot "
                "%ui without a descriptor to redirect it to", slot);

        nxt_port_recv_msg_close_fds(msg);
        return;
    }

    nxt_debug(task, "change log file %FD:%FD", msg->fd[0], log_file->fd);

    /*
     * A second descriptor is never used on this path, but a message can
     * carry one whatever its type: close it before the redirect, so that
     * neither outcome of the redirect leaves it behind.
     */
    if (msg->fd[1] != -1) {
        nxt_fd_close(msg->fd[1]);
        msg->fd[1] = -1;
    }

    /*
     * The old log file descriptor must be closed at the moment when no
     * other threads use it.  dup2() allows to use the old file descriptor
     * for new log file.  This change is performed atomically in the kernel.
     *
     * nxt_file_redirect() consumes the descriptor on every outcome, so drop
     * it from the message rather than letting close_fds() reach a number
     * that has already been reused.
     */
    ret = nxt_file_redirect(log_file, msg->fd[0]);

    msg->fd[0] = -1;

    if (nxt_fast_path(ret == NXT_OK)) {
        if (slot == 0) {
            (void) nxt_file_stderr(log_file);
        }
    }
}


void
nxt_port_data_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    size_t     dump_size;
    nxt_buf_t  *b;

    b = msg->buf;
    dump_size = b->mem.free - b->mem.pos;

    if (dump_size > 300) {
        dump_size = 300;
    }

    nxt_debug(task, "data: %*s", dump_size, b->mem.pos);
}


void
nxt_port_remove_notify_others(nxt_task_t *task, nxt_process_t *process)
{
    nxt_pid_t           pid;
    nxt_buf_t           *buf;
    nxt_port_t          *port;
    nxt_runtime_t       *rt;
    nxt_process_t       *p;
    nxt_process_type_t  ptype;

    pid = process->pid;

    ptype = nxt_process_type(process);

    rt = task->thread->runtime;

    nxt_runtime_process_each(rt, p) {

        if (p->pid == nxt_pid
            || p->pid == pid
            || nxt_queue_is_empty(&p->ports))
        {
            continue;
        }

        port = nxt_process_port_first(p);

        if (nxt_proc_remove_notify_matrix[ptype][port->type] == 0) {
            continue;
        }

        buf = nxt_buf_mem_ts_alloc(task, task->thread->engine->mem_pool,
                                   sizeof(pid));

        if (nxt_slow_path(buf == NULL)) {
            continue;
        }

        buf->mem.free = nxt_cpymem(buf->mem.free, &pid, sizeof(pid));

        nxt_port_socket_write(task, port, NXT_PORT_MSG_REMOVE_PID, -1,
                              process->stream, 0, buf);

    } nxt_runtime_process_loop;
}


void
nxt_port_remove_pid_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_pid_t  pid;
    nxt_buf_t  *buf;

    buf = msg->buf;

    nxt_assert(nxt_buf_used_size(buf) == sizeof(pid));

    nxt_memcpy(&pid, buf->mem.pos, sizeof(nxt_pid_t));

    nxt_port_remove_pid(task, msg, pid);
}


static void
nxt_port_remove_pid(nxt_task_t *task, nxt_port_recv_msg_t *msg,
    nxt_pid_t pid)
{
    nxt_runtime_t  *rt;
    nxt_process_t  *process;

    msg->u.removed_pid = pid;

    nxt_debug(task, "port remove pid %PI handler", pid);

    rt = task->thread->runtime;

    nxt_port_rpc_remove_peer(task, msg->port, pid);

    /*
     * Referenced even though this only runs on main engines: the constraint
     * that matters here is ownership, not which thread runs.  A router worker
     * can drop the last reference to the process at any point, and
     * nxt_process_close_ports() takes its own reference around the port loop
     * -- on a process already at zero that would be a second drop to zero and
     * so a second teardown, racing the one already posted to this engine.
     */

    process = nxt_runtime_process_ref(rt, pid);

    if (process) {
        nxt_process_close_ports(task, process);

        nxt_process_use(task, process, -1);
    }
}


void
nxt_port_empty_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_debug(task, "port empty handler");
}


typedef struct {
    nxt_work_t               work;
    nxt_port_t               *port;
    nxt_port_post_handler_t  handler;
} nxt_port_work_t;


static void
nxt_port_post_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_port_t               *port;
    nxt_port_work_t          *pw;
    nxt_port_post_handler_t  handler;

    pw = obj;
    port = pw->port;
    handler = pw->handler;

    nxt_free(pw);

    handler(task, port, data);

    nxt_port_use(task, port, -1);
}


nxt_int_t
nxt_port_post(nxt_task_t *task, nxt_port_t *port,
    nxt_port_post_handler_t handler, void *data)
{
    nxt_port_work_t  *pw;

    if (task->thread->engine == port->engine) {
        handler(task, port, data);

        return NXT_OK;
    }

    pw = nxt_zalloc(sizeof(nxt_port_work_t));

    if (nxt_slow_path(pw == NULL)) {
        return NXT_ERROR;
    }

    nxt_atomic_fetch_add(&port->use_count, 1);

    pw->work.handler = nxt_port_post_handler;
    pw->work.task = &port->engine->task;
    pw->work.obj = pw;
    pw->work.data = data;

    pw->port = port;
    pw->handler = handler;

    nxt_event_engine_post(port->engine, &pw->work);

    return NXT_OK;
}


static void
nxt_port_release_handler(nxt_task_t *task, nxt_port_t *port, void *data)
{
    /* no op */
}


void
nxt_port_use(nxt_task_t *task, nxt_port_t *port, int i)
{
    int  c;

    c = nxt_atomic_fetch_add(&port->use_count, i);

    if (i < 0 && c == -i) {

        if (port->engine == NULL || task->thread->engine == port->engine) {
            nxt_port_release(task, port);

            return;
        }

        nxt_port_post(task, port, nxt_port_release_handler, NULL);
    }
}
