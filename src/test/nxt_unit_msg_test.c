
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Malformed port messages at libunit's receive side (src/nxt_unit.c), fed
 * through nxt_unit_test_process_msg().  nxt_unit_mmap_read() used to read a
 * partial last record past the message, mmap_id 0xFFFFFFFF wrapped the
 * lib->incoming index, and a segment of any size was accepted.  Cases that
 * could crash the old code run in a child; a signal is a failure.
 */

#include "nxt_main.h"
#include "nxt_port_memory_int.h"
#include "nxt_port_queue.h"
#include "nxt_app_queue.h"
#include "nxt_unit.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>


static int             nxt_unit_msg_test_failures;
static int             nxt_unit_msg_test_send_fails;
static int             nxt_unit_msg_test_quit_called;
static nxt_unit_ctx_t  *nxt_unit_msg_test_ctx;
static nxt_unit_ctx_t  *nxt_unit_msg_test_follower;

static int nxt_unit_msg_test_send_records_to(nxt_unit_ctx_t *ctx,
    const nxt_port_mmap_msg_t *records, size_t nrecords, size_t tail);


static void
nxt_unit_msg_test_assert(int cond, const char *name)
{
    if (cond) {
        printf("unit msg test: %-50s passed\n", name);

    } else {
        printf("unit msg test: %-50s FAILED\n", name);

        nxt_unit_msg_test_failures++;
    }
}


static void
nxt_unit_msg_test_handler(nxt_unit_request_info_t *req)
{
    nxt_unit_request_done(req, NXT_UNIT_ERROR);
}


static ssize_t
nxt_unit_msg_test_send(nxt_unit_ctx_t *ctx, nxt_unit_port_t *port,
    const void *buf, size_t buf_size, const void *oob, size_t oob_size)
{
    const nxt_port_msg_t  *msg;
    nxt_port_mmap_msg_t   rec;

    msg = buf;

    /*
     * While one context asks the router for a segment, another one reads
     * a record for the same segment and parks behind it.
     */
    if (msg->type == _NXT_PORT_MSG_GET_MMAP
        && nxt_unit_msg_test_follower != NULL)
    {
        rec.mmap_id = ((const nxt_port_msg_get_mmap_t *) (msg + 1))->id;
        rec.chunk_id = 0;
        rec.size = 100;

        (void) nxt_unit_msg_test_send_records_to(nxt_unit_msg_test_follower,
                                                 &rec, 1, 0);

        nxt_unit_msg_test_follower = NULL;
    }

    return nxt_unit_msg_test_send_fails ? -1 : (ssize_t) buf_size;
}


static ssize_t
nxt_unit_msg_test_recv(nxt_unit_ctx_t *ctx, nxt_unit_port_t *port, void *buf,
    size_t buf_size, void *oob, size_t *oob_size)
{
    *oob_size = 0;

    return 0;
}


static int
nxt_unit_msg_test_shm(size_t size)
{
    int  fd;

#if (NXT_HAVE_MEMFD_CREATE)
    fd = syscall(SYS_memfd_create, "unit.msg.test", 0);
#else
    char  name[64];

    snprintf(name, sizeof(name), "/unit.msg.test.%d", (int) getpid());

    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    shm_unlink(name);
#endif

    if (fd == -1) {
        perror("shm");
        return -1;
    }

    if (ftruncate(fd, size) == -1) {
        perror("ftruncate");
        close(fd);
        return -1;
    }

    return fd;
}


/* A segment the router would hand over, from this process to itself. */
static int
nxt_unit_msg_test_send_segment(size_t size, uint32_t id)
{
    int                     fd;
    nxt_port_msg_t          msg;
    nxt_port_mmap_header_t  *hdr;

    fd = nxt_unit_msg_test_shm(size);
    if (fd == -1) {
        return -2;
    }

    hdr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (hdr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -2;
    }

    hdr->id = id;
    hdr->src_pid = getpid();
    hdr->dst_pid = getpid();

    munmap(hdr, size);

    memset(&msg, 0, sizeof(msg));

    msg.pid = getpid();
    msg.type = _NXT_PORT_MSG_MMAP;
    msg.last = 1;

    return nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, &msg, sizeof(msg),
                                     fd);
}


/* RPC_READY with the mmap bit: the result is the mmap read's verdict. */
static int
nxt_unit_msg_test_send_records_to(nxt_unit_ctx_t *ctx,
    const nxt_port_mmap_msg_t *records, size_t nrecords, size_t tail)
{
    u_char          buf[256];
    size_t          size;
    nxt_port_msg_t  msg;

    memset(&msg, 0, sizeof(msg));

    msg.stream = 7;
    msg.pid = getpid();
    msg.type = _NXT_PORT_MSG_RPC_READY;
    msg.last = 1;
    msg.mmap = 1;

    size = nrecords * sizeof(nxt_port_mmap_msg_t);

    memcpy(buf, &msg, sizeof(msg));
    memcpy(buf + sizeof(msg), records, size);
    memset(buf + sizeof(msg) + size, 0, tail);

    return nxt_unit_test_process_msg(ctx, buf, sizeof(msg) + size + tail, -1);
}


static int
nxt_unit_msg_test_send_records(const nxt_port_mmap_msg_t *records,
    size_t nrecords, size_t tail)
{
    return nxt_unit_msg_test_send_records_to(nxt_unit_msg_test_ctx, records,
                                             nrecords, tail);
}


/* The child cases exit with the return code + 10, to tell it from 1, 2. */

#define NXT_UNIT_MSG_TEST_RC(rc)  ((rc) + 10)


/* Runs "fn" in a child: asserts that it exits with RC(expect). */
static void
nxt_unit_msg_test_in_child(const char *name, int (*fn)(void *), void *data,
    int expect)
{
    int    status;
    pid_t  child;

    status = -1;
    fflush(stdout);

    child = fork();

    if (child == 0) {
        _exit(fn(data) & 0xFF);
    }

    if (child != -1 && waitpid(child, &status, 0) == child
        && WIFSIGNALED(status))
    {
        printf("unit msg test: child killed by signal %d\n", WTERMSIG(status));
    }

    nxt_unit_msg_test_assert(child != -1 && WIFEXITED(status)
                             && WEXITSTATUS(status)
                                == NXT_UNIT_MSG_TEST_RC(expect), name);
}


static int
nxt_unit_msg_test_records_case(void *data)
{
    return NXT_UNIT_MSG_TEST_RC(nxt_unit_msg_test_send_records(data, 1, 0));
}


typedef struct {
    const char  *name;
    size_t      size;
    uint32_t    id;
    int         expect;
} nxt_unit_msg_test_segment_t;


static const nxt_unit_msg_test_segment_t  segments[] = {
    { "segment id 0xFFFFFFFF is refused", PORT_MMAP_SIZE, 0xFFFFFFFF,
      NXT_UNIT_ERROR },
    { "segment id 1000 is accepted", PORT_MMAP_SIZE, 1000, NXT_UNIT_OK },
    { "segment shorter than PORT_MMAP_SIZE is refused",
      PORT_MMAP_HEADER_SIZE, 1, NXT_UNIT_ERROR },
    { "segment longer than PORT_MMAP_SIZE is refused",
      2 * PORT_MMAP_SIZE, 1, NXT_UNIT_ERROR },
};


static const struct {
    const char           *name;
    nxt_port_mmap_msg_t  rec;
} bad_records[] = {
    { "mmap_id 0xFFFFFFFF is refused", { 0xFFFFFFFF, 0, 100 } },
    { "chunk_id past the data area is refused",
      { 0, PORT_MMAP_CHUNK_COUNT, 100 } },
    { "size past the data area is refused",
      { 0, PORT_MMAP_CHUNK_COUNT - 1, PORT_MMAP_CHUNK_SIZE + 1 } },
};


static int
nxt_unit_msg_test_segment_case(void *data)
{
    const nxt_unit_msg_test_segment_t  *seg = data;

    return NXT_UNIT_MSG_TEST_RC(nxt_unit_msg_test_send_segment(seg->size,
                                                               seg->id));
}


static void
nxt_unit_msg_test_quit(nxt_unit_ctx_t *ctx)
{
    nxt_unit_msg_test_quit_called = 1;
}


/*
 * A record for an unknown segment parks the read buffer and asks the router
 * for the segment.  When that cannot be sent, the buffer used to be freed
 * still counted in wait_items, so a graceful quit never happened.
 */
static int
nxt_unit_msg_test_get_mmap_fail_case(void *data)
{
    int                  rc;
    u_char               buf[sizeof(nxt_port_msg_t) + 1];
    nxt_port_msg_t       msg;
    nxt_port_mmap_msg_t  rec;

    rec.mmap_id = 5;
    rec.chunk_id = 0;
    rec.size = 100;

    nxt_unit_msg_test_send_fails = 1;
    rc = nxt_unit_msg_test_send_records(&rec, 1, 0);
    nxt_unit_msg_test_send_fails = 0;

    if (rc != NXT_UNIT_ERROR) {
        return 1;
    }

    memset(&msg, 0, sizeof(msg));

    msg.pid = getpid();
    msg.type = _NXT_PORT_MSG_QUIT;
    msg.last = 1;

    memcpy(buf, &msg, sizeof(msg));
    buf[sizeof(msg)] = NXT_PORT_QUIT_GRACEFUL;

    (void) nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, buf, sizeof(buf),
                                     -1);

    return nxt_unit_msg_test_quit_called ? NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK)
                                         : 2;
}


/*
 * Two contexts park on the same unknown segment; only the first asks the
 * router for it.  When that failed, the second buffer stayed parked with
 * its wait counted, so the second context never quit gracefully.
 */
static int
nxt_unit_msg_test_get_mmap_follower_case(void *data)
{
    int                  rc;
    u_char               buf[sizeof(nxt_port_msg_t) + 1];
    nxt_port_msg_t       msg;
    nxt_unit_ctx_t       *ctx2;
    nxt_port_mmap_msg_t  rec;

    ctx2 = nxt_unit_ctx_alloc(nxt_unit_msg_test_ctx, NULL);
    if (ctx2 == NULL) {
        return 1;
    }

    rec.mmap_id = 6;
    rec.chunk_id = 0;
    rec.size = 100;

    nxt_unit_msg_test_follower = ctx2;
    nxt_unit_msg_test_send_fails = 1;

    rc = nxt_unit_msg_test_send_records(&rec, 1, 0);

    if (rc != NXT_UNIT_ERROR || nxt_unit_msg_test_follower != NULL) {
        return 2;
    }

    memset(&msg, 0, sizeof(msg));

    msg.pid = getpid();
    msg.type = _NXT_PORT_MSG_QUIT;
    msg.last = 1;

    memcpy(buf, &msg, sizeof(msg));
    buf[sizeof(msg)] = NXT_PORT_QUIT_GRACEFUL;

    /* The follower reads its record again, asks, fails, and quits. */

    (void) nxt_unit_test_process_msg(ctx2, buf, sizeof(buf), -1);

    return nxt_unit_msg_test_quit_called ? NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK)
                                         : 3;
}


static int
nxt_unit_msg_test_count_maps(void)
{
    int   n;
    char  line[512];
    FILE  *f;

    f = fopen("/proc/self/maps", "r");
    if (f == NULL) {
        return -1;
    }

    n = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        n += (strstr(line, "unit.msg.test") != NULL);
    }

    fclose(f);

    return n;
}


/* A duplicate segment id must not leak a mapping; the first one stays. */
static int
nxt_unit_msg_test_dup_id_case(void *data)
{
    int                  i, before, after;
    nxt_port_mmap_msg_t  rec;

    before = nxt_unit_msg_test_count_maps();

    for (i = 0; i < 3; i++) {
        if (nxt_unit_msg_test_send_segment(PORT_MMAP_SIZE, 0) != NXT_UNIT_OK) {
            return 1;
        }
    }

    after = nxt_unit_msg_test_count_maps();

    if (before < 0 || after != before) {
        printf("unit msg test: %d segment mappings, was %d\n", after, before);
        return 2;
    }

    rec.mmap_id = 0;
    rec.chunk_id = 0;
    rec.size = 100;

    return NXT_UNIT_MSG_TEST_RC(nxt_unit_msg_test_send_records(&rec, 1, 0));
}


int
main(void)
{
    int                  rc, ready[2], router[2], read[2], shared[2];
    char                 name[64];
    size_t               tail, i;
    nxt_unit_init_t      init;
    nxt_port_mmap_msg_t  rec[2];

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, ready) == -1
        || socketpair(AF_UNIX, SOCK_DGRAM, 0, router) == -1
        || socketpair(AF_UNIX, SOCK_DGRAM, 0, read) == -1
        || socketpair(AF_UNIX, SOCK_DGRAM, 0, shared) == -1)
    {
        perror("socketpair");
        return 1;
    }

    memset(&init, 0, sizeof(init));

    init.callbacks.request_handler = nxt_unit_msg_test_handler;
    init.callbacks.port_send = nxt_unit_msg_test_send;
    init.callbacks.port_recv = nxt_unit_msg_test_recv;
    init.callbacks.quit = nxt_unit_msg_test_quit;

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
    init.shared_queue_fd = nxt_unit_msg_test_shm(sizeof(nxt_app_queue_t));
    init.log_fd = STDERR_FILENO;

    if (init.shared_queue_fd == -1) {
        return 1;
    }

    nxt_unit_msg_test_ctx = nxt_unit_init(&init);
    if (nxt_unit_msg_test_ctx == NULL) {
        printf("unit msg test: nxt_unit_init() failed\n");
        return 1;
    }

    /* Segment 0, the one every well-formed record below refers to. */

    rc = nxt_unit_msg_test_send_segment(PORT_MMAP_SIZE, 0);

    nxt_unit_msg_test_assert(rc == NXT_UNIT_OK, "segment 0 is accepted");

    if (rc != NXT_UNIT_OK) {
        return 1;
    }

    memset(rec, 0, sizeof(rec));

    rec[0].size = 100;
    rec[1].chunk_id = 1;
    rec[1].size = PORT_MMAP_CHUNK_SIZE;

    nxt_unit_msg_test_assert(
        nxt_unit_msg_test_send_records(rec, 1, 0) == NXT_UNIT_OK,
        "one whole record is accepted");

    nxt_unit_msg_test_assert(
        nxt_unit_msg_test_send_records(rec, 2, 0) == NXT_UNIT_OK,
        "two whole records are accepted");

    for (tail = 1; tail < sizeof(nxt_port_mmap_msg_t); tail++) {
        snprintf(name, sizeof(name), "record + %d-byte partial tail refused",
                 (int) tail);

        nxt_unit_msg_test_assert(
            nxt_unit_msg_test_send_records(rec, 1, tail) == NXT_UNIT_ERROR,
            name);
    }

    nxt_unit_msg_test_assert(
        nxt_unit_msg_test_send_records(rec, 0, 5) == NXT_UNIT_ERROR,
        "a lone partial record is refused");

    for (i = 0; i < nxt_nitems(bad_records); i++) {
        nxt_unit_msg_test_in_child(bad_records[i].name,
                                   nxt_unit_msg_test_records_case,
                                   (void *) &bad_records[i].rec,
                                   NXT_UNIT_ERROR);
    }

    /* Not capped: the router's outgoing segments are unbounded (#172). */

    for (i = 0; i < nxt_nitems(segments); i++) {
        nxt_unit_msg_test_in_child(segments[i].name,
                                   nxt_unit_msg_test_segment_case,
                                   (void *) &segments[i], segments[i].expect);
    }

    nxt_unit_msg_test_in_child("failed get_mmap does not block a graceful "
                               "quit", nxt_unit_msg_test_get_mmap_fail_case,
                               NULL, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("failed get_mmap unparks the other contexts",
                               nxt_unit_msg_test_get_mmap_follower_case,
                               NULL, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("duplicate segment id does not leak a mapping",
                               nxt_unit_msg_test_dup_id_case, NULL,
                               NXT_UNIT_OK);

    if (nxt_unit_msg_test_failures != 0) {
        printf("unit msg test: %d failure(s)\n", nxt_unit_msg_test_failures);
        return 1;
    }

    printf("unit msg test: all passed\n");

    return 0;
}
