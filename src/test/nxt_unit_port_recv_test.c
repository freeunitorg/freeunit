
/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for descriptor ownership on the callbacks.port_recv()
 * path of libunit (src/nxt_unit.c).
 *
 * The callback's oob_size argument is in/out: libunit passes the capacity
 * of its control buffer in and reads the length of the control data
 * actually received back out.  A callback that reports "no message"
 * without assigning it (go/port.go used to do exactly that on io.EOF and
 * on an unknown port) leaves the capacity behind, and libunit would then
 * parse the control data of the *previous* message -- still present in the
 * recycled read buffer -- as a fresh SCM_RIGHTS and close its descriptors
 * a second time.  Observed in CI as
 *
 *   close(N) failed at nxt_unit_port_release: Bad file descriptor;
 *   fd previously closed at nxt_unit_process_msg
 *
 * with the more dangerous variant silent: once the number has been reused,
 * the replay closes a live, unrelated descriptor and nothing is logged.
 *
 * Only the public libunit API is used.  A second context is allocated so
 * that nxt_unit_send_port() hands the test, through its own
 * callbacks.port_send(), the descriptors of that context's read port and
 * of its port queue.  Owning the queue lets the test play the router:
 * enqueue the READ_SOCKET notification, then answer the resulting socket
 * read from callbacks.port_recv().
 */

#include "nxt_main.h"
#include "nxt_port.h"
#include "nxt_port_queue.h"
#include "nxt_app_queue.h"
#include "nxt_unit.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>


static int   nxt_port_recv_test_failures;
static int   nxt_port_recv_test_step;
static int   nxt_port_recv_test_sock_fd = -1;
static int   nxt_port_recv_test_queue_fd = -1;
static int   nxt_port_recv_test_ctx_queue_fd = -1;
static void  *nxt_port_recv_test_ctx_queue;

static struct {
    nxt_port_msg_t           msg;
    nxt_port_msg_new_port_t  new_port;
} nxt_packed nxt_port_recv_test_msg;


static void
nxt_port_recv_test_assert(int cond, const char *name)
{
    if (cond) {
        printf("port_recv test: %-42s passed\n", name);

    } else {
        printf("port_recv test: %-42s FAILED\n", name);

        nxt_port_recv_test_failures++;
    }
}


static void
nxt_port_recv_test_handler(nxt_unit_request_info_t *req)
{
}


/*
 * Captures the read port queue of the context being announced to the
 * router.  libunit closes that descriptor as soon as the announcement is
 * sent, so keep a private duplicate.
 */
static ssize_t
nxt_port_recv_test_send(nxt_unit_ctx_t *ctx, nxt_unit_port_t *port,
    const void *buf, size_t buf_size, const void *oob, size_t oob_size)
{
    int             fds[2];
    size_t          size;
    struct msghdr   msg;
    struct cmsghdr  *cmsg;

    if (oob_size == 0 || nxt_port_recv_test_ctx_queue_fd != -1) {
        return buf_size;
    }

    msg.msg_control = (void *) oob;
    msg.msg_controllen = oob_size;

    for (cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
        size = cmsg->cmsg_len - CMSG_LEN(0);

        if (cmsg->cmsg_level == SOL_SOCKET
            && cmsg->cmsg_type == SCM_RIGHTS
            && size == 2 * sizeof(int))
        {
            memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));

            nxt_port_recv_test_ctx_queue_fd = dup(fds[1]);
        }
    }

    return buf_size;
}


/*
 * First call delivers a NEW_PORT message carrying two descriptors, leaving
 * their SCM_RIGHTS in libunit's read buffer.  Every later call reproduces
 * the shape of a callback that received nothing: return 0 and leave
 * *oob_size at the capacity libunit passed in.
 */
static ssize_t
nxt_port_recv_test_recv(nxt_unit_ctx_t *ctx, nxt_unit_port_t *port, void *buf,
    size_t buf_size, void *oob, size_t *oob_size)
{
    int             fds[2];
    struct cmsghdr  *cmsg;

    if (nxt_port_recv_test_step++ > 0) {
        return 0;
    }

    memset(&nxt_port_recv_test_msg, 0, sizeof(nxt_port_recv_test_msg));

    nxt_port_recv_test_msg.msg.stream = 1;
    nxt_port_recv_test_msg.msg.pid = getpid();
    nxt_port_recv_test_msg.msg.type = _NXT_PORT_MSG_NEW_PORT;
    nxt_port_recv_test_msg.msg.last = 1;

    nxt_port_recv_test_msg.new_port.id = 7;
    nxt_port_recv_test_msg.new_port.pid = getpid();
    nxt_port_recv_test_msg.new_port.max_size = 16 * 1024;
    nxt_port_recv_test_msg.new_port.max_share = 64 * 1024;
    nxt_port_recv_test_msg.new_port.type = NXT_PROCESS_APP;

    memcpy(buf, &nxt_port_recv_test_msg, sizeof(nxt_port_recv_test_msg));

    fds[0] = nxt_port_recv_test_sock_fd;
    fds[1] = nxt_port_recv_test_queue_fd;

    cmsg = oob;
    memset(cmsg, 0, sizeof(struct cmsghdr));

    cmsg->cmsg_len = CMSG_LEN(2 * sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;

    memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));

    *oob_size = CMSG_SPACE(2 * sizeof(int));

    return sizeof(nxt_port_recv_test_msg);
}


static int
nxt_port_recv_test_shm(size_t size)
{
    int   fd;
#if (NXT_HAVE_MEMFD_CREATE || NXT_HAVE_SHM_OPEN)
    char  name[64];

    snprintf(name, sizeof(name), NXT_SHM_PREFIX "unit.test.%d", (int) getpid());
#endif

    /*
     * Mirror nxt_unit_shm_open()'s backend ladder, including its syscall form
     * of memfd_create(): auto/shmem probes SYS_memfd_create, so a libc without
     * the wrapper still sets NXT_HAVE_MEMFD_CREATE and this file has to link.
     */

#if (NXT_HAVE_MEMFD_CREATE)

    fd = syscall(SYS_memfd_create, name, MFD_CLOEXEC);
    if (fd == -1) {
        perror("memfd_create");
        return -1;
    }

#elif (NXT_HAVE_SHM_OPEN_ANON)

    fd = shm_open(SHM_ANON, O_RDWR, 0600);
    if (fd == -1) {
        perror("shm_open(SHM_ANON)");
        return -1;
    }

#elif (NXT_HAVE_SHM_OPEN)

    shm_unlink(name);

    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        perror("shm_open");
        return -1;
    }

    shm_unlink(name);

#else

    (void) size;

    printf("port_recv test: no shared memory backend, skipped\n");
    return -1;

#endif

    if (ftruncate(fd, size) == -1) {
        perror("ftruncate");
        close(fd);
        return -1;
    }

    return fd;
}


static void
nxt_port_recv_test_notify(void)
{
    int      notify;
    uint8_t  type;

    type = _NXT_PORT_MSG_READ_SOCKET;

    (void) nxt_port_queue_send(nxt_port_recv_test_ctx_queue, &type, 1,
                               &notify);
}


int
main(void)
{
    int              ready[2], router[2], read[2], shared[2], sock[2];
    int              unrelated;
    nxt_unit_ctx_t   *ctx, *ctx2;
    nxt_unit_init_t  init;

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ready) == -1
        || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, router) == -1
        || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, read) == -1
        || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, shared) == -1
        || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sock) == -1)
    {
        perror("socketpair");
        return 1;
    }

    nxt_port_recv_test_sock_fd = sock[0];

    nxt_port_recv_test_queue_fd
        = nxt_port_recv_test_shm(sizeof(nxt_port_queue_t));

    if (nxt_port_recv_test_queue_fd == -1) {
        return 1;
    }

    memset(&init, 0, sizeof(init));

    init.callbacks.request_handler = nxt_port_recv_test_handler;
    init.callbacks.port_send = nxt_port_recv_test_send;
    init.callbacks.port_recv = nxt_port_recv_test_recv;

    init.ready_port.id.pid = getpid();
    init.ready_port.id.id = 1;
    init.ready_port.in_fd = -1;
    init.ready_port.out_fd = ready[0];

    init.ready_stream = 1;

    init.router_port.id.pid = getpid();
    init.router_port.id.id = 2;
    init.router_port.in_fd = -1;
    init.router_port.out_fd = router[0];

    init.read_port.id.pid = getpid();
    init.read_port.id.id = 3;
    init.read_port.in_fd = read[0];
    init.read_port.out_fd = read[1];

    init.shared_port_fd = shared[0];
    init.shared_queue_fd = nxt_port_recv_test_shm(sizeof(nxt_app_queue_t));
    init.log_fd = STDERR_FILENO;

    if (init.shared_queue_fd == -1) {
        return 1;
    }

    ctx = nxt_unit_init(&init);
    if (ctx == NULL) {
        printf("port_recv test: nxt_unit_init() failed\n");
        return 1;
    }

    ctx2 = nxt_unit_ctx_alloc(ctx, NULL);
    if (ctx2 == NULL || nxt_port_recv_test_ctx_queue_fd == -1) {
        printf("port_recv test: nxt_unit_ctx_alloc() failed\n");
        return 1;
    }

    nxt_port_recv_test_ctx_queue = mmap(NULL, sizeof(nxt_port_queue_t),
                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                        nxt_port_recv_test_ctx_queue_fd, 0);

    if (nxt_port_recv_test_ctx_queue == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Message 1: NEW_PORT.  fd[0] is adopted by the new port, fd[1] closed. */

    nxt_port_recv_test_notify();

    (void) nxt_unit_run_once(ctx2);

    nxt_port_recv_test_assert(
        fcntl(nxt_port_recv_test_sock_fd, F_GETFD) != -1,
        "new port keeps its socket");

    /*
     * fd[1] of that message was closed while it was processed, so the
     * number is free again and the next open() takes it: a replay would
     * destroy a descriptor that has nothing to do with Unit.
     */

    unrelated = open("/dev/null", O_RDONLY);

    /*
     * The point of this case is that the freed number is reused, so assert
     * that it actually was: without this the check below would also pass if
     * open() failed, or handed out some other descriptor the replay never
     * touches.
     */
    nxt_port_recv_test_assert(
        unrelated != -1 && unrelated == nxt_port_recv_test_queue_fd,
        "the freed queue fd number is reused by the next open()");

    /* Message 2: nothing received, *oob_size left at the capacity. */

    nxt_port_recv_test_notify();

    (void) nxt_unit_run_once(ctx2);

    nxt_port_recv_test_assert(
        fcntl(nxt_port_recv_test_sock_fd, F_GETFD) != -1,
        "live port fd survives an empty read");

    nxt_port_recv_test_assert(
        fcntl(unrelated, F_GETFD) != -1,
        "unrelated fd survives an empty read");

    if (nxt_port_recv_test_failures != 0) {
        printf("port_recv test: %d failure(s)\n",
               nxt_port_recv_test_failures);
        return 1;
    }

    printf("port_recv test: all passed\n");
    return 0;
}
