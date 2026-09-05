/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for control-data truncation on a port socket
 * (src/nxt_socket_msg.h, src/nxt_socket_msg.c).
 *
 * recvmsg() sets MSG_CTRUNC when it could not deliver all of the control
 * data the sender attached.  The case that matters here is an SCM_RIGHTS the
 * kernel discards while delivering everything else -- the payload and, on
 * Linux, the SCM_CREDENTIALS the receiver's SO_PASSCRED asks for.  In
 * production that happens under descriptor pressure: scm_detach_fds() cannot
 * install the descriptors and drops the whole cmsg.
 *
 * Nothing used to look at msg_flags, so what reached a handler was a message
 * that passed every check it makes -- right size, right type, kernel-validated
 * sender -- and simply had fd[0] == -1.  NEW_PORT, MMAP and CHANGE_FILE each
 * take some fd-less path from there.
 *
 * The truncation is driven directly rather than through RLIMIT_NOFILE: the
 * receive buffer's length is the other input to the same kernel decision, and
 * unlike a descriptor limit it is deterministic and leaves the rest of the
 * test process alone.  What is asserted is the contract every receive path
 * shares, that nxt_socket_msg_oob_get() and nxt_socket_msg_oob_get_fds()
 * refuse a truncated control block -- and that they refuse it after reporting
 * whatever descriptors did arrive, since the caller can only close what it
 * has been told about.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_queue.h>
#include <nxt_runtime.h>
#include <nxt_socket_msg.h>
#include "nxt_tests.h"

#include <fcntl.h>


#define NXT_CTRUNC_TEST_FD_MAX  1024

/*
 * The control space the receiver has to keep for the credential cmsg before
 * the SCM_RIGHTS can begin.  Zero where the platform passes no credential.
 */
#if (NXT_CRED_USECMSG)
#define NXT_CTRUNC_TEST_CRED_SPACE  CMSG_SPACE(sizeof(nxt_socket_cred_t))
#else
#define NXT_CTRUNC_TEST_CRED_SPACE  0
#endif


static nxt_uint_t
nxt_port_ctrunc_test_fd_count(void)
{
    int         fd;
    nxt_uint_t  n;

    n = 0;

    for (fd = 0; fd < NXT_CTRUNC_TEST_FD_MAX; fd++) {
        if (fcntl(fd, F_GETFD) != -1) {
            n++;
        }
    }

    return n;
}


/*
 * Send one message carrying two descriptors, and receive it with a control
 * buffer of exactly "controllen" bytes, filling "oob" the way nxt_recvmsg()
 * would.  A short buffer is what makes the kernel drop the SCM_RIGHTS and
 * raise MSG_CTRUNC.
 */
static nxt_int_t
nxt_port_ctrunc_test_exchange(nxt_thread_t *thr, nxt_socket_t *pair,
    size_t controllen, nxt_recv_oob_t *oob, ssize_t *received)
{
    int             fds[2];
    u_char          payload;
    ssize_t         n;
    nxt_send_oob_t  send_oob;
    struct iovec    iov[1];
    struct msghdr   msg;

    fds[0] = open("/dev/null", O_RDONLY);
    fds[1] = open("/dev/null", O_RDONLY);

    if (fds[0] == -1 || fds[1] == -1) {
        nxt_log_alert(thr->log, "port ctrunc test failed to open /dev/null");
        goto fail;
    }

    nxt_socket_msg_oob_init(&send_oob, fds);

    payload = 0x5A;

    iov[0].iov_base = &payload;
    iov[0].iov_len = sizeof(payload);

    n = nxt_sendmsg(pair[1], iov, 1, &send_oob);

    if (n != (ssize_t) sizeof(payload)) {
        nxt_log_alert(thr->log, "port ctrunc test sendmsg failed %E",
                      nxt_errno);
        goto fail;
    }

    /*
     * sendmsg() duplicated the descriptors into the socket; this end has no
     * further use for them, and leaving them open would hide a leak on the
     * receiving side behind a constant offset.
     */
    (void) close(fds[0]);
    (void) close(fds[1]);

    fds[0] = -1;
    fds[1] = -1;

    payload = 0;

    iov[0].iov_base = &payload;
    iov[0].iov_len = sizeof(payload);

    nxt_memzero(oob, sizeof(nxt_recv_oob_t));

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = oob->buf;
    msg.msg_controllen = controllen;
    msg.msg_flags = 0;

    n = recvmsg(pair[0], &msg, 0);

    if (n != (ssize_t) sizeof(payload) || payload != 0x5A) {
        nxt_log_alert(thr->log, "port ctrunc test recvmsg failed %z %E",
                      n, nxt_errno);
        return NXT_ERROR;
    }

    oob->size = msg.msg_controllen;
    oob->truncated = (msg.msg_flags & MSG_CTRUNC) != 0;

    *received = n;

    return NXT_OK;

fail:

    if (fds[0] != -1) {
        (void) close(fds[0]);
    }

    if (fds[1] != -1) {
        (void) close(fds[1]);
    }

    return NXT_ERROR;
}


static void
nxt_port_ctrunc_test_close_fds(nxt_fd_t *fd)
{
    nxt_uint_t  i;

    for (i = 0; i < 2; i++) {
        if (fd[i] != -1) {
            (void) close(fd[i]);
            fd[i] = -1;
        }
    }
}


/*
 * Defined only where it is called: both call sites sit in the non-macOS arm
 * below, and an unused static trips -Wunused-function under -Werror on a
 * platform no CI leg builds.
 */

#if !(NXT_MACOSX)

/*
 * The regression itself: a control buffer that holds the credential but not
 * the SCM_RIGHTS.  Everything a handler inspects is intact and the message is
 * one descriptor short, which is exactly the shape the kernel delivers under
 * descriptor pressure.  Before the fix both accessors returned NXT_OK here.
 */
static nxt_int_t
nxt_port_ctrunc_test_truncated(nxt_thread_t *thr, nxt_socket_t *pair,
    size_t controllen, const char *name)
{
    ssize_t         n;
    nxt_int_t       ret;
    nxt_pid_t       pid;
    nxt_uint_t      before, after;
    nxt_fd_t        fd[2], fd2[2];
    nxt_recv_oob_t  oob;

    before = nxt_port_ctrunc_test_fd_count();

    if (nxt_port_ctrunc_test_exchange(thr, pair, controllen, &oob, &n)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (!oob.truncated) {
        nxt_log_alert(thr->log, "port ctrunc test: %s did not truncate "
                      "(controllen %uz, received %uz)",
                      name, controllen, oob.size);
        return NXT_ERROR;
    }

    fd[0] = -1;
    fd[1] = -1;

    /*
     * The two accessors are checked against the same received control block,
     * so both see the identical descriptor numbers; they are closed once, at
     * the end, rather than after each call.
     */
    ret = nxt_socket_msg_oob_get_fds(&oob, fd);

    if (ret != NXT_ERROR) {
        nxt_log_alert(thr->log, "port ctrunc test: %s: oob_get_fds accepted "
                      "a truncated control block (fd %d)", name, fd[0]);

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }

    fd2[0] = -1;
    fd2[1] = -1;
    pid = -1;

    ret = nxt_socket_msg_oob_get(&oob, fd2, &pid);

    if (ret != NXT_ERROR) {
        nxt_log_alert(thr->log, "port ctrunc test: %s: oob_get accepted a "
                      "truncated control block (fd %d, pid %PI)",
                      name, fd2[0], pid);

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }

    if (fd2[0] != fd[0] || fd2[1] != fd[1]) {
        nxt_log_alert(thr->log, "port ctrunc test: %s: the two accessors "
                      "reported different descriptors (%d %d vs %d %d)",
                      name, fd[0], fd[1], fd2[0], fd2[1]);

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }

    /*
     * Whatever the kernel did install has to come back in fd[] even though
     * the call failed, or nothing can close it.
     */
    nxt_port_ctrunc_test_close_fds(fd);

    after = nxt_port_ctrunc_test_fd_count();

    if (after != before) {
        nxt_log_alert(thr->log, "port ctrunc test: %s leaked %d descriptors",
                      name, (int) (after - before));
        return NXT_ERROR;
    }

    return NXT_OK;
}

#endif


/*
 * The control case: a buffer of the size every production receive path uses.
 * A message Unit can send never truncates against it, so the accessors accept
 * it and hand back both descriptors -- which is what makes the case above an
 * assertion about truncation rather than about the buffer being short.
 */
static nxt_int_t
nxt_port_ctrunc_test_intact(nxt_thread_t *thr, nxt_socket_t *pair)
{
    ssize_t         n;
    nxt_int_t       ret;
    nxt_pid_t       pid;
    nxt_uint_t      before, after;
    nxt_fd_t        fd[2];
    nxt_recv_oob_t  oob;

    before = nxt_port_ctrunc_test_fd_count();

    if (nxt_port_ctrunc_test_exchange(thr, pair, NXT_OOB_RECV_SIZE, &oob, &n)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (oob.truncated) {
        nxt_log_alert(thr->log, "port ctrunc test: a %uz byte control buffer "
                      "truncated a two descriptor message",
                      (size_t) NXT_OOB_RECV_SIZE);
        return NXT_ERROR;
    }

    fd[0] = -1;
    fd[1] = -1;
    pid = -1;

    ret = nxt_socket_msg_oob_get(&oob, fd, &pid);

    if (ret != NXT_OK || fd[0] == -1 || fd[1] == -1) {
        nxt_log_alert(thr->log, "port ctrunc test: intact message refused "
                      "(ret %i, fd %d %d)", ret, fd[0], fd[1]);

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }

#if (NXT_CRED_USECMSG)
    if (pid != getpid()) {
        nxt_log_alert(thr->log, "port ctrunc test: intact message carried "
                      "pid %PI, expected %PI", pid, (nxt_pid_t) getpid());

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }
#endif

    nxt_port_ctrunc_test_close_fds(fd);

    after = nxt_port_ctrunc_test_fd_count();

    if (after != before) {
        nxt_log_alert(thr->log, "port ctrunc test: intact case leaked %d "
                      "descriptors", (int) (after - before));
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * The accessor contract on its own, with no kernel behaviour in it: take a
 * control block the kernel delivered whole, mark it truncated, and require
 * the same refusal.  This is what holds everywhere -- the cases above depend
 * on how a particular kernel divides a short control buffer, and xnu does not
 * divide it the same way -- and it is also the exact shape the accessors see
 * in production, since a real truncation can deliver every descriptor it was
 * asked for and still raise MSG_CTRUNC.
 */
static nxt_int_t
nxt_port_ctrunc_test_marked(nxt_thread_t *thr, nxt_socket_t *pair)
{
    ssize_t         n;
    nxt_int_t       ret;
    nxt_pid_t       pid;
    nxt_uint_t      before, after;
    nxt_fd_t        fd[2];
    nxt_recv_oob_t  oob;

    before = nxt_port_ctrunc_test_fd_count();

    if (nxt_port_ctrunc_test_exchange(thr, pair, NXT_OOB_RECV_SIZE, &oob, &n)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (oob.truncated) {
        nxt_log_alert(thr->log, "port ctrunc test: a full size control "
                      "buffer truncated a two descriptor message");
        return NXT_ERROR;
    }

    oob.truncated = 1;

    fd[0] = -1;
    fd[1] = -1;
    pid = -1;

    ret = nxt_socket_msg_oob_get(&oob, fd, &pid);

    if (ret != NXT_ERROR) {
        nxt_log_alert(thr->log, "port ctrunc test: oob_get accepted a block "
                      "marked truncated (fd %d %d)", fd[0], fd[1]);

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }

    /*
     * Every descriptor the block carries still has to be reported, or the
     * caller that is about to drop the message cannot close them.
     */
    if (fd[0] == -1 || fd[1] == -1) {
        nxt_log_alert(thr->log, "port ctrunc test: a block marked truncated "
                      "withheld its descriptors (fd %d %d)", fd[0], fd[1]);

        nxt_port_ctrunc_test_close_fds(fd);

        return NXT_ERROR;
    }

    nxt_port_ctrunc_test_close_fds(fd);

    after = nxt_port_ctrunc_test_fd_count();

    if (after != before) {
        nxt_log_alert(thr->log, "port ctrunc test: marked case leaked %d "
                      "descriptors", (int) (after - before));
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * Linux only, deliberately narrower than NXT_CRED_USECMSG.  This fixture
 * manufactures a message the receiver must refuse by relying on the
 * credential being *enforced but not attached*: nxt_socket_msg_oob_get()
 * fails a control block that carries no credential, and with SO_PASSCRED off
 * this socketpair the kernel adds none.  Where the credential travels as
 * SCM_CREDS instead (FreeBSD), nxt_socket_msg_oob_init() attaches one itself,
 * so the same call produces a perfectly valid message and the refusal this
 * fixture is built on never happens.  The truncation cases above cover those
 * platforms; this one needs Linux semantics to construct its input at all.
 */

#if (NXT_HAVE_UCRED)

static nxt_uint_t  nxt_port_ctrunc_test_delivered;


static void
nxt_port_ctrunc_test_quit_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_port_ctrunc_test_delivered++;
}


static const nxt_port_handlers_t  nxt_port_ctrunc_test_handlers = {
    .quit = nxt_port_ctrunc_test_quit_handler,
};


static void
nxt_port_ctrunc_test_enable_read_stub(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev)
{
    /* The fixture engine polls nothing. */
}


/*
 * Send one message over "fd" with a control block the receiver will refuse.
 * "with_fd" attaches a descriptor, so that the refusal happens after the
 * descriptor has been delivered -- the shape a truncation has.
 */
static nxt_int_t
nxt_port_ctrunc_test_send_bad(nxt_thread_t *thr, nxt_socket_t fd,
    nxt_port_msg_t *port_msg)
{
    int             fds[2];
    ssize_t         n;
    nxt_send_oob_t  oob;
    struct iovec    iov[1];

    fds[0] = open("/dev/null", O_RDONLY);
    fds[1] = -1;

    if (fds[0] == -1) {
        nxt_log_alert(thr->log, "port ctrunc test failed to open /dev/null");
        return NXT_ERROR;
    }

    nxt_socket_msg_oob_init(&oob, fds);

    iov[0].iov_base = port_msg;
    iov[0].iov_len = sizeof(nxt_port_msg_t);

    n = nxt_sendmsg(fd, iov, 1, &oob);

    (void) close(fds[0]);

    if (n != (ssize_t) sizeof(nxt_port_msg_t)) {
        nxt_log_alert(thr->log, "port ctrunc test sendmsg failed %E",
                      nxt_errno);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * The control message: no control data at all, which nxt_socket_msg_oob_get()
 * accepts on any platform, so the handler dispatches it.
 */
static nxt_int_t
nxt_port_ctrunc_test_send_good(nxt_thread_t *thr, nxt_socket_t fd,
    nxt_port_msg_t *port_msg)
{
    ssize_t       n;
    struct iovec  iov[1];

    iov[0].iov_base = port_msg;
    iov[0].iov_len = sizeof(nxt_port_msg_t);

    n = nxt_sendmsg(fd, iov, 1, NULL);

    if (n != (ssize_t) sizeof(nxt_port_msg_t)) {
        nxt_log_alert(thr->log, "port ctrunc test sendmsg failed %E",
                      nxt_errno);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * Announce a socket message on the queue the way nxt_port_socket_send() does.
 * Without the READ_SOCKET marker the handler has port->from_socket == 0 and
 * suspends the socket message instead of processing it, which is a different
 * path from the one under test.
 */
static nxt_int_t
nxt_port_ctrunc_test_notify(nxt_thread_t *thr, nxt_port_queue_t *queue)
{
    int     notify;
    uint8_t qmsg;

    qmsg = _NXT_PORT_MSG_READ_SOCKET;

    if (nxt_port_queue_send(queue, &qmsg, 1, &notify) != NXT_OK) {
        nxt_log_alert(thr->log, "port ctrunc test failed to queue a "
                      "read_socket marker");
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * Put a whole message on the queue, the way nxt_port_socket_write2() does for
 * an enqueueable one, and report what it decided about the wakeup.  "notify"
 * is the crux of the stranding case: nxt_port_queue_send() clears it when
 * nitems is already non-zero, on the understanding that a reader is awake and
 * will come back round for this message.
 */
static nxt_int_t
nxt_port_ctrunc_test_enqueue(nxt_thread_t *thr, nxt_port_queue_t *queue,
    nxt_port_msg_t *port_msg, int *notify)
{
    if (nxt_port_queue_send(queue, port_msg, sizeof(nxt_port_msg_t), notify)
        != NXT_OK)
    {
        nxt_log_alert(thr->log, "port ctrunc test failed to enqueue a "
                      "queue message");
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * The queue-backed read path drops a refused message rather than failing the
 * port, and has to give back everything the iteration took before it does.
 *
 * queue->nitems is the one that bites: it is incremented on entry and read by
 * nxt_port_queue_send() in every *other* process holding this port, which
 * skips the socket notification while it is non-zero because a reader is
 * supposed to be awake.  A bare return therefore left the counter raised
 * forever, and the port was never woken for a queue-only message again --
 * one refused datagram costing the port every message after it.  The buffer
 * and the READ_SOCKET marker leaked on the same path.
 *
 * The refusal is driven by a control block the accessor rejects for a reason
 * that needs no kernel cooperation: SO_PASSCRED is left off this socketpair,
 * so the SCM_CREDENTIALS that nxt_socket_msg_oob_get() insists on never
 * arrives, and the message is refused after its descriptor was delivered --
 * the same shape, and the same cleanup path, as a truncation.  Driving an
 * actual MSG_CTRUNC here would mean exhausting the test process's descriptor
 * table, which is neither deterministic nor confined to this test.
 */
static nxt_int_t
nxt_port_ctrunc_test_queue(nxt_thread_t *thr)
{
    nxt_mp_t                  *mp;
    int                        notify;
    nxt_int_t                  ret;
    nxt_port_t                *port;
    nxt_task_t                *task;
    nxt_runtime_t             *rt, *saved_rt;
    nxt_port_msg_t             port_msg;
    nxt_port_queue_t          *queue;
    nxt_event_engine_t        *engine, *saved_engine;
    nxt_nncq_atomic_t          nitems;

    task = thr->task;
    task->thread = thr;

    port = NULL;
    queue = MAP_FAILED;
    ret = NXT_ERROR;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    engine = nxt_mp_zalloc(mp, sizeof(nxt_event_engine_t));

    if (nxt_slow_path(rt == NULL || engine == NULL)) {
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
    thr->engine = engine;

    engine->event.enable_read = nxt_port_ctrunc_test_enable_read_stub;

    port = nxt_port_new(task, 0, nxt_pid, NXT_PROCESS_APP);
    if (nxt_slow_path(port == NULL)) {
        goto done;
    }

    /*
     * Plain socketpair(), not nxt_socketpair_create(): the missing
     * SO_PASSCRED is what makes the receiver refuse the message.  The
     * non-blocking mode that nxt_socketpair_create() also sets is not
     * optional, though -- the read loop ends by receiving EAGAIN, so a
     * blocking descriptor parks the handler in recvmsg() for good.
     */
    if (nxt_slow_path(socketpair(AF_UNIX, SOCK_DGRAM, 0, port->pair) != 0)) {
        nxt_log_alert(thr->log, "port ctrunc test: socketpair failed %E",
                      nxt_errno);
        goto done;
    }

    if (nxt_slow_path(nxt_socket_nonblocking(task, port->pair[0]) != NXT_OK
                      || nxt_socket_nonblocking(task, port->pair[1]) != NXT_OK))
    {
        goto done;
    }

    port->max_size = 1024;

    /*
     * nxt_socketpair_recv() logs through ev->task; the event facility would
     * have set it when the port was created against a real engine.
     */
    port->socket.task = task;

    queue = mmap(NULL, sizeof(nxt_port_queue_t), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANON, -1, 0);

    if (nxt_slow_path(queue == MAP_FAILED)) {
        nxt_log_alert(thr->log, "port ctrunc test failed to map a queue");
        goto done;
    }

    nxt_port_queue_init(queue);

    port->queue = queue;

    nxt_port_enable(task, port, &nxt_port_ctrunc_test_handlers);
    nxt_port_read_enable(task, port);

    if (nxt_slow_path(port->socket.read_handler == NULL)) {
        nxt_log_alert(thr->log, "port ctrunc test: no queue read handler");
        goto done;
    }

    nitems = queue->nitems;

    nxt_memzero(&port_msg, sizeof(nxt_port_msg_t));
    port_msg.type = _NXT_PORT_MSG_QUIT;
    port_msg.last = 1;

    if (nxt_slow_path(nxt_port_ctrunc_test_send_bad(thr, port->pair[1],
                                                    &port_msg) != NXT_OK))
    {
        goto done;
    }

    if (nxt_slow_path(nxt_port_ctrunc_test_notify(thr, queue) != NXT_OK)) {
        goto done;
    }

    /* What the event facility would have set. */
    port->socket.read_ready = 1;

    nxt_port_ctrunc_test_delivered = 0;

    port->socket.read_handler(task, &port->socket, NULL);

    if (nxt_port_ctrunc_test_delivered != 0) {
        nxt_log_alert(thr->log, "port ctrunc test: a message with an "
                      "unusable control block was delivered");
        goto done;
    }

    if (queue->nitems != nitems) {
        nxt_log_alert(thr->log, "port ctrunc test: a refused message left "
                      "queue->nitems at %uD, was %uD; the port would never "
                      "be notified again",
                      (uint32_t) queue->nitems, (uint32_t) nitems);
        goto done;
    }

    if (port->from_socket != 0) {
        nxt_log_alert(thr->log, "port ctrunc test: a refused message left "
                      "from_socket at %d", (int) port->from_socket);
        goto done;
    }

    /*
     * The point of the counter: the very next message still has to arrive.
     */
    if (nxt_slow_path(nxt_port_ctrunc_test_send_good(thr, port->pair[1],
                                                     &port_msg) != NXT_OK))
    {
        goto done;
    }

    if (nxt_slow_path(nxt_port_ctrunc_test_notify(thr, queue) != NXT_OK)) {
        goto done;
    }

    port->socket.read_ready = 1;

    port->socket.read_handler(task, &port->socket, NULL);

    if (nxt_port_ctrunc_test_delivered != 1) {
        nxt_log_alert(thr->log, "port ctrunc test: the message after a "
                      "refused one was not delivered");
        goto done;
    }

    /*
     * The case the counter alone does not cover: a message queued inside the
     * window, after the READ_SOCKET marker but before this handler gets to
     * the datagram it announces.
     *
     * Its sender sees nitems already raised and so asks for no wakeup -- the
     * assertion below -- because a reader is supposed to be awake and about to
     * drain the queue.  That reader is this invocation.  If the refusal path
     * returns instead of carrying on round the loop, nothing is left to
     * collect the message and nothing will be scheduled to: it is stranded
     * with the counter correct and the port quiet.  Restoring nitems is
     * necessary and not sufficient.
     */

    nxt_port_ctrunc_test_delivered = 0;

    if (nxt_slow_path(nxt_port_ctrunc_test_send_bad(thr, port->pair[1],
                                                    &port_msg) != NXT_OK))
    {
        goto done;
    }

    if (nxt_slow_path(nxt_port_ctrunc_test_notify(thr, queue) != NXT_OK)) {
        goto done;
    }

    if (nxt_slow_path(nxt_port_ctrunc_test_enqueue(thr, queue, &port_msg,
                                                   &notify) != NXT_OK))
    {
        goto done;
    }

    if (notify != 0) {
        nxt_log_alert(thr->log, "port ctrunc test: the queued message asked "
                      "for a wakeup, so it is not the stranding case");
        goto done;
    }

    port->socket.read_ready = 1;

    port->socket.read_handler(task, &port->socket, NULL);

    if (nxt_port_ctrunc_test_delivered != 1) {
        nxt_log_alert(thr->log, "port ctrunc test: a message queued behind a "
                      "refused one was stranded");
        goto done;
    }

    if (queue->nitems != nitems) {
        nxt_log_alert(thr->log, "port ctrunc test: draining past a refused "
                      "message left queue->nitems at %uD, was %uD",
                      (uint32_t) queue->nitems, (uint32_t) nitems);
        goto done;
    }

    ret = NXT_OK;

done:

    /*
     * The port carries its own memory pool and a use count, and its pool
     * cleanup asserts that both descriptors are already gone -- so it has to
     * be taken down through nxt_port_close(), not with close() on the pair,
     * and then released by dropping the reference nxt_port_new() took.  The
     * queue is detached first: it is this fixture's mapping, unmapped below,
     * and nxt_port_close() would otherwise release it as its own.
     */
    if (port != NULL) {
        port->queue = NULL;

        nxt_port_close(task, port);
        nxt_port_use(task, port, -1);
    }

    if (queue != MAP_FAILED) {
        (void) munmap(queue, sizeof(nxt_port_queue_t));
    }

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    nxt_thread_mutex_destroy(&rt->processes_mutex);
    nxt_mp_destroy(mp);

    return ret;
}

#endif /* NXT_HAVE_UCRED */


nxt_int_t
nxt_port_ctrunc_test(nxt_thread_t *thr)
{
    nxt_int_t      ret;
    nxt_task_t     *task;
    nxt_socket_t   pair[2];

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port ctrunc test started");

    task = thr->task;
    task->thread = thr;

    /*
     * nxt_socketpair_create() rather than socketpair(): SO_PASSCRED is what
     * puts a credential cmsg ahead of the SCM_RIGHTS, and so what lets a
     * buffer sized for the credential alone reproduce the production shape --
     * a fully authenticated message with its descriptor missing.
     */
    if (nxt_socketpair_create(task, pair) != NXT_OK) {
        nxt_log_alert(thr->log, "port ctrunc test: socketpair failed");
        return NXT_ERROR;
    }

    ret = nxt_port_ctrunc_test_intact(thr, pair);

    if (ret == NXT_OK) {
        ret = nxt_port_ctrunc_test_marked(thr, pair);
    }

    /*
     * The cases below assert how the kernel itself divides a control buffer
     * that is too small: Linux and the BSDs run scm_detach_fds() against the
     * room that is left and raise MSG_CTRUNC, while xnu externalizes the
     * rights before it measures, so the same short receive there does not
     * report truncation at all.  Skipped rather than adapted: what they add
     * over nxt_port_ctrunc_test_marked() is the proof that a real kernel
     * produces the flag, and on a kernel that does not, there is nothing to
     * prove.  The code under test is platform independent -- nothing acts on
     * a truncation that was not reported.
     */
#if (NXT_MACOSX)

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port ctrunc test: kernel driven "
                  "truncation cases skipped on macOS");

#else

    if (ret == NXT_OK) {
        ret = nxt_port_ctrunc_test_truncated(thr, pair,
                                  NXT_CTRUNC_TEST_CRED_SPACE + CMSG_LEN(0),
                                  "credential kept, SCM_RIGHTS dropped");
    }

    if (ret == NXT_OK) {
        /*
         * The other side of the same kernel decision: room for one descriptor
         * where two were sent.  scm_detach_fds() installs what fits and still
         * raises MSG_CTRUNC, so the accessors have to report the partial
         * descriptor and refuse the message, or it leaks.
         *
         * The length is CMSG_LEN(0) + sizeof(int), not CMSG_SPACE(sizeof(int)):
         * cmsg padding rounds a one-descriptor payload up to the same space
         * two occupy, so a CMSG_SPACE-derived budget delivers both and does
         * not truncate at all.
         */
        ret = nxt_port_ctrunc_test_truncated(thr, pair,
                                  NXT_CTRUNC_TEST_CRED_SPACE
                                  + CMSG_LEN(0) + sizeof(int),
                                  "one of two descriptors delivered");
    }

#endif

    nxt_socket_close(task, pair[0]);
    nxt_socket_close(task, pair[1]);

#if (NXT_HAVE_UCRED)

    if (ret == NXT_OK) {
        ret = nxt_port_ctrunc_test_queue(thr);
    }

#else

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port ctrunc test: queue read "
                  "path case skipped without SCM_CREDENTIALS");

#endif

    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port ctrunc test passed");

    return NXT_OK;
}
