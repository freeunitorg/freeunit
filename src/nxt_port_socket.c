
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_socket_msg.h>
#include <nxt_port_queue.h>
#include <nxt_port_memory_int.h>


#define NXT_PORT_MAX_ENQUEUE_BUF_SIZE \
          (int) (NXT_PORT_QUEUE_MSG_SIZE - sizeof(nxt_port_msg_t))


static nxt_bool_t nxt_port_can_enqueue_buf(nxt_buf_t *b);
static uint8_t nxt_port_enqueue_buf(nxt_task_t *task, nxt_port_msg_t *pm,
    void *qbuf, nxt_buf_t *b);
static nxt_int_t nxt_port_msg_chk_insert(nxt_task_t *task, nxt_port_t *port,
    nxt_port_send_msg_t *msg);
static nxt_port_send_msg_t *nxt_port_msg_alloc(const nxt_port_send_msg_t *m);
static nxt_int_t nxt_port_msg_dup_fds(nxt_port_send_msg_t *msg);
static nxt_int_t nxt_port_write_msgs(nxt_task_t *task, void *obj,
    void *data, nxt_bool_t *send_failed);
static void nxt_port_write_handler(nxt_task_t *task, void *obj, void *data);
static nxt_port_send_msg_t *nxt_port_msg_first(nxt_port_t *port);
nxt_inline nxt_bool_t nxt_port_msg_has_fd(const nxt_port_send_msg_t *msg);
nxt_inline void nxt_port_msg_fd_uncount_locked(nxt_port_t *port,
    nxt_port_send_msg_t *msg);
static void nxt_port_msg_fd_uncount(nxt_port_t *port,
    nxt_port_send_msg_t *msg);
nxt_inline void nxt_port_msg_close_fd(nxt_port_send_msg_t *msg);
nxt_inline void nxt_port_close_fds(nxt_fd_t *fd);
static nxt_buf_t *nxt_port_buf_completion(nxt_task_t *task,
    nxt_work_queue_t *wq, nxt_buf_t *b, size_t sent, nxt_bool_t mmap_mode);
static nxt_port_send_msg_t *nxt_port_msg_insert_tail(nxt_port_t *port,
    nxt_port_send_msg_t *msg);
static void nxt_port_read_handler(nxt_task_t *task, void *obj, void *data);
static void nxt_port_queue_read_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_port_read_msg_process(nxt_task_t *task, nxt_port_t *port,
    nxt_port_recv_msg_t *msg);
static nxt_buf_t *nxt_port_buf_alloc(nxt_port_t *port);
static void nxt_port_buf_free(nxt_port_t *port, nxt_buf_t *b);
static void nxt_port_announce(nxt_task_t *task, nxt_port_t *port);
static void nxt_port_rearm_now(nxt_task_t *task, nxt_port_t *port);
static void nxt_port_rearm_work_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_port_error_handler(nxt_task_t *task, void *obj, void *data);

#if (NXT_TESTS)
static nxt_uint_t  nxt_port_test_msg_alloc_failure_count;

nxt_uint_t  nxt_port_test_broadcasts;


void
nxt_port_test_msg_alloc_failures(nxt_uint_t failures)
{
    nxt_port_test_msg_alloc_failure_count = failures;
}


/*
 * Public wrapper that lets src/test/nxt_port_fail_test.c invoke the
 * static nxt_port_error_handler() directly with a synthesised port
 * and queued message — used to verify that the "queued, then write
 * failed" cleanup matches the ordering the cert/script/socket reply
 * paths now mirror after the audit fix (close fd first, queue buffer
 * completion second).  Pass NULL for `data` so use_delta does not
 * decrement for the "obj == data" case — the test owns the port
 * reference and releases it explicitly.
 */
void
nxt_port_test_run_error_handler(nxt_task_t *task, nxt_port_t *port)
{
    nxt_port_error_handler(task, &port->socket, NULL);
}


/*
 * Public wrapper that lets src/test/nxt_port_fd_test.c invoke the static
 * nxt_port_read_msg_process() directly with a synthesised port and message
 * -- used to verify the tail close of whatever a dispatch leaves in
 * msg->fd[], and the close in the last-fragment merge that runs before
 * msg->fd[0] is overwritten by the assembled message's own descriptors
 * (freeunitorg/freeunit#342, "port: close the descriptors a handler does
 * not take").
 */
void
nxt_port_test_run_read_msg_process(nxt_task_t *task, nxt_port_t *port,
    nxt_port_recv_msg_t *msg)
{
    nxt_port_read_msg_process(task, port, msg);
}

#endif


nxt_int_t
nxt_port_socket_init(nxt_task_t *task, nxt_port_t *port, size_t max_size)
{
    nxt_int_t     sndbuf, rcvbuf, size;
    nxt_socket_t  snd, rcv;

    port->socket.task = task;

    port->pair[0] = -1;
    port->pair[1] = -1;

    if (nxt_slow_path(nxt_socketpair_create(task, port->pair) != NXT_OK)) {
        goto socketpair_fail;
    }

    snd = port->pair[1];

    sndbuf = nxt_socket_getsockopt(task, snd, SOL_SOCKET, SO_SNDBUF);
    if (nxt_slow_path(sndbuf < 0)) {
        goto getsockopt_fail;
    }

    rcv = port->pair[0];

    rcvbuf = nxt_socket_getsockopt(task, rcv, SOL_SOCKET, SO_RCVBUF);
    if (nxt_slow_path(rcvbuf < 0)) {
        goto getsockopt_fail;
    }

    if (max_size == 0) {
        max_size = 16 * 1024;
    }

    if ((size_t) sndbuf < max_size) {
        /*
         * On Unix domain sockets
         *   Linux uses 224K on both send and receive directions;
         *   FreeBSD, MacOSX, NetBSD, and OpenBSD use 2K buffer size
         *   on send direction and 4K buffer size on receive direction;
         *   Solaris uses 16K on send direction and 5K on receive direction.
         */
        (void) nxt_socket_setsockopt(task, snd, SOL_SOCKET, SO_SNDBUF,
                                     max_size);

        sndbuf = nxt_socket_getsockopt(task, snd, SOL_SOCKET, SO_SNDBUF);
        if (nxt_slow_path(sndbuf < 0)) {
            goto getsockopt_fail;
        }

        size = sndbuf * 4;

        if (rcvbuf < size) {
            (void) nxt_socket_setsockopt(task, rcv, SOL_SOCKET, SO_RCVBUF,
                                         size);

            rcvbuf = nxt_socket_getsockopt(task, rcv, SOL_SOCKET, SO_RCVBUF);
            if (nxt_slow_path(rcvbuf < 0)) {
                goto getsockopt_fail;
            }
        }
    }

    port->max_size = nxt_min(max_size, (size_t) sndbuf);
    port->max_share = (64 * 1024);

    return NXT_OK;

getsockopt_fail:

    nxt_socket_close(task, port->pair[0]);
    nxt_socket_close(task, port->pair[1]);

socketpair_fail:

    return NXT_ERROR;
}


void
nxt_port_destroy(nxt_port_t *port)
{
    nxt_socket_close(port->socket.task, port->socket.fd);
    nxt_mp_destroy(port->mem_pool);
}


void
nxt_port_write_enable(nxt_task_t *task, nxt_port_t *port)
{
    port->socket.fd = port->pair[1];
    port->socket.log = &nxt_main_log;
    port->socket.write_ready = 1;

    port->engine = task->thread->engine;

    port->socket.write_work_queue = &port->engine->fast_work_queue;
    port->socket.write_handler = nxt_port_write_handler;
    port->socket.error_handler = nxt_port_error_handler;
}


void
nxt_port_write_close(nxt_port_t *port)
{
    nxt_socket_close(port->socket.task, port->pair[1]);
    port->pair[1] = -1;
}


static void
nxt_port_release_send_msg(nxt_port_send_msg_t *msg)
{
    if (msg->allocated) {
        nxt_free(msg);
    }
}


/*
 * Give up on a message that nxt_port_write_msgs() is dropping.  Complete
 * what it still owns.
 *
 * This applies only to a message not yet in port->messages, msg->link.next
 * == NULL: the caller's stack copy from the nxt_port_msg_chk_insert()
 * NXT_DECLINED branch.  It cannot double-complete, because the
 * nxt_port_error_handler() the caller raises next completes only what the
 * queue holds, and this message was never in it.
 *
 * nxt_port_socket_write2() answers NXT_OK on the paths that still reach
 * here, on purpose.  Callers that ignore the return rely on the port to
 * finish what they handed it, such as the START_PROCESS deadline in
 * src/nxt_router.c; skipping this call would orphan those buffers instead.
 * The inline first fragment of a send to a dead peer no longer comes this
 * way: nothing of it was consumed, so it is reported and stays the
 * caller's.
 *
 * Close descriptors first, then complete buffers -- the order
 * nxt_port_error_handler() uses.
 */

static void
nxt_port_msg_drop(nxt_task_t *task, nxt_port_send_msg_t *msg)
{
    nxt_buf_t         *b, *next;
    nxt_work_queue_t  *wq;

    nxt_port_msg_close_fd(msg);

    wq = &task->thread->engine->fast_work_queue;

    for (b = msg->buf; b != NULL; b = next) {
        next = b->next;
        b->next = NULL;

        if (nxt_buf_is_sync(b)) {
            continue;
        }

        nxt_work_queue_add(wq, b->completion_handler, task, b, b->parent);
    }

    msg->buf = NULL;

    nxt_port_release_send_msg(msg);
}


nxt_int_t
nxt_port_socket_write2(nxt_task_t *task, nxt_port_t *port, nxt_uint_t type,
    nxt_fd_t fd, nxt_fd_t fd2, uint32_t stream, nxt_port_id_t reply_port,
    nxt_buf_t *b)
{
    int                  notify;
    uint8_t              qmsg_size;
    nxt_int_t            res;
    nxt_bool_t           enqueued, send_failed;
    nxt_port_send_msg_t  msg;
    struct {
        nxt_port_msg_t   pm;
        uint8_t          buf[NXT_PORT_MAX_ENQUEUE_BUF_SIZE];
    } qmsg;

    enqueued = 0;
    send_failed = 0;

    msg.link.next = NULL;
    msg.link.prev = NULL;

    msg.buf = b;
    msg.share = 0;
    msg.fd[0] = fd;
    msg.fd[1] = fd2;
    msg.close_fd = (type & NXT_PORT_MSG_CLOSE_FD) != 0;
    msg.allocated = 0;

    msg.port_msg.stream = stream;
    msg.port_msg.pid = nxt_pid;
    msg.port_msg.reply_port = reply_port;
    msg.port_msg.type = type & NXT_PORT_MSG_MASK;
    msg.port_msg.last = (type & NXT_PORT_MSG_LAST) != 0;
    msg.port_msg.mmap = 0;
    msg.port_msg.nf = 0;
    msg.port_msg.mf = 0;

    if (port->queue != NULL && type != _NXT_PORT_MSG_READ_QUEUE) {

        if (fd == -1 && nxt_port_can_enqueue_buf(b)) {
            qmsg.pm = msg.port_msg;

            qmsg_size = sizeof(qmsg.pm);

            if (b != NULL) {
                qmsg_size += nxt_port_enqueue_buf(task, &qmsg.pm, qmsg.buf, b);
            }

            res = nxt_port_queue_send(port->queue, &qmsg, qmsg_size, &notify);

            nxt_debug(task, "port{%d,%d} %d: enqueue %d notify %d, %d",
                      (int) port->pid, (int) port->id, port->socket.fd,
                      (int) qmsg_size, notify, res);

            if (b != NULL && nxt_fast_path(res == NXT_OK)) {
                if (qmsg.pm.mmap) {
                    b->is_port_mmap_sent = 1;
                }

                b->mem.pos = b->mem.free;

                nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                                   b->completion_handler, task, b, b->parent);
            }

            if (notify == 0) {
                return res;
            }

            /*
             * The payload is in the shared queue and its completion is
             * already queued: b belongs to the port from here on, whatever
             * becomes of the wake-up below.
             */
            enqueued = 1;

            msg.port_msg.type = _NXT_PORT_MSG_READ_QUEUE;
            msg.buf = NULL;

        } else {
            qmsg.buf[0] = _NXT_PORT_MSG_READ_SOCKET;

            res = nxt_port_queue_send(port->queue, qmsg.buf, 1, &notify);

            nxt_debug(task, "port{%d,%d} %d: enqueue 1 notify %d, %d",
                      (int) port->pid, (int) port->id, port->socket.fd,
                      notify, res);

            if (nxt_slow_path(res == NXT_AGAIN)) {
                return NXT_AGAIN;
            }
        }
    }

    res = nxt_port_msg_chk_insert(task, port, &msg);
    if (nxt_fast_path(res == NXT_DECLINED)) {
        /*
         * Inline, with this stack copy: the answer is whether the port took
         * the message, not whether the call ran.  On NXT_ERROR nothing of it
         * was consumed, so the caller still owns fd, fd2 and b -- the
         * contract in src/nxt_port.h, and the same state this function
         * already answers for the allocation failure above.
         */
        res = nxt_port_write_msgs(task, &port->socket, &msg, &send_failed);
    }

    if (nxt_slow_path(res != NXT_OK && enqueued)) {
        /*
         * Only the wake-up failed.  The buffer already went into the shared
         * queue above, with its completion queued, so answering anything
         * but NXT_OK would hand back a buffer this function no longer owns
         * -- and a caller that cleans up after a failed write would
         * complete it a second time.  nxt_runtime_port_send_quit()
         * (src/nxt_runtime.c) is such a caller.
         *
         * The message sits in the queue, but nothing told the peer to look.
         * nxt_port_queue_send() raises "notify" only on the 0-to-1
         * transition (src/nxt_port_queue.h), so later enqueues on this port
         * stay silent while this item keeps nitems above zero.
         *
         * After EAGAIN the peer still gets it.  That errno means the socket
         * holds data the peer has not read, and nxt_port_queue_read_handler()
         * drains the ring before it reads the socket, so the next read pass
         * takes this item too.
         *
         * ENOBUFS and ENOMEM promise nothing: nxt_socketpair_send() maps
         * both to NXT_AGAIN as well (src/nxt_socketpair.c), and they mean
         * the system could not allocate for the call, not that the peer
         * owes this port a read.
         *
         * So record that a marker is owed and re-arm.  nxt_port_announce()
         * sends it from the stack with no allocation, either here on this
         * port's own engine or from the item nxt_port_rearm() posts.
         *
         * Unless the send failed outright.  Then it did not hit a transient
         * condition: nxt_socketpair_send() answers NXT_AGAIN for EAGAIN,
         * ENOBUFS and ENOMEM and retries EINTR, so everything that reaches
         * here is an error that will not get better -- a dead peer
         * routinely, and EMSGSIZE or EBADF otherwise.  Every one of them
         * ends this port: the marker can never be delivered, and re-arming
         * the write event only brings this function back to fail the same
         * send, leaving another queued error handler each pass while the
         * event stays armed on a port that is about to be freed.
         */

        if (nxt_fast_path(!send_failed)) {
            nxt_atomic_fetch_add(&port->announce, 1);

            nxt_port_rearm(task, port);
        }

        res = NXT_OK;
    }

    return res;
}


static nxt_bool_t
nxt_port_can_enqueue_buf(nxt_buf_t *b)
{
    if (b == NULL) {
        return 1;
    }

    if (b->next != NULL) {
        return 0;
    }

    return (nxt_buf_mem_used_size(&b->mem) <= NXT_PORT_MAX_ENQUEUE_BUF_SIZE
            || nxt_buf_is_port_mmap(b));
}


static uint8_t
nxt_port_enqueue_buf(nxt_task_t *task, nxt_port_msg_t *pm, void *qbuf,
    nxt_buf_t *b)
{
    ssize_t                  size;
    nxt_port_mmap_msg_t      *mm;
    nxt_port_mmap_header_t   *hdr;
    nxt_port_mmap_handler_t  *mmap_handler;

    size = nxt_buf_mem_used_size(&b->mem);

    if (size <= NXT_PORT_MAX_ENQUEUE_BUF_SIZE) {
        nxt_memcpy(qbuf, b->mem.pos, size);

        return size;
    }

    mmap_handler = b->parent;
    hdr = mmap_handler->hdr;
    mm = qbuf;

    mm->mmap_id = hdr->id;
    mm->chunk_id = nxt_port_mmap_chunk_id(hdr, b->mem.pos);
    mm->size = nxt_buf_mem_used_size(&b->mem);

    pm->mmap = 1;

    nxt_debug(task, "mmap_msg={%D, %D, %D}", mm->mmap_id, mm->chunk_id,
              mm->size);

    return sizeof(nxt_port_mmap_msg_t);
}


static nxt_int_t
nxt_port_msg_chk_insert(nxt_task_t *task, nxt_port_t *port,
    nxt_port_send_msg_t *msg)
{
    nxt_int_t   res;
    nxt_bool_t  has_fd, over_bound;

    has_fd = nxt_port_msg_has_fd(msg);
    over_bound = 0;

    nxt_thread_mutex_lock(&port->write_mutex);

    if (nxt_fast_path(port->socket.write_ready
                      && nxt_queue_is_empty(&port->messages)))
    {
        res = NXT_DECLINED;

    } else if (nxt_slow_path(has_fd
                             && port->fd_messages >= NXT_PORT_MAX_FD_MSGS))
    {
        /*
         * Refuse rather than drop.  NXT_ERROR from here says nothing of the
         * message was consumed and it is still the caller's -- the same
         * answer this function already gives when nxt_port_msg_alloc()
         * fails, and the contract in src/nxt_port.h -- so every caller that
         * sends a descriptor already has cleanup for it.
         *
         * Logged once per stall, not once per refusal: see ->fd_refusing.
         */

        over_bound = !port->fd_refusing;
        port->fd_refusing = 1;
        res = NXT_ERROR;

    } else {
        msg = nxt_port_msg_alloc(msg);

        if (nxt_fast_path(msg != NULL)) {
            nxt_queue_insert_tail(&port->messages, &msg->link);

            if (has_fd) {
                port->fd_messages++;
                port->fd_refusing = 0;
            }

            nxt_port_use(task, port, 1);
            res = NXT_OK;

        } else {
            res = NXT_ERROR;
        }
    }

    nxt_thread_mutex_unlock(&port->write_mutex);

    if (nxt_slow_path(over_bound)) {
        nxt_alert(task, "port{%d,%d} %d: %d queued messages already hold a "
                  "descriptor, refusing to queue another",
                  (int) port->pid, (int) port->id, port->socket.fd,
                  NXT_PORT_MAX_FD_MSGS);
    }

    return res;
}


static nxt_port_send_msg_t *
nxt_port_msg_alloc(const nxt_port_send_msg_t *m)
{
    nxt_port_send_msg_t  *msg;

#if (NXT_TESTS)
    if (nxt_slow_path(nxt_port_test_msg_alloc_failure_count != 0)) {
        nxt_port_test_msg_alloc_failure_count--;
        return NULL;
    }
#endif

    msg = nxt_malloc(sizeof(nxt_port_send_msg_t));
    if (nxt_slow_path(msg == NULL)) {
        return NULL;
    }

    *msg = *m;

    msg->allocated = 1;

    /*
     * A queued message must own the descriptors it names.
     *
     * The caller's copy borrows them: NEW_PORT names another port's pair[1]
     * and queue_fd, START_PROCESS the application's shared port.  The
     * sendmsg() that puts them into SCM_RIGHTS runs later, from
     * nxt_port_write_msgs(), and by then the owner may have closed them --
     * nxt_port_close() (src/nxt_port.c) does when a port goes away -- and
     * the kernel may have given the numbers to something else.  The deferred
     * send would then fail with EBADF, or hand the peer whatever descriptor
     * now sits at that number.
     *
     * So duplicate them here, and mark the copy as owning what it sends.
     * From here on the send path closes them the way it closes any owned
     * descriptor: after the send in nxt_port_write_msgs(), or from
     * nxt_port_error_handler() and nxt_port_socket_cancel() when the message
     * is dropped instead.  The originals stay with their owner.
     *
     * The dup holds the descriptor until the message goes out, so a peer
     * that stops reading holds this side's descriptor table open in
     * proportion to what it was sent.  That is what NXT_PORT_MAX_FD_MSGS
     * bounds; the callers of this function check it before they get here.
     */

    if (!msg->close_fd && nxt_slow_path(nxt_port_msg_dup_fds(msg) != NXT_OK)) {
        nxt_free(msg);
        return NULL;
    }

    return msg;
}


/*
 * Replace a borrowed fd[0]/fd[1] with duplicates the message owns.  On
 * failure nothing is changed: a first duplicate is closed again, so the
 * caller keeps exactly its originals.
 */

static nxt_int_t
nxt_port_msg_dup_fds(nxt_port_send_msg_t *msg)
{
    nxt_fd_t  fd0, fd1;

    if (msg->fd[0] == -1 && msg->fd[1] == -1) {
        return NXT_OK;
    }

    fd0 = -1;
    fd1 = -1;

    if (msg->fd[0] != -1) {
        fd0 = fcntl(msg->fd[0], F_DUPFD_CLOEXEC, 0);
        if (nxt_slow_path(fd0 == -1)) {
            nxt_thread_log_alert("dup(%FD) failed %E", msg->fd[0], nxt_errno);
            return NXT_ERROR;
        }
    }

    if (msg->fd[1] != -1) {
        fd1 = fcntl(msg->fd[1], F_DUPFD_CLOEXEC, 0);
        if (nxt_slow_path(fd1 == -1)) {
            nxt_thread_log_alert("dup(%FD) failed %E", msg->fd[1], nxt_errno);

            if (fd0 != -1) {
                nxt_fd_close(fd0);
            }

            return NXT_ERROR;
        }
    }

    msg->fd[0] = fd0;
    msg->fd[1] = fd1;
    msg->close_fd = 1;

    return NXT_OK;
}


/*
 * Disable, not block.  Blocking leaves the descriptor registered and only
 * has the poller ignore what it reports; in edge-triggered mode that report
 * is then lost, and the enable that later re-arms a blocked event touches
 * the registration not at all.  That is fine after EAGAIN, where the
 * socket is full and the peer's next read raises a fresh edge.  After a
 * send that failed for want of kernel memory it is not: the socket stayed
 * writable, no edge is coming, and the message queued for a later attempt
 * would wait on an event that never fires.  Disabling drops the
 * registration, so the re-arm adds it back and the poller re-checks
 * readiness on the spot (#392).
 *
 * The cost is one epoll_ctl() here and one on the next EAGAIN, both off
 * the inline path.  Only the event-loop pass of nxt_port_write_msgs() --
 * data == NULL, the queue drained -- asks for this, and that pass runs on
 * port->engine, where nxt_port_post() calls the handler directly: the
 * post that allocates, and whose failure would skip this, is not reached
 * from here.
 *
 * Both tests below are repeated from the caller, for the same reason
 * nxt_port_announce() repeats its own: the descriptor, because a port can
 * be closed while this item is in flight and nxt_port_write_close() leaves
 * socket.fd at the number it had, so the disable would go out on a closed
 * or already reused descriptor (see nxt_port_rearm_now()); the event state,
 * because nxt_fd_event_disable_write() carries no guard of its own, unlike
 * the nxt_fd_event_block_write() this replaced, and an EV_DISABLE on a
 * kqueue knote that was never added is ENOENT.
 */

static void
nxt_port_fd_disable_write(nxt_task_t *task, nxt_port_t *port, void *data)
{
    if (port->pair[1] == -1 || !nxt_fd_event_is_active(port->socket.write)) {
        return;
    }

    nxt_fd_event_disable_write(task->thread->engine, &port->socket);
}


/*
 * Tell the peer the shared queue has something in it, when the marker that
 * should have said so could not be written.
 *
 * The marker is a bare header with no buffer and no descriptor, so it goes out
 * from the stack with one sendmsg() and allocates nothing -- which is the
 * point, because this runs after an allocation failure.  The flag is cleared
 * only once the marker has left: a peer that reads one drains the whole ring,
 * so anything enqueued in between is covered by the same marker.
 */

static void
nxt_port_announce(nxt_task_t *task, nxt_port_t *port)
{
    ssize_t         n;
    nxt_fd_t        fd[2];
    struct iovec    iov;
    nxt_port_msg_t  msg;

    /*
     * pair[1], not socket.fd: nxt_port_write_close() clears the first and
     * leaves the second at the number it had, so socket.fd tells you nothing
     * about whether this port may still be written to.  The caller checks the
     * same thing before it arms the event; this repeats it because the test
     * is cheap and the consequence of getting it wrong is writing to somebody
     * else's descriptor.
     */

    if (port->announce == 0 || port->pair[1] == -1) {
        return;
    }

    /* nxt_socketpair_send() reads both, whether or not it sends them. */

    fd[0] = -1;
    fd[1] = -1;

    nxt_memzero(&msg, sizeof(nxt_port_msg_t));

    msg.type = _NXT_PORT_MSG_READ_QUEUE;
    msg.pid = nxt_pid;
    msg.last = 1;

    iov.iov_base = &msg;
    iov.iov_len = sizeof(nxt_port_msg_t);

    n = nxt_socketpair_send(&port->socket, fd, &iov, 1);

    if (n == (ssize_t) sizeof(nxt_port_msg_t)) {
        nxt_atomic_fetch_add(&port->announce, -1);

        nxt_debug(task, "port{%d,%d} %d: queue announced", (int) port->pid,
                  (int) port->id, port->socket.fd);
    }
}


/*
 * The work itself, on port->engine and touching neither the flag nor the
 * reference count: both belong to whoever posted, and a caller already on
 * this engine posts nothing.
 *
 * The descriptor is checked because a port can be closed while an item is in
 * flight: nxt_port_write_close() sets pair[1] to -1 but leaves socket.fd at
 * the number it had, so enabling would register that number -- possibly
 * somebody else's by then -- against this port's handlers.  Every close runs
 * on port->engine, which is this thread, so the read is stable.
 */

static void
nxt_port_rearm_now(nxt_task_t *task, nxt_port_t *port)
{
    if (port->pair[1] == -1) {
        return;
    }

    nxt_fd_event_enable_write(task->thread->engine, &port->socket);

    nxt_port_announce(task, port);
}


/*
 * Clear ->rearm_pending before the work, not after: a poster that arrives
 * while this runs then posts again rather than dropping the re-arm, and
 * enabling the same write event twice costs nothing.  The reverse order can
 * lose one, because a poster that sees the flag set cannot know whether the
 * enable it skipped has happened yet.
 *
 * Only this handler clears it, and only because the post that queued this
 * item set it.  The flag counts posts, not re-arms.
 */

static void
nxt_port_rearm_work_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_port_t  *port;

    port = obj;

    nxt_atomic_fetch_add(&port->rearm_pending, -1);

    nxt_port_rearm_now(task, port);

    /*
     * The reference the post took.  It runs on port->engine, so this drop
     * takes the on-engine branch of nxt_port_use() and releases the port only
     * if nobody else holds it -- the same discipline nxt_port_post() uses.
     */

    nxt_port_use(task, port, -1);
}


/*
 * Re-arm the write event, and announce the queue if a marker is owed.
 *
 * On port->engine both are immediate.  From another engine they have to be
 * posted, and nxt_port_post() would allocate the item -- which fails exactly
 * when this is needed, since the write that brought us here ran out of memory
 * too.  So the port carries its own item, and ->rearm_pending keeps it off the
 * engine's queue twice: linking one item as its own successor would spin
 * nxt_locked_work_queue_add() forever.
 */

void
nxt_port_rearm(nxt_task_t *task, nxt_port_t *port)
{
    if (task->thread->engine == port->engine) {
        nxt_port_rearm_now(task, port);

        return;
    }

    if (!nxt_atomic_cmp_set(&port->rearm_pending, 0, 1)) {
        return;
    }

    nxt_atomic_fetch_add(&port->use_count, 1);

    port->rearm_work.handler = nxt_port_rearm_work_handler;
    port->rearm_work.task = &port->engine->task;
    port->rearm_work.obj = port;
    port->rearm_work.data = NULL;
    port->rearm_work.next = NULL;

    nxt_event_engine_post(port->engine, &port->rearm_work);
}


/*
 * Write what the port has and report whether an inline message was taken.
 *
 * The event loop never sets the port socket's own ->data, so data != NULL
 * marks the inline call from nxt_port_socket_write2(), with "msg" as that
 * caller's stack copy.  Only then does the return value matter -- a queued
 * message belongs to the port either way, with nobody left to tell.
 *
 * NXT_ERROR means the message was neither sent nor queued, and nothing of
 * it was consumed.  See the ownership contract this promises the caller in
 * the comment above the inline call in nxt_port_socket_write2().
 *
 * Two exits answer NXT_ERROR, both for a first fragment sent inline: an
 * EAGAIN with no memory to hold it for a later attempt, and a send that
 * fails outright, which routinely means the peer is gone.
 *
 * "send_failed" separates them for the one caller that has to tell them
 * apart, and is set on that second exit alone.  It says what it tests: the
 * sendmsg() failed with something nxt_socketpair_send() does not retry, of
 * which a dead peer is the routine case.  Such a port arms nothing -- it
 * owes no queue marker, and re-arming its write event only schedules the
 * same failing send again.
 *
 * The other three exits that raise the error handler -- a short write, and
 * two allocation failures after a fragment has gone out -- leave a socket
 * that still works, so they keep re-arming on an owed marker.  The handler
 * itself does not decide this either way: it drains the port's queued
 * messages and drops one reference, and closes nothing.
 */

static nxt_int_t
nxt_port_write_msgs(nxt_task_t *task, void *obj, void *data,
    nxt_bool_t *send_failed)
{
    int                     use_delta;
    size_t                  plain_size;
    ssize_t                 n;
    nxt_int_t               ret;
    uint32_t                mmsg_buf[3 * NXT_IOBUF_MAX * 10];
    nxt_bool_t              failed, block_write, enable_write;
    nxt_port_t              *port;
    struct iovec            iov[NXT_IOBUF_MAX * 10];
    nxt_work_queue_t        *wq;
    nxt_port_method_t       m;
    nxt_port_send_msg_t     *msg, *qmsg;
    nxt_sendbuf_coalesce_t  sb;

    port = nxt_container_of(obj, nxt_port_t, socket);

    ret = NXT_OK;
    failed = 0;
    block_write = 0;
    enable_write = 0;
    use_delta = 0;

    wq = &task->thread->engine->fast_work_queue;

    do {
        if (data) {
            msg = data;

        } else {
            msg = nxt_port_msg_first(port);

            if (msg == NULL) {
                block_write = 1;
                goto cleanup;
            }
        }

next_fragment:

        iov[0].iov_base = &msg->port_msg;
        iov[0].iov_len = sizeof(nxt_port_msg_t);

        sb.buf = msg->buf;
        sb.iobuf = &iov[1];
        sb.nmax = NXT_IOBUF_MAX - 1;
        sb.sync = 0;
        sb.last = 0;
        sb.size = 0;
        sb.limit = port->max_size;

        sb.limit_reached = 0;
        sb.nmax_reached = 0;

        m = nxt_port_mmap_get_method(task, port, msg->buf);

        if (m == NXT_PORT_METHOD_MMAP) {
            sb.limit = (1ULL << 31) - 1;
            sb.nmax = nxt_min(NXT_IOBUF_MAX * 10 - 1,
                              port->max_size / PORT_MMAP_MIN_SIZE);
        }

        sb.limit -= iov[0].iov_len;

        nxt_sendbuf_mem_coalesce(task, &sb);

        plain_size = sb.size;

        /*
         * Send through mmap enabled only when payload
         * is bigger than PORT_MMAP_MIN_SIZE.
         */
        if (m == NXT_PORT_METHOD_MMAP && plain_size > PORT_MMAP_MIN_SIZE) {
            nxt_port_mmap_write(task, port, msg, &sb, mmsg_buf);

        } else {
            m = NXT_PORT_METHOD_PLAIN;
        }

        msg->port_msg.last |= sb.last;
        msg->port_msg.mf = sb.limit_reached || sb.nmax_reached;

        n = nxt_socketpair_send(&port->socket, msg->fd, iov, sb.niov + 1);

        if (n > 0) {
            if (nxt_slow_path((size_t) n != sb.size + iov[0].iov_len)) {
                nxt_alert(task, "port %d: short write: %z instead of %uz",
                          port->socket.fd, n, sb.size + iov[0].iov_len);
                goto fail;
            }

            nxt_port_msg_fd_uncount(port, msg);

            nxt_port_msg_close_fd(msg);

            msg->buf = nxt_port_buf_completion(task, wq, msg->buf, plain_size,
                                               m == NXT_PORT_METHOD_MMAP);

            if (msg->buf != NULL) {
                nxt_debug(task, "port %d: frag stream #%uD", port->socket.fd,
                          msg->port_msg.stream);

                /*
                 * A file descriptor is sent only
                 * in the first message of a stream.
                 */
                msg->fd[0] = -1;
                msg->fd[1] = -1;
                msg->share += n;
                msg->port_msg.nf = 1;

                if (msg->share >= port->max_share) {
                    msg->share = 0;

                    if (msg->link.next != NULL) {
                        nxt_thread_mutex_lock(&port->write_mutex);

                        nxt_queue_remove(&msg->link);
                        nxt_queue_insert_tail(&port->messages, &msg->link);

                        nxt_thread_mutex_unlock(&port->write_mutex);

                    } else {
                        qmsg = nxt_port_msg_insert_tail(port, msg);
                        if (nxt_slow_path(qmsg == NULL)) {
                            nxt_port_msg_drop(task, msg);

                            goto fail;
                        }

                        msg = qmsg;

                        use_delta++;
                    }

                } else {
                    goto next_fragment;
                }

            } else {
                if (msg->link.next != NULL) {
                    nxt_thread_mutex_lock(&port->write_mutex);

                    nxt_queue_remove(&msg->link);
                    msg->link.next = NULL;

                    nxt_thread_mutex_unlock(&port->write_mutex);

                    use_delta--;
                }

                nxt_port_release_send_msg(msg);
            }

            if (data != NULL) {
                goto cleanup;
            }

        } else {
            /*
             * A send that failed outright -- routinely a dead peer, but
             * nxt_socketpair_send() retries or defers everything except a
             * non-recoverable errno, so this covers all of them.  Nothing of
             * the message was consumed -- nxt_socketpair_send() sent no
             * bytes and no descriptor, and nxt_port_msg_close_fd() runs only
             * after a successful send -- so an inline first fragment is
             * still whole and still the caller's.  Saying so is what lets
             * the caller stop waiting for a reply that is not coming.
             *
             * Once a fragment has gone out the message is no longer whole,
             * and a queued message has no caller to tell, so both keep the
             * old treatment: the port owns them, and nxt_port_msg_drop()
             * finishes them.
             */

            if (nxt_slow_path(n == NXT_ERROR)) {
                failed = 1;

                if (msg->link.next == NULL) {
                    if (data != NULL && msg->port_msg.nf == 0) {
                        ret = NXT_ERROR;

                    } else {
                        nxt_port_msg_drop(task, msg);
                    }
                }

                goto fail;
            }

            /*
             * NXT_AGAIN.  After EAGAIN the socket is full, and the peer's
             * next read raises the edge that brings this handler back.
             * After ENOBUFS or ENOMEM it is not full: the kernel could not
             * allocate for the call, and no edge is coming.  The inline
             * pass re-arms a disabled event, which is how the first retry
             * runs at all; but this pass finds the event active, and an
             * active edge-triggered event is never re-added, so a retry
             * that runs out of memory again would leave the message queued
             * for ever, and every later message on this port behind it.
             * Force the readiness re-check the drained pass does -- disable,
             * then enable -- so the poller reports the socket writable on
             * the spot.  One epoll_ctl() pair per failed retry.
             */

            if (data == NULL && port->socket.error != NXT_EAGAIN) {
                block_write = 1;
                enable_write = 1;
            }

            if (msg->link.next == NULL) {
                qmsg = nxt_port_msg_insert_tail(port, msg);
                if (nxt_slow_path(qmsg == NULL)) {
                    /*
                     * EAGAIN, then no memory to hold the message for a later
                     * attempt.  The socket is alive and nothing of the
                     * message was consumed: nxt_socketpair_send() sent no
                     * bytes and no descriptor, and nxt_port_msg_close_fd()
                     * runs only after a successful send.  So an inline
                     * caller can be told, and can act on it: keep its stream
                     * armed, or complete its own payload.
                     *
                     * This applies only to the first fragment.  Once a
                     * fragment has gone out, ->nf is set, the descriptor is
                     * closed, and the sent buffers are completed, so the
                     * port owns the cleanup, not the caller.
                     */

                    if (data != NULL && msg->port_msg.nf == 0) {
                        ret = NXT_ERROR;

                        /*
                         * Leave the loop the ordinary way instead of raising
                         * the error handler.  The socket is alive, and the
                         * ordinary exit re-arms the write event that
                         * nxt_socketpair_send() just cleared.  Without that
                         * re-arm, everything queued on this port from here
                         * on -- including work another thread inserted
                         * during this send -- would wait for a writable
                         * event that never comes.
                         */

                        break;
                    }

                    nxt_port_msg_drop(task, msg);

                    goto fail;
                }

                msg = qmsg;

                use_delta++;
            }
        }

    } while (port->socket.write_ready);

    if (nxt_fd_event_is_disabled(port->socket.write)) {
        enable_write = 1;
    }

    goto cleanup;

fail:

    use_delta++;

    nxt_work_queue_add(wq, nxt_port_error_handler, task, &port->socket,
                       &port->socket);

cleanup:

    if (block_write && nxt_fd_event_is_active(port->socket.write)) {
        nxt_port_post(task, port, nxt_port_fd_disable_write, NULL);
    }

    /*
     * An owed marker is a reason to come back even when the event needs no
     * arming: the first retry runs from the re-arm handler, which enables the
     * event, so nothing would set enable_write again and a second ENOBUFS
     * would strand the marker.  Every later pass through this function tries
     * once more.
     */

    /*
     * Not on a port whose send failed outright, and that covers the
     * event-loop pass as well as the inline one: both reach this through
     * the same "fail:" label, and neither has anything to gain from arming
     * a socket that has just refused to carry a message.
     */

    if (!failed && (enable_write || port->announce != 0)) {
        nxt_port_rearm(task, port);
    }

    if (use_delta != 0) {
        nxt_port_use(task, port, use_delta);
    }

    if (send_failed != NULL) {
        *send_failed = failed;
    }

    return ret;
}


static void
nxt_port_write_handler(nxt_task_t *task, void *obj, void *data)
{
    (void) nxt_port_write_msgs(task, obj, data, NULL);
}


nxt_port_msg_cancel_t
nxt_port_socket_cancel(nxt_task_t *task, nxt_port_t *port, nxt_uint_t type,
    uint32_t stream, nxt_port_id_t reply_port, nxt_buf_t *b)
{
    nxt_buf_t              *buf, *next;
    nxt_work_queue_t       *wq;
    nxt_port_send_msg_t    *msg, *found;
    nxt_port_msg_cancel_t  ret;

    found = NULL;
    ret = NXT_PORT_MSG_NOT_FOUND;

    nxt_thread_mutex_lock(&port->write_mutex);

    nxt_queue_each(msg, &port->messages, nxt_port_send_msg_t, link) {

        if (msg->port_msg.stream != stream
            || msg->port_msg.type != (type & NXT_PORT_MSG_MASK)
            || msg->port_msg.reply_port != reply_port)
        {
            continue;
        }

        if (b != NULL && msg->buf != b) {
            continue;
        }

        /*
         * nxt_port_write_handler() sets nf on the message once it has sent a
         * fragment, and clears msg->fd[] at the same time: from then on the
         * peer is reading a stream this message must finish.
         */

        if (msg->port_msg.nf) {
            ret = NXT_PORT_MSG_STARTED;
            break;
        }

        nxt_port_msg_fd_uncount_locked(port, msg);

        nxt_queue_remove(&msg->link);
        msg->link.next = NULL;

        found = msg;
        ret = NXT_PORT_MSG_CANCELLED;

        break;

    } nxt_queue_loop;

    nxt_thread_mutex_unlock(&port->write_mutex);

    if (found == NULL) {
        return ret;
    }

    /*
     * Outside the mutex from here: a completion handler may reach arbitrary
     * router code, and nxt_port_use() can destroy the port.
     */

    buf = found->buf;
    found->buf = NULL;

    nxt_port_msg_close_fd(found);

    nxt_port_release_send_msg(found);

    wq = &task->thread->engine->fast_work_queue;

    while (buf != NULL) {
        next = buf->next;
        buf->next = NULL;

        if (!nxt_buf_is_sync(buf)) {
            nxt_work_queue_add(wq, buf->completion_handler, task, buf,
                               buf->parent);
        }

        buf = next;
    }

    /* The reference nxt_port_msg_chk_insert() took for the queued message. */

    nxt_port_use(task, port, -1);

    return ret;
}


static nxt_port_send_msg_t *
nxt_port_msg_first(nxt_port_t *port)
{
    nxt_queue_link_t     *lnk;
    nxt_port_send_msg_t  *msg;

    nxt_thread_mutex_lock(&port->write_mutex);

    lnk = nxt_queue_first(&port->messages);

    if (lnk == nxt_queue_tail(&port->messages)) {
        msg = NULL;

    } else {
        msg = nxt_queue_link_data(lnk, nxt_port_send_msg_t, link);
    }

    nxt_thread_mutex_unlock(&port->write_mutex);

    return msg;
}


nxt_inline nxt_bool_t
nxt_port_msg_has_fd(const nxt_port_send_msg_t *msg)
{
    return msg->fd[0] != -1 || msg->fd[1] != -1;
}


/*
 * Stop counting a queued message against NXT_PORT_MAX_FD_MSGS.
 *
 * The count follows one invariant: an entry of port->messages is counted
 * exactly while it carries a descriptor.  So this runs at the two moments
 * that end it -- the message leaves the queue, or it is still queued but has
 * just had its descriptors sent -- and must run before whatever clears
 * msg->fd[], since that is what it reads.  msg->link.next is the queue's own
 * "is it in the list" marker, which keeps the inline stack copy in
 * nxt_port_socket_write2() out of the count.
 *
 * The _locked form is for the callers that already hold port->write_mutex.
 */

nxt_inline void
nxt_port_msg_fd_uncount_locked(nxt_port_t *port, nxt_port_send_msg_t *msg)
{
    /*
     * The count is never below zero by the invariant, so the test on it
     * only guards against a bug elsewhere.  It is cheap, and a wrap is
     * worse than the leak the bound fixes: at UINT32_MAX the port would
     * refuse every descriptor for the rest of its life.
     */

    if (msg->link.next != NULL && nxt_port_msg_has_fd(msg)
        && nxt_fast_path(port->fd_messages != 0))
    {
        port->fd_messages--;
    }
}


/*
 * The test before the lock only spares the mutex for the common message
 * with no descriptor.  It is repeated under the lock, because
 * nxt_port_socket_cancel() can take the same message out of the queue, and
 * uncount it, between the two.
 */

static void
nxt_port_msg_fd_uncount(nxt_port_t *port, nxt_port_send_msg_t *msg)
{
    if (msg->link.next == NULL || !nxt_port_msg_has_fd(msg)) {
        return;
    }

    nxt_thread_mutex_lock(&port->write_mutex);

    nxt_port_msg_fd_uncount_locked(port, msg);

    nxt_thread_mutex_unlock(&port->write_mutex);
}


nxt_inline void
nxt_port_msg_close_fd(nxt_port_send_msg_t *msg)
{
    if (!msg->close_fd) {
        return;
    }

    nxt_port_close_fds(msg->fd);
}


nxt_inline void
nxt_port_close_fds(nxt_fd_t *fd)
{
    if (fd[0] != -1) {
        nxt_fd_close(fd[0]);
        fd[0] = -1;
    }

    if (fd[1] != -1) {
        nxt_fd_close(fd[1]);
        fd[1] = -1;
    }
}


static nxt_buf_t *
nxt_port_buf_completion(nxt_task_t *task, nxt_work_queue_t *wq, nxt_buf_t *b,
    size_t sent, nxt_bool_t mmap_mode)
{
    size_t     size;
    nxt_buf_t  *next;

    while (b != NULL) {

        nxt_prefetch(b->next);

        if (!nxt_buf_is_sync(b)) {

            size = nxt_buf_used_size(b);

            if (size != 0) {

                if (sent == 0) {
                    break;
                }

                if (nxt_buf_is_port_mmap(b) && mmap_mode) {
                    /*
                     * buffer has been sent to other side which is now
                     * responsible for shared memory bucket release
                     */
                    b->is_port_mmap_sent = 1;
                }

                if (sent < size) {

                    if (nxt_buf_is_mem(b)) {
                        b->mem.pos += sent;
                    }

                    if (nxt_buf_is_file(b)) {
                        b->file_pos += sent;
                    }

                    break;
                }

                /* b->mem.free is NULL in file-only buffer. */
                b->mem.pos = b->mem.free;

                if (nxt_buf_is_file(b)) {
                    b->file_pos = b->file_end;
                }

                sent -= size;
            }
        }

        nxt_work_queue_add(wq, b->completion_handler, task, b, b->parent);

        next = b->next;
        b->next = NULL;
        b = next;
    }

    return b;
}


/*
 * The bound applies here as well: this is the other way a descriptor-carrying
 * message enters port->messages, when an inline first fragment hits EAGAIN and
 * has to be held for a later attempt.  Answering NULL is what an allocation
 * failure already answers, and nxt_port_write_msgs() turns both into the same
 * "nothing was consumed" NXT_ERROR for a first fragment.
 */

static nxt_port_send_msg_t *
nxt_port_msg_insert_tail(nxt_port_t *port, nxt_port_send_msg_t *msg)
{
    nxt_bool_t  has_fd, log;

    has_fd = nxt_port_msg_has_fd(msg);

    nxt_thread_mutex_lock(&port->write_mutex);

    if (nxt_slow_path(has_fd && port->fd_messages >= NXT_PORT_MAX_FD_MSGS)) {
        log = !port->fd_refusing;
        port->fd_refusing = 1;

        nxt_thread_mutex_unlock(&port->write_mutex);

        if (log) {
            nxt_thread_log_alert("port{%d,%d}: %d queued messages already "
                                 "hold a descriptor, refusing to queue "
                                 "another", (int) port->pid, (int) port->id,
                                 NXT_PORT_MAX_FD_MSGS);
        }

        return NULL;
    }

    if (msg->allocated == 0) {
        msg = nxt_port_msg_alloc(msg);

        if (nxt_slow_path(msg == NULL)) {
            nxt_thread_mutex_unlock(&port->write_mutex);

            return NULL;
        }
    }

    nxt_queue_insert_tail(&port->messages, &msg->link);

    if (has_fd) {
        port->fd_messages++;
        port->fd_refusing = 0;
    }

    nxt_thread_mutex_unlock(&port->write_mutex);

    return msg;
}


void
nxt_port_read_enable(nxt_task_t *task, nxt_port_t *port)
{
    port->socket.fd = port->pair[0];
    port->socket.log = &nxt_main_log;

    port->engine = task->thread->engine;

    port->socket.read_work_queue = &port->engine->fast_work_queue;
    port->socket.read_handler = port->queue != NULL
                                ? nxt_port_queue_read_handler
                                : nxt_port_read_handler;
    port->socket.error_handler = nxt_port_error_handler;

    nxt_fd_event_enable_read(port->engine, &port->socket);
}


void
nxt_port_read_close(nxt_port_t *port)
{
    port->socket.read_ready = 0;
    port->socket.read = NXT_EVENT_INACTIVE;
    nxt_socket_close(port->socket.task, port->pair[0]);
    port->pair[0] = -1;
}


/*
 * Report a message whose control data could not be taken, on the way to
 * dropping it.
 *
 * Truncation is called out separately because it is the case a reader of the
 * log cannot otherwise infer: the payload and, on Linux, the kernel
 * credential arrived intact, and only the SCM_RIGHTS was cut -- so what a
 * handler would see is a well-formed, authenticated message whose descriptor
 * is -1.  The message is named by type and stream, which is what ties the
 * loss to the start, mmap or port hand-off that will not complete.
 *
 * "n" is the payload length recvmsg() returned; below sizeof(nxt_port_msg_t)
 * there is no header to name the message by.
 */

static void
nxt_port_oob_error_alert(nxt_task_t *task, nxt_port_t *port,
    nxt_recv_oob_t *oob, nxt_port_recv_msg_t *msg, ssize_t n)
{
    if (nxt_fast_path(!oob->truncated)) {
        nxt_alert(task, "failed to get oob data from %d", port->socket.fd);

        return;
    }

    if (n >= (ssize_t) sizeof(nxt_port_msg_t)) {
        nxt_alert(task, "port{%d,%d} %d: control data truncated on message "
                        "type %d stream #%uD; message dropped",
                  (int) port->pid, (int) port->id, port->socket.fd,
                  (int) msg->port_msg.type, msg->port_msg.stream);

    } else {
        nxt_alert(task, "port{%d,%d} %d: control data truncated on a %z byte "
                        "message; message dropped",
                  (int) port->pid, (int) port->id, port->socket.fd, n);
    }
}


static void
nxt_port_read_handler(nxt_task_t *task, void *obj, void *data)
{
    ssize_t              n;
    nxt_buf_t            *b;
    nxt_int_t            ret;
    nxt_port_t           *port;
    nxt_recv_oob_t       oob;
    nxt_port_recv_msg_t  msg;
    struct iovec         iov[2];

    port = msg.port = nxt_container_of(obj, nxt_port_t, socket);

    nxt_assert(port->engine == task->thread->engine);

    for ( ;; ) {
        b = nxt_port_buf_alloc(port);

        if (nxt_slow_path(b == NULL)) {
            /*
             * Buffer pool exhausted (transient OOM on port mem_pool).
             * Falling through would dereference b->mem.pos and crash;
             * disarm the read event and route through the orderly
             * error path instead.  No timer infrastructure exists for
             * ports, so there is no way to re-arm the read later:
             * terminal teardown via nxt_port_error_handler is the
             * strict improvement over the NULL dereference.
             */
            nxt_alert(task, "port{%d,%d} %d: buf alloc failed; "
                            "disabling read",
                      (int) port->pid, (int) port->id, port->socket.fd);
            nxt_fd_event_block_read(task->thread->engine, &port->socket);
            goto fail;
        }

        /* Guards against a future regression emptying the branch above. */
        nxt_assert(b != NULL);

        iov[0].iov_base = &msg.port_msg;
        iov[0].iov_len = sizeof(nxt_port_msg_t);

        iov[1].iov_base = b->mem.pos;
        iov[1].iov_len = port->max_size;

        n = nxt_socketpair_recv(&port->socket, iov, 2, &oob);

        if (n > 0) {
            msg.fd[0] = -1;
            msg.fd[1] = -1;

#if (NXT_USE_CMSG_PID)
            /*
             * Fail safe: a message without SCM_CREDENTIALS must never
             * be attributed to a valid sender PID.
             */
            msg.cmsg_pid = -1;
#endif

            ret = nxt_socket_msg_oob_get(&oob, msg.fd,
                                         nxt_recv_msg_cmsg_pid_ref(&msg));
            if (nxt_slow_path(ret != NXT_OK)) {
                nxt_port_oob_error_alert(task, port, &oob, &msg, n);

                /*
                 * Whatever part of the SCM_RIGHTS did arrive is in msg.fd;
                 * the dispatcher does not reclaim descriptors once a message
                 * is refused here, so close them before dropping it.
                 */
                nxt_port_close_fds(msg.fd);

                goto fail;
            }

            msg.buf = b;
            msg.size = n;

            nxt_port_read_msg_process(task, port, &msg);

            /*
             * To disable instant completion or buffer re-usage,
             * handler should reset 'msg.buf'.
             */
            if (msg.buf == b) {
                nxt_port_buf_free(port, b);
            }

            if (port->socket.read_ready) {
                continue;
            }

            return;
        }

        if (n == NXT_AGAIN) {
            nxt_port_buf_free(port, b);

            nxt_fd_event_enable_read(task->thread->engine, &port->socket);
            return;
        }

fail:
        /* n == 0 || error  */
        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           nxt_port_error_handler, task, &port->socket, NULL);
        return;
    }
}


static void
nxt_port_queue_read_handler(nxt_task_t *task, void *obj, void *data)
{
    ssize_t              n;
    nxt_buf_t            *b;
    nxt_int_t            ret;
    nxt_port_t           *port;
    struct iovec         iov[2];
    nxt_recv_oob_t       oob;
    nxt_port_queue_t     *queue;
    nxt_port_recv_msg_t  msg, *smsg;
    uint8_t              qmsg[NXT_PORT_QUEUE_MSG_SIZE];

    port = nxt_container_of(obj, nxt_port_t, socket);
    msg.port = port;

    nxt_assert(port->engine == task->thread->engine);

    queue = port->queue;
    nxt_atomic_fetch_add(&queue->nitems, 1);

    for ( ;; ) {

        /*
         * Fail safe: messages dequeued from the shared memory queue carry
         * neither file descriptors nor socket credentials.  Reset both up
         * front so a queue message can never be seen carrying an fd or a
         * sender PID left over from a socket message processed on a previous
         * loop iteration.  The fd reset matters for the reject paths in the
         * privileged handlers: they call nxt_port_recv_msg_close_fds(), which
         * would otherwise close a stale descriptor in this process.  The
         * socket and suspended-message paths overwrite these fields below.
         */
        msg.fd[0] = -1;
        msg.fd[1] = -1;
#if (NXT_USE_CMSG_PID)
        msg.cmsg_pid = -1;
#endif

        if (port->from_socket == 0) {
            n = nxt_port_queue_recv(queue, qmsg);

            if (n < 0 && !port->socket.read_ready) {
                nxt_atomic_fetch_add(&queue->nitems, -1);

                n = nxt_port_queue_recv(queue, qmsg);
                if (n < 0) {
                    return;
                }

                nxt_atomic_fetch_add(&queue->nitems, 1);
            }

            if (n == 1 && qmsg[0] == _NXT_PORT_MSG_READ_SOCKET) {
                port->from_socket++;

                nxt_debug(task, "port{%d,%d} %d: dequeue 1 read_socket %d",
                          (int) port->pid, (int) port->id, port->socket.fd,
                          port->from_socket);

                continue;
            }

            nxt_debug(task, "port{%d,%d} %d: dequeue %d",
                      (int) port->pid, (int) port->id, port->socket.fd,
                      (int) n);

        } else {
            if ((smsg = port->socket_msg) != NULL && smsg->size != 0) {
                msg.port_msg = smsg->port_msg;
                b = smsg->buf;
                n = smsg->size;
                msg.fd[0] = smsg->fd[0];
                msg.fd[1] = smsg->fd[1];

#if (NXT_USE_CMSG_PID)
                msg.cmsg_pid = smsg->cmsg_pid;
#endif

                smsg->size = 0;

                port->from_socket--;

                nxt_debug(task, "port{%d,%d} %d: use suspended message %d",
                          (int) port->pid, (int) port->id, port->socket.fd,
                          (int) n);

                goto process;
            }

            n = -1;
        }

        if (n < 0 && !port->socket.read_ready) {
            nxt_atomic_fetch_add(&queue->nitems, -1);
            return;
        }

        b = nxt_port_buf_alloc(port);

        if (nxt_slow_path(b == NULL)) {
            /*
             * Buffer pool exhausted (transient OOM on port mem_pool).
             * Falling through would dereference b->mem.pos in either
             * the dequeue memcpy or the iov[1] recv branch.  Disarm
             * the read event, decrement the queue counter, and route
             * through the orderly error handler; terminal teardown is
             * the strict improvement over the NULL dereference.
             */
            nxt_alert(task, "port{%d,%d} %d: buf alloc failed; "
                            "disabling read",
                      (int) port->pid, (int) port->id, port->socket.fd);
            nxt_fd_event_block_read(task->thread->engine, &port->socket);
            nxt_atomic_fetch_add(&queue->nitems, -1);
            nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                               nxt_port_error_handler, task, &port->socket,
                               NULL);
            return;
        }

        /* Guards against a future regression emptying the branch above. */
        nxt_assert(b != NULL);

        if (n >= (ssize_t) sizeof(nxt_port_msg_t)) {
            nxt_memcpy(&msg.port_msg, qmsg, sizeof(nxt_port_msg_t));

            if (n > (ssize_t) sizeof(nxt_port_msg_t)) {
                nxt_memcpy(b->mem.pos, qmsg + sizeof(nxt_port_msg_t),
                           n - sizeof(nxt_port_msg_t));
            }

        } else {
            iov[0].iov_base = &msg.port_msg;
            iov[0].iov_len = sizeof(nxt_port_msg_t);

            iov[1].iov_base = b->mem.pos;
            iov[1].iov_len = port->max_size;

            n = nxt_socketpair_recv(&port->socket, iov, 2, &oob);

            if (n > 0) {
                msg.fd[0] = -1;
                msg.fd[1] = -1;

#if (NXT_USE_CMSG_PID)
                /* Fail safe, see nxt_port_read_handler(). */
                msg.cmsg_pid = -1;
#endif

                ret = nxt_socket_msg_oob_get(&oob, msg.fd,
                                             nxt_recv_msg_cmsg_pid_ref(&msg));
                if (nxt_slow_path(ret != NXT_OK)) {
                    nxt_port_oob_error_alert(task, port, &oob, &msg, n);

                    /* See nxt_port_read_handler(). */
                    nxt_port_close_fds(msg.fd);

                    /*
                     * Unwind what this iteration took, then keep draining.
                     * Returning bare -- which is what this site did -- strands
                     * the buffer and the READ_SOCKET marker this read was
                     * consuming, and it strands whatever was queued behind
                     * them: queue->nitems is non-zero for the whole
                     * invocation, which is precisely how nxt_port_queue_send()
                     * decides a reader is already awake and skips the socket
                     * notification.  A message enqueued in that window is
                     * therefore never announced, and returning here means
                     * nobody ever comes back for it -- one refused datagram
                     * costing the port every message queued behind it.
                     *
                     * nitems is deliberately not decremented here.  The
                     * handler holds exactly one nitems credit for the whole
                     * invocation, and whichever exit it finally takes releases
                     * it -- the empty-queue check above, or the loop's own
                     * termination.  Decrementing here as well as there would
                     * give back one credit twice.
                     */
                    if (port->from_socket > 0) {
                        port->from_socket--;
                    }

                    nxt_port_buf_free(port, b);

                    continue;
                }
            }

            if (n == (ssize_t) sizeof(nxt_port_msg_t)
                && msg.port_msg.type == _NXT_PORT_MSG_READ_QUEUE)
            {
                /*
                 * A wake-up marker carries no payload and needs no
                 * descriptor, but the sender chooses the type byte and
                 * SCM_RIGHTS is already attached by the time it is read.
                 * This exit never reaches nxt_port_read_msg_process(), so
                 * the close at its tail cannot cover it.
                 */
                nxt_port_close_fds(msg.fd);

                nxt_port_buf_free(port, b);

                nxt_debug(task, "port{%d,%d} %d: recv %d read_queue",
                          (int) port->pid, (int) port->id, port->socket.fd,
                          (int) n);

                continue;
            }

            nxt_debug(task, "port{%d,%d} %d: recvmsg %d",
                      (int) port->pid, (int) port->id, port->socket.fd,
                      (int) n);

            if (n > 0) {
                if (port->from_socket == 0) {
                    nxt_debug(task, "port{%d,%d} %d: suspend message %d",
                              (int) port->pid, (int) port->id, port->socket.fd,
                              (int) n);

                    smsg = port->socket_msg;

                    if (nxt_slow_path(smsg == NULL)) {
                        smsg = nxt_mp_alloc(port->mem_pool,
                                            sizeof(nxt_port_recv_msg_t));

                        if (nxt_slow_path(smsg == NULL)) {
                            nxt_alert(task, "port{%d,%d} %d: suspend message "
                                            "failed",
                                      (int) port->pid, (int) port->id,
                                      port->socket.fd);

                            nxt_port_close_fds(msg.fd);

                            return;
                        }

                        port->socket_msg = smsg;

                    } else {
                        if (nxt_slow_path(smsg->size != 0)) {
                            nxt_alert(task, "port{%d,%d} %d: too many suspend "
                                            "messages",
                                      (int) port->pid, (int) port->id,
                                      port->socket.fd);

                            nxt_port_close_fds(msg.fd);

                            return;
                        }
                    }

                    smsg->port_msg = msg.port_msg;
                    smsg->buf = b;
                    smsg->size = n;
                    smsg->fd[0] = msg.fd[0];
                    smsg->fd[1] = msg.fd[1];

#if (NXT_USE_CMSG_PID)
                    smsg->cmsg_pid = msg.cmsg_pid;
#endif

                    continue;
                }

                port->from_socket--;
            }
        }

    process:

        if (n > 0) {
            msg.buf = b;
            msg.size = n;

            nxt_port_read_msg_process(task, port, &msg);

            /*
             * To disable instant completion or buffer re-usage,
             * handler should reset 'msg.buf'.
             */
            if (msg.buf == b) {
                nxt_port_buf_free(port, b);
            }

            continue;
        }

        if (n == NXT_AGAIN) {
            nxt_port_buf_free(port, b);

            nxt_fd_event_enable_read(task->thread->engine, &port->socket);

            continue;
        }

        /* n == 0 || n == NXT_ERROR */

        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           nxt_port_error_handler, task, &port->socket, NULL);
        return;
    }
}


typedef struct {
    uint32_t  stream;
    uint32_t  pid;
} nxt_port_frag_key_t;


static nxt_int_t
nxt_port_lvlhsh_frag_test(nxt_lvlhsh_query_t *lhq, void *data)
{
    nxt_port_recv_msg_t  *fmsg;
    nxt_port_frag_key_t  *frag_key;

    fmsg = data;
    frag_key = (nxt_port_frag_key_t *) lhq->key.start;

    if (lhq->key.length == sizeof(nxt_port_frag_key_t)
        && frag_key->stream == fmsg->port_msg.stream
        && frag_key->pid == (uint32_t) fmsg->port_msg.pid)
    {
        return NXT_OK;
    }

    return NXT_DECLINED;
}


static void *
nxt_port_lvlhsh_frag_alloc(void *ctx, size_t size)
{
    return nxt_mp_align(ctx, size, size);
}


static void
nxt_port_lvlhsh_frag_free(void *ctx, void *p)
{
    nxt_mp_free(ctx, p);
}


static const nxt_lvlhsh_proto_t  lvlhsh_frag_proto  nxt_aligned(64) = {
    NXT_LVLHSH_DEFAULT,
    nxt_port_lvlhsh_frag_test,
    nxt_port_lvlhsh_frag_alloc,
    nxt_port_lvlhsh_frag_free,
};


static nxt_port_recv_msg_t *
nxt_port_frag_start(nxt_task_t *task, nxt_port_t *port,
    nxt_port_recv_msg_t *msg)
{
    nxt_int_t            res;
    nxt_lvlhsh_query_t   lhq;
    nxt_port_recv_msg_t  *fmsg;
    nxt_port_frag_key_t  frag_key;

    nxt_debug(task, "start frag stream #%uD", msg->port_msg.stream);

    fmsg = nxt_mp_alloc(port->mem_pool, sizeof(nxt_port_recv_msg_t));

    if (nxt_slow_path(fmsg == NULL)) {
        return NULL;
    }

    *fmsg = *msg;

    frag_key.stream = fmsg->port_msg.stream;
    frag_key.pid = fmsg->port_msg.pid;

    lhq.key_hash = nxt_murmur_hash2(&frag_key, sizeof(nxt_port_frag_key_t));
    lhq.key.length = sizeof(nxt_port_frag_key_t);
    lhq.key.start = (u_char *) &frag_key;
    lhq.proto = &lvlhsh_frag_proto;
    lhq.replace = 0;
    lhq.value = fmsg;
    lhq.pool = port->mem_pool;

    res = nxt_lvlhsh_insert(&port->frags, &lhq);

    switch (res) {

    case NXT_OK:
        return fmsg;

    case NXT_DECLINED:
        nxt_log(task, NXT_LOG_WARN, "duplicate frag stream #%uD",
                fmsg->port_msg.stream);
        nxt_mp_free(port->mem_pool, fmsg);

        return NULL;

    default:
        nxt_log(task, NXT_LOG_WARN, "failed to add frag stream #%uD",
                fmsg->port_msg.stream);

        nxt_mp_free(port->mem_pool, fmsg);

        return NULL;

    }
}


static nxt_port_recv_msg_t *
nxt_port_frag_find(nxt_task_t *task, nxt_port_t *port, nxt_port_recv_msg_t *msg)
{
    nxt_int_t            res;
    nxt_bool_t           last;
    nxt_lvlhsh_query_t   lhq;
    nxt_port_frag_key_t  frag_key;

    last = msg->port_msg.mf == 0;

    nxt_debug(task, "%s frag stream #%uD", last ? "last" : "next",
              msg->port_msg.stream);

    frag_key.stream = msg->port_msg.stream;
    frag_key.pid = msg->port_msg.pid;

    lhq.key_hash = nxt_murmur_hash2(&frag_key, sizeof(nxt_port_frag_key_t));
    lhq.key.length = sizeof(nxt_port_frag_key_t);
    lhq.key.start = (u_char *) &frag_key;
    lhq.proto = &lvlhsh_frag_proto;
    lhq.pool = port->mem_pool;

    res = last != 0 ? nxt_lvlhsh_delete(&port->frags, &lhq) :
          nxt_lvlhsh_find(&port->frags, &lhq);

    switch (res) {

    case NXT_OK:
        return lhq.value;

    default:
        nxt_log(task, NXT_LOG_INFO, "frag stream #%uD not found",
                frag_key.stream);

        return NULL;
    }
}


static void
nxt_port_read_msg_process(nxt_task_t *task, nxt_port_t *port,
    nxt_port_recv_msg_t *msg)
{
    nxt_buf_t            *b, *orig_b, *next;
    nxt_port_recv_msg_t  *fmsg;

    if (nxt_slow_path(msg->size < sizeof(nxt_port_msg_t))) {
        nxt_alert(task, "port %d: too small message:%uz",
                  port->socket.fd, msg->size);

        nxt_port_close_fds(msg->fd);

        return;
    }

    /* adjust size to actual buffer used size */
    msg->size -= sizeof(nxt_port_msg_t);

    b = orig_b = msg->buf;
    b->mem.free += msg->size;

    msg->cancelled = 0;

    if (nxt_slow_path(msg->port_msg.nf != 0)) {

        fmsg = nxt_port_frag_find(task, port, msg);

        if (nxt_slow_path(fmsg == NULL)) {
            goto fmsg_failed;
        }

        if (nxt_fast_path(fmsg->cancelled == 0)) {

            if (msg->port_msg.mmap) {
                nxt_port_mmap_read(task, msg);
            }

            nxt_buf_chain_add(&fmsg->buf, msg->buf);

            fmsg->size += msg->size;
            msg->buf = NULL;
            b = NULL;

            if (nxt_fast_path(msg->port_msg.mf == 0)) {

                b = fmsg->buf;

                port->handler(task, fmsg);

                /*
                 * The assembled message's descriptors are about to overwrite
                 * this fragment's own, which would drop them unnoticed.
                 */
                nxt_port_close_fds(msg->fd);

                msg->buf = fmsg->buf;
                msg->fd[0] = fmsg->fd[0];
                msg->fd[1] = fmsg->fd[1];

                /*
                 * To disable instant completion or buffer re-usage,
                 * handler should reset 'msg.buf'.
                 */
                if (!msg->port_msg.mmap && msg->buf == b) {
                    nxt_port_buf_free(port, b);
                }
            }
        }

        if (nxt_fast_path(msg->port_msg.mf == 0)) {
            nxt_mp_free(port->mem_pool, fmsg);
        }
    } else {
        if (nxt_slow_path(msg->port_msg.mf != 0)) {

            if (msg->port_msg.mmap && msg->cancelled == 0) {
                nxt_port_mmap_read(task, msg);
                b = msg->buf;
            }

            fmsg = nxt_port_frag_start(task, port, msg);

            if (nxt_slow_path(fmsg == NULL)) {
                goto fmsg_failed;
            }

            fmsg->port_msg.nf = 0;
            fmsg->port_msg.mf = 0;

            if (nxt_fast_path(msg->cancelled == 0)) {
                msg->buf = NULL;
                msg->fd[0] = -1;
                msg->fd[1] = -1;
                b = NULL;

            } else {
                nxt_port_close_fds(msg->fd);
            }
        } else {
            if (nxt_fast_path(msg->cancelled == 0)) {

                if (msg->port_msg.mmap) {
                    nxt_port_mmap_read(task, msg);
                    b = msg->buf;
                }

                port->handler(task, msg);
            }
        }
    }

fmsg_failed:

    if (msg->port_msg.mmap && orig_b != b) {

        /*
         * To disable instant buffer completion,
         * handler should reset 'msg->buf'.
         */
        if (msg->buf == b) {
            /* complete mmap buffers */
            while (b != NULL) {
                nxt_debug(task, "complete buffer %p", b);

                nxt_work_queue_add(port->socket.read_work_queue,
                    b->completion_handler, task, b, b->parent);

                next = b->next;
                b->next = NULL;
                b = next;
            }
        }

        /* restore original buf */
        msg->buf = orig_b;
    }

    /*
     * Close whatever nothing took ownership of.  A handler that keeps a
     * descriptor sets its slot to -1, so this closes only what is genuinely
     * unclaimed, and it is idempotent for the paths that already closed.
     *
     * This sits at the tail rather than beside the handler call because the
     * paths that drop descriptors are not all handler paths: "goto
     * fmsg_failed" above skips the dispatch entirely when a fragment cannot
     * be found or started, and a middle fragment is chained into the
     * assembled message without its own fd[] being carried across.  Neither
     * runs a handler, so no per-handler close can reach them.
     *
     * The message header is attacker-chosen -- type, stream and the nf/mf
     * fragment bits are bytes off the wire, and an application holds the
     * write end of the router's main port -- so every one of those paths is
     * reachable from a compromised application, one leaked descriptor per
     * message until the table is exhausted.
     */
    nxt_port_close_fds(msg->fd);
}


static nxt_buf_t *
nxt_port_buf_alloc(nxt_port_t *port)
{
    nxt_buf_t  *b;

    if (port->free_bufs != NULL) {
        b = port->free_bufs;
        port->free_bufs = b->next;

        b->mem.pos = b->mem.start;
        b->mem.free = b->mem.start;
        b->next = NULL;
    } else {
        b = nxt_buf_mem_alloc(port->mem_pool, port->max_size, 0);
        if (nxt_slow_path(b == NULL)) {
            return NULL;
        }
    }

    return b;
}


static void
nxt_port_buf_free(nxt_port_t *port, nxt_buf_t *b)
{
    nxt_buf_chain_add(&b, port->free_bufs);
    port->free_bufs = b;
}


static void
nxt_port_error_handler(nxt_task_t *task, void *obj, void *data)
{
    int                  use_delta;
    nxt_buf_t            *b, *next;
    nxt_port_t           *port;
    nxt_work_queue_t     *wq;
    nxt_port_send_msg_t  *msg;

    nxt_debug(task, "port error handler %p", obj);
    /*
     * The bare TODO here historically asked for richer error context
     * (e.g. surfacing the actual socket.error to peers).  The handler
     * already drains all queued send messages, releases buffers, and
     * decrements port refcounts; richer error responses remain a
     * possible future enhancement.
     */

    port = nxt_container_of(obj, nxt_port_t, socket);

    use_delta = 0;

    if (obj == data) {
        use_delta--;
    }

    wq = &task->thread->engine->fast_work_queue;

    nxt_thread_mutex_lock(&port->write_mutex);

    nxt_queue_each(msg, &port->messages, nxt_port_send_msg_t, link) {

        nxt_port_msg_fd_uncount_locked(port, msg);

        nxt_port_msg_close_fd(msg);

        for (b = msg->buf; b != NULL; b = next) {
            next = b->next;
            b->next = NULL;

            if (nxt_buf_is_sync(b)) {
                continue;
            }

            nxt_work_queue_add(wq, b->completion_handler, task, b, b->parent);
        }

        nxt_queue_remove(&msg->link);
        use_delta--;

        nxt_port_release_send_msg(msg);

    } nxt_queue_loop;

    nxt_thread_mutex_unlock(&port->write_mutex);

    if (use_delta != 0) {
        nxt_port_use(task, port, use_delta);
    }
}
