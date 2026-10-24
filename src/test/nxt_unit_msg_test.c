
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Malformed port messages at libunit's receive side (src/nxt_unit.c), fed
 * through nxt_unit_test_process_msg().  nxt_unit_mmap_read() used to read a
 * partial last record past the message, mmap_id 0xFFFFFFFF wrapped the
 * lib->incoming index, a segment id like 100000000 grew lib->incoming to
 * that many slots, and a segment of any size was accepted.  Cases that
 * could crash the old code run in a child; a signal is a failure.
 *
 * The request cases put an nxt_unit_request_t into segment 0 the way the
 * router does, and keep the test's own mapping of the segment: the segment
 * is shared by every process of an application, so the test also plays a
 * sibling process that writes the request after libunit checked it.
 */

#include "nxt_main.h"
#include "nxt_port_memory_int.h"
#include "nxt_port_queue.h"
#include "nxt_app_queue.h"
#include "nxt_unit.h"
#include "nxt_unit_request.h"
#include "nxt_unit_response.h"
#include "nxt_unit_websocket.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#if (NXT_MACOSX)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif


/* What the request handler does with a request that arrived. */
typedef enum {
    NXT_UNIT_MSG_TEST_READ_BODY = 0,
    NXT_UNIT_MSG_TEST_READ_SPLIT_BODY,
    NXT_UNIT_MSG_TEST_READ_SPLIT_BODY_EBADF,
    NXT_UNIT_MSG_TEST_GROUP_DUP,
    NXT_UNIT_MSG_TEST_RAISE_COUNT,
    NXT_UNIT_MSG_TEST_UPGRADE,
    NXT_UNIT_MSG_TEST_ORDER,
    NXT_UNIT_MSG_TEST_SMALL,
    NXT_UNIT_MSG_TEST_PLAIN_HELD,
    NXT_UNIT_MSG_TEST_NO_CHUNK,
    NXT_UNIT_MSG_TEST_WRITE_FIRST,
} nxt_unit_msg_test_mode_t;


/* A message that libunit sent to the socket of the engine port. */
typedef struct {
    nxt_port_msg_t  msg;
    uint32_t        size;     /* Of the data, in shared memory or plain. */
} nxt_unit_msg_test_dgram_t;


#define NXT_UNIT_MSG_TEST_ROUTER_PORT  2
#define NXT_UNIT_MSG_TEST_ENGINE_PORT  5
#define NXT_UNIT_MSG_TEST_FIELDS       32
#define NXT_UNIT_MSG_TEST_STREAM       7
#define NXT_UNIT_MSG_TEST_BODY         "ab"
#define NXT_UNIT_MSG_TEST_JSON         "{\"message\":\"No route found\"}"
#define NXT_UNIT_MSG_TEST_HELLO        "hello\n"
#define NXT_UNIT_MSG_TEST_HELD_SIZE    500
#define NXT_UNIT_MSG_TEST_PART_SIZE    100
#define NXT_UNIT_MSG_TEST_WRITE_SIZE   2000
#define NXT_UNIT_RESPONSE_PLAIN_ALLOC  1008


static int                      nxt_unit_msg_test_failures;
static int                      nxt_unit_msg_test_send_fails;
static int                      nxt_unit_msg_test_quit_called;
static int                      nxt_unit_msg_test_handler_calls;
static int                      nxt_unit_msg_test_handler_ok;
static int                      nxt_unit_msg_test_ws_calls;
static int                      nxt_unit_msg_test_ws_ok;
static int                      nxt_unit_msg_test_close_calls;
static int                      nxt_unit_msg_test_get_mmaps;
static int                      nxt_unit_msg_test_rpc_errors;
static int                      nxt_unit_msg_test_get_ports;
static char                     nxt_unit_msg_test_ws_order[8];
static size_t                   nxt_unit_msg_test_ws_norder;
static nxt_unit_msg_test_mode_t nxt_unit_msg_test_mode;
static pid_t                    nxt_unit_msg_test_pid;
static nxt_unit_ctx_t           *nxt_unit_msg_test_ctx;
static nxt_unit_ctx_t           *nxt_unit_msg_test_follower;
static nxt_port_mmap_header_t   *nxt_unit_msg_test_seg0;
static uint16_t                 nxt_unit_msg_test_reply_port
                                    = NXT_UNIT_MSG_TEST_ROUTER_PORT;
static uint32_t                 nxt_unit_msg_test_stream
                                    = NXT_UNIT_MSG_TEST_STREAM;

/*
 * What libunit sent to the socket of the engine port, in order.  The mutex
 * is for nxt_unit_msg_test_router_thread().
 */
/* The router and process "Z" for nxt_unit_msg_test_own_item_case(). */
typedef struct {
    void               *queue;
    int                stage;
    /* Lap the queue counters instead of stalling at Z's marker. */
    int                lap;
    nxt_nncq_atomic_t  head;
} nxt_unit_msg_test_stall_t;


static nxt_unit_msg_test_dgram_t  nxt_unit_msg_test_dgrams[16];
static size_t                     nxt_unit_msg_test_ndgrams;
static pthread_mutex_t            nxt_unit_msg_test_dgrams_mutex
                                      = PTHREAD_MUTEX_INITIALIZER;

static int nxt_unit_msg_test_respond(nxt_unit_request_info_t *req);
static int nxt_unit_msg_test_respond_small(nxt_unit_request_info_t *req);
static int nxt_unit_msg_test_respond_held(nxt_unit_request_info_t *req);
static int nxt_unit_msg_test_respond_no_chunk(nxt_unit_request_info_t *req);
static int nxt_unit_msg_test_respond_write_first(
    nxt_unit_request_info_t *req);

static void nxt_unit_msg_test_stall_step(const nxt_port_msg_t *msg);

/* grok-713 harness: a port_send callback that blocks like the Go adapter. */
int nxt_unit_port_send_flags(nxt_unit_ctx_t *ctx) __attribute__((weak));
static int  nxt_unit_msg_test_full_fd = -1;
static int  nxt_unit_msg_test_wake_cb;
static int  nxt_unit_msg_test_wake_dropped;
static int  nxt_unit_msg_test_quit_dropped;
static int  nxt_unit_msg_test_read_fds[2] = { -1, -1 };
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
    char                buf[16];
    ssize_t             n;
    nxt_unit_field_t    *f;
    nxt_unit_request_t  *r;

    nxt_unit_msg_test_handler_calls++;

    r = req->request;

    switch (nxt_unit_msg_test_mode) {

    case NXT_UNIT_MSG_TEST_READ_BODY:
        n = nxt_unit_request_read(req, buf, sizeof(buf));

        nxt_unit_msg_test_handler_ok =
            n == (ssize_t) nxt_length(NXT_UNIT_MSG_TEST_BODY)
            && memcmp(buf, NXT_UNIT_MSG_TEST_BODY, n) == 0;

        break;

    case NXT_UNIT_MSG_TEST_READ_SPLIT_BODY:
        memset(buf, 0x5A, sizeof(buf));

        n = nxt_unit_request_read(req, buf, 4);

        nxt_unit_msg_test_handler_ok =
            n == 4
            && memcmp(buf, "abcd", 4) == 0
            && (u_char) buf[4] == 0x5A;

        break;

    case NXT_UNIT_MSG_TEST_READ_SPLIT_BODY_EBADF:
        memset(buf, 0x5A, sizeof(buf));

        n = nxt_unit_request_read(req, buf, 4);

        nxt_unit_msg_test_handler_ok =
            n == 2
            && memcmp(buf, "ab", 2) == 0
            && (u_char) buf[2] == 0x5A;

        break;

    case NXT_UNIT_MSG_TEST_GROUP_DUP:
        nxt_unit_request_group_dup_fields(req);

        /* "X-Dup" and "x-dup" are now neighbours, with one name. */
        f = r->fields;

        nxt_unit_msg_test_handler_ok =
            r->fields_count == 3
            && f[1].name_length == nxt_length("X-Dup")
            && nxt_unit_sptr_get(&f[1].name) == nxt_unit_sptr_get(&f[2].name)
            && memcmp(nxt_unit_sptr_get(&f[1].value), "1", 1) == 0
            && memcmp(nxt_unit_sptr_get(&f[2].value), "2", 1) == 0
            && f[0].name_length == nxt_length("Host")
            && memcmp(nxt_unit_sptr_get(&f[0].value), "localhost", 9) == 0;

        break;

    case NXT_UNIT_MSG_TEST_RAISE_COUNT:
        /*
         * A sibling process raises the count after libunit checked it.
         * The request sits in the last chunk of the segment, so a walk
         * over that many fields leaves the mapping.
         */
        r->fields_count = 0x00FFFFFF;

        nxt_unit_request_group_dup_fields(req);

        nxt_unit_msg_test_handler_ok = 1;

        break;

    case NXT_UNIT_MSG_TEST_UPGRADE:
        nxt_unit_msg_test_handler_ok =
            nxt_unit_response_init(req, 101, 0, 0) == NXT_UNIT_OK
            && nxt_unit_response_upgrade(req) == NXT_UNIT_OK
            && nxt_unit_response_send(req) == NXT_UNIT_OK;

        /* The request now waits for frames; the close finishes it. */
        return;

    case NXT_UNIT_MSG_TEST_ORDER:
        nxt_unit_msg_test_handler_ok =
            nxt_unit_msg_test_respond(req) == NXT_UNIT_OK;

        nxt_unit_request_done(req, NXT_UNIT_OK);

        return;

    case NXT_UNIT_MSG_TEST_SMALL:
        nxt_unit_msg_test_handler_ok =
            nxt_unit_msg_test_respond_small(req) == NXT_UNIT_OK;

        nxt_unit_request_done(req, NXT_UNIT_OK);

        return;

    case NXT_UNIT_MSG_TEST_PLAIN_HELD:
        nxt_unit_msg_test_handler_ok =
            nxt_unit_msg_test_respond_held(req) == NXT_UNIT_OK;

        nxt_unit_request_done(req, NXT_UNIT_OK);

        return;

    case NXT_UNIT_MSG_TEST_NO_CHUNK:
        nxt_unit_msg_test_handler_ok =
            nxt_unit_msg_test_respond_no_chunk(req) == NXT_UNIT_OK;

        nxt_unit_request_done(req, NXT_UNIT_OK);

        return;

    case NXT_UNIT_MSG_TEST_WRITE_FIRST:
        nxt_unit_msg_test_handler_ok =
            nxt_unit_msg_test_respond_write_first(req) == NXT_UNIT_OK;

        nxt_unit_request_done(req, NXT_UNIT_OK);

        return;
    }

    nxt_unit_request_done(req, NXT_UNIT_ERROR);
}


static nxt_unit_msg_test_stall_t  nxt_unit_msg_test_stall;


static void
nxt_unit_msg_test_websocket(nxt_unit_websocket_frame_t *ws)
{
    char     buf[16];
    ssize_t  n;

    nxt_unit_msg_test_ws_calls++;

    n = nxt_unit_websocket_read(ws, buf, sizeof(buf));

    nxt_unit_msg_test_ws_ok = ws->payload_len == 3 && n == 3
                              && memcmp(buf, "abc", 3) == 0;

    /* The first payload byte of each frame, in the order of the calls. */
    if (n > 0 && nxt_unit_msg_test_ws_norder
                 < sizeof(nxt_unit_msg_test_ws_order) - 1)
    {
        nxt_unit_msg_test_ws_order[nxt_unit_msg_test_ws_norder++] = buf[0];
    }

    nxt_unit_websocket_done(ws);
}


static void
nxt_unit_msg_test_close(nxt_unit_request_info_t *req)
{
    nxt_unit_msg_test_close_calls++;

    nxt_unit_request_done(req, NXT_UNIT_ERROR);
}


static ssize_t
nxt_unit_msg_test_send(nxt_unit_ctx_t *ctx, nxt_unit_port_t *port,
    const void *buf, size_t buf_size, const void *oob, size_t oob_size)
{
    const nxt_port_msg_t       *msg;
    nxt_port_mmap_msg_t        rec;
    nxt_unit_msg_test_dgram_t  *d;

    msg = buf;

    if (nxt_unit_msg_test_full_fd != -1 && buf_size >= sizeof(nxt_port_msg_t)
        && (msg->type == _NXT_PORT_MSG_RPC_READY
            || msg->type == _NXT_PORT_MSG_READ_QUEUE
            || msg->type == _NXT_PORT_MSG_QUIT))
    {
        int      fl;
        ssize_t  n;

        fl = (nxt_unit_port_send_flags != NULL)
             ? nxt_unit_port_send_flags(ctx) : 0;

        nxt_unit_msg_test_wake_cb++;

        /* Like go/port.go: a send over the peer's full socket. */
        n = send(nxt_unit_msg_test_full_fd, buf, buf_size, fl);

        if (n == -1 && (fl & MSG_DONTWAIT) != 0 && errno == EAGAIN) {
            nxt_unit_msg_test_wake_dropped++;

            if (msg->type == _NXT_PORT_MSG_QUIT) {
                nxt_unit_msg_test_quit_dropped++;
            }

            return 0;
        }

        return n;
    }

    pthread_mutex_lock(&nxt_unit_msg_test_dgrams_mutex);

    /*
     * What the router reads as the end of a failed request.  "last" makes
     * nxt_port_rpc_handler() delete the registration; without it the
     * registration and its port reference stay.
     */
    if (buf_size >= sizeof(nxt_port_msg_t)
        && msg->type == _NXT_PORT_MSG_RPC_ERROR
        && msg->stream == NXT_UNIT_MSG_TEST_STREAM
        && msg->last == 1
        && msg->pid == nxt_unit_msg_test_pid)
    {
        nxt_unit_msg_test_rpc_errors++;
    }

    if (buf_size >= sizeof(nxt_port_msg_t)
        && msg->type == _NXT_PORT_MSG_GET_PORT)
    {
        nxt_unit_msg_test_get_ports++;
    }

    if (port->id.id == NXT_UNIT_MSG_TEST_ENGINE_PORT
        && buf_size >= sizeof(nxt_port_msg_t)
        && msg->type != _NXT_PORT_MSG_READ_QUEUE
        && nxt_unit_msg_test_ndgrams < nxt_nitems(nxt_unit_msg_test_dgrams))
    {
        d = &nxt_unit_msg_test_dgrams[nxt_unit_msg_test_ndgrams++];

        memcpy(&d->msg, msg, sizeof(nxt_port_msg_t));

        if (msg->mmap
            && buf_size >= sizeof(nxt_port_msg_t) + sizeof(nxt_port_mmap_msg_t))
        {
            memcpy(&rec, msg + 1, sizeof(rec));
            d->size = rec.size;

        } else {
            d->size = buf_size - sizeof(nxt_port_msg_t);
        }
    }

    pthread_mutex_unlock(&nxt_unit_msg_test_dgrams_mutex);

    if (nxt_unit_msg_test_stall.queue != NULL
        && port->id.id == NXT_UNIT_MSG_TEST_ENGINE_PORT
        && buf_size >= sizeof(nxt_port_msg_t))
    {
        nxt_unit_msg_test_stall_step(msg);
    }

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

    if (msg->type == _NXT_PORT_MSG_GET_MMAP) {
        nxt_unit_msg_test_get_mmaps++;
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

    /* The test keeps segment 0 mapped: the request cases write into it. */
    if (id == 0 && nxt_unit_msg_test_seg0 == NULL && size == PORT_MMAP_SIZE) {
        nxt_unit_msg_test_seg0 = hdr;

    } else {
        munmap(hdr, size);
    }

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
    { "segment id NXT_PORT_MMAPS_MAX - 1 is accepted", PORT_MMAP_SIZE,
      NXT_PORT_MMAPS_MAX - 1, NXT_UNIT_OK },
    { "segment id NXT_PORT_MMAPS_MAX is refused", PORT_MMAP_SIZE,
      NXT_PORT_MMAPS_MAX, NXT_UNIT_ERROR },
    { "segment id 100000000 is refused", PORT_MMAP_SIZE, 100000000,
      NXT_UNIT_ERROR },
    { "segment shorter than PORT_MMAP_SIZE is refused",
      PORT_MMAP_HEADER_SIZE, 1, NXT_UNIT_ERROR },
    /*
     * A whole 64 KiB page short: macOS rounds a shm object up to a page,
     * so a one-byte gap would vanish there.  10424320 rounds up to at most
     * 10485760 for any page up to 64 KiB, still below PORT_MMAP_SIZE.
     */
    { "segment 64 KiB shorter than PORT_MMAP_SIZE is refused",
      PORT_MMAP_SIZE - 65536, 1, NXT_UNIT_ERROR },
    { "segment rounded up to a 16 KiB page is accepted",
      (PORT_MMAP_SIZE + 16383) & ~(size_t) 16383, 1, NXT_UNIT_OK },
    { "segment longer than PORT_MMAP_SIZE is accepted",
      2 * PORT_MMAP_SIZE, 1, NXT_UNIT_OK },
};


static const struct {
    const char           *name;
    nxt_port_mmap_msg_t  rec;
} bad_records[] = {
    { "mmap_id 0xFFFFFFFF is refused", { 0xFFFFFFFF, 0, 100 } },
    { "mmap_id NXT_PORT_MMAPS_MAX is refused", { NXT_PORT_MMAPS_MAX, 0, 100 } },
    { "mmap_id 100000000 is refused", { 100000000, 0, 100 } },
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


/*
 * Records for segments 3, 9, 30 and 100 grow lib->incoming four times, and
 * a read buffer parks on each segment between the growths.  The array was
 * grown with realloc(), and the lists of the segments still pointed into
 * the old array.  A later read buffer for such a segment, when it was not
 * mapped yet, was written into freed memory.
 */
static int
nxt_unit_msg_test_mmaps_grow_case(void *data)
{
    u_char               buf[sizeof(nxt_port_msg_t)
                             + sizeof(nxt_port_mmap_msg_t)];
    size_t               i, parked;
    nxt_port_msg_t       msg;
    nxt_port_mmap_msg_t  rec;

    static const uint32_t  ids[] = { 3, 9, 30, 100 };

    memset(&msg, 0, sizeof(msg));

    msg.stream = 7;
    msg.pid = getpid();
    msg.type = _NXT_PORT_MSG_RPC_READY;
    msg.last = 1;
    msg.mmap = 1;

    rec.chunk_id = 0;
    rec.size = 100;

    for (i = 0; i < nxt_nitems(ids); i++) {
        rec.mmap_id = ids[i];

        memcpy(buf, &msg, sizeof(msg));
        memcpy(buf + sizeof(msg), &rec, sizeof(rec));

        /* At once, not behind the buffer parked before. */
        (void) nxt_unit_test_process_shared_msg(nxt_unit_msg_test_ctx, buf,
                                                sizeof(buf), -1);
    }

    parked = nxt_unit_test_parked_rbufs(nxt_unit_msg_test_ctx);

    if (parked != nxt_nitems(ids)) {
        printf("unit msg test: after the growths: %ld parked\n",
               (long) parked);

        /* The child ends with _exit(), which does not flush stdout. */
        fflush(stdout);

        return 2;
    }

    for (i = 0; i < nxt_nitems(ids); i++) {
        if (nxt_unit_msg_test_send_segment(PORT_MMAP_SIZE, ids[i])
            != NXT_UNIT_OK)
        {
            return 3;
        }
    }

    if (nxt_unit_test_parked_rbufs(nxt_unit_msg_test_ctx) != 0) {
        return 3;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * A context is freed with a read buffer parked on segment 9, and with a
 * message in pending_rbuf that brought a descriptor.  The parked buffer
 * stayed on the list of the segment, and when the segment came,
 * nxt_unit_unpark_rbufs() used the freed context.  The pending buffer and
 * its descriptor leaked.
 */
static int
nxt_unit_msg_test_ctx_free_case(void *data)
{
    int                  rc, fd, fds[2];
    u_char               buf[sizeof(nxt_port_msg_t)
                             + sizeof(nxt_port_mmap_msg_t)];
    size_t               parked;
    nxt_bool_t           is_open;
    nxt_unit_ctx_t       *ctx2;
    nxt_port_msg_t       msg;
    nxt_port_mmap_msg_t  rec;

    ctx2 = nxt_unit_ctx_alloc(nxt_unit_msg_test_ctx, NULL);
    if (ctx2 == NULL || pipe(fds) == -1) {
        return 1;
    }

    close(fds[1]);
    fd = fds[0];

    rec.mmap_id = 9;
    rec.chunk_id = 0;
    rec.size = 100;

    (void) nxt_unit_msg_test_send_records_to(ctx2, &rec, 1, 0);

    /* A record in segment 0 waits behind the parked one, with "fd". */

    memset(&msg, 0, sizeof(msg));

    msg.stream = 7;
    msg.pid = getpid();
    msg.type = _NXT_PORT_MSG_RPC_READY;
    msg.last = 1;
    msg.mmap = 1;

    rec.mmap_id = 0;

    memcpy(buf, &msg, sizeof(msg));
    memcpy(buf + sizeof(msg), &rec, sizeof(rec));

    (void) nxt_unit_test_process_msg(ctx2, buf, sizeof(buf), fd);

    if (nxt_unit_test_parked_rbufs(nxt_unit_msg_test_ctx) != 1
        || fcntl(fd, F_GETFD) == -1)
    {
        return 1;
    }

    nxt_unit_done(ctx2);

    parked = nxt_unit_test_parked_rbufs(nxt_unit_msg_test_ctx);
    is_open = (fcntl(fd, F_GETFD) != -1);

    if (parked != 0 || is_open) {
        printf("unit msg test: after the context was freed: %d parked, "
               "fd %s\n", (int) parked, is_open ? "open" : "closed");

        /* The child ends with _exit(), which does not flush stdout. */
        fflush(stdout);

        return 2;
    }

    rc = nxt_unit_msg_test_send_segment(PORT_MMAP_SIZE, 9);

    return (rc == NXT_UNIT_OK) ? NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK) : 3;
}


/*
 * The context that asked for segment 10 is freed, and the main context
 * waits behind it for the same segment.  The answer would go to the port
 * of the freed context, so the buffer of the main context goes back to it.
 * The main context reads its record again, asks for the segment itself,
 * and gets it when it comes.
 */
static int
nxt_unit_msg_test_ctx_free_asker_case(void *data)
{
    int                  rc;
    size_t               parked;
    nxt_unit_ctx_t       *ctx2;
    nxt_port_msg_t       msg;
    nxt_port_mmap_msg_t  rec;

    ctx2 = nxt_unit_ctx_alloc(nxt_unit_msg_test_ctx, NULL);
    if (ctx2 == NULL) {
        return 1;
    }

    rec.mmap_id = 10;
    rec.chunk_id = 0;
    rec.size = 100;

    nxt_unit_msg_test_follower = nxt_unit_msg_test_ctx;
    nxt_unit_msg_test_get_mmaps = 0;

    (void) nxt_unit_msg_test_send_records_to(ctx2, &rec, 1, 0);

    if (nxt_unit_msg_test_follower != NULL
        || nxt_unit_msg_test_get_mmaps != 1
        || nxt_unit_test_parked_rbufs(nxt_unit_msg_test_ctx) != 2)
    {
        return 1;
    }

    /*
     * The read port of the main context is full, as when it stopped reading:
     * the wake-up from nxt_unit_done() must not block on it.
     */
    memset(&msg, 0, sizeof(msg));

    while (send(nxt_unit_msg_test_read_fds[1], &msg, sizeof(msg),
                MSG_DONTWAIT) > 0)
    {
        /* fill */
    }

    nxt_unit_msg_test_full_fd = nxt_unit_msg_test_read_fds[1];

    alarm(10);

    nxt_unit_done(ctx2);

    alarm(0);

    nxt_unit_msg_test_full_fd = -1;

    /* The socket is shared with the parent: drain it. */
    while (recv(nxt_unit_msg_test_read_fds[0], &msg, sizeof(msg),
                MSG_DONTWAIT) > 0)
    {
        /* drain */
    }

    parked = nxt_unit_test_parked_rbufs(nxt_unit_msg_test_ctx);

    if (parked != 0) {
        printf("unit msg test: after the asking context was freed: "
               "%d parked\n", (int) parked);

        /* The child ends with _exit(), which does not flush stdout. */
        fflush(stdout);

        return 2;
    }

    /* Any message makes the main context read its pending messages. */

    memset(&msg, 0, sizeof(msg));

    msg.pid = getpid();
    msg.type = _NXT_PORT_MSG_RPC_READY;
    msg.last = 1;

    (void) nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, &msg, sizeof(msg),
                                     -1);

    parked = nxt_unit_test_parked_rbufs(nxt_unit_msg_test_ctx);

    if (parked != 1 || nxt_unit_msg_test_get_mmaps != 2) {
        printf("unit msg test: after the main context read again: "
               "%d parked, %d GET_MMAP\n", (int) parked,
               nxt_unit_msg_test_get_mmaps);

        /* The child ends with _exit(), which does not flush stdout. */
        fflush(stdout);

        return 3;
    }

    rc = nxt_unit_msg_test_send_segment(PORT_MMAP_SIZE, 10);

    if (rc != NXT_UNIT_OK
        || nxt_unit_test_parked_rbufs(nxt_unit_msg_test_ctx) != 0)
    {
        return 4;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * grok-713: a buffer of ctx2 parks on segment 12; the main context gets the
 * segment and wakes ctx2.  The callback's socket is full, as a Go peer's is
 * when it stopped reading.  The wake-up must not block.
 */
static int
nxt_unit_msg_test_full_wake_case(void *data)
{
    int                  sp[2];
    char                 junk[64];
    nxt_unit_ctx_t       *ctx2;
    nxt_port_mmap_msg_t  rec;

    ctx2 = nxt_unit_ctx_alloc(nxt_unit_msg_test_ctx, NULL);
    if (ctx2 == NULL || socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) == -1) {
        return 1;
    }

    memset(junk, 0, sizeof(junk));

    while (send(sp[0], junk, sizeof(junk), MSG_DONTWAIT) > 0) {
        /* fill */
    }

    rec.mmap_id = 12;
    rec.chunk_id = 0;
    rec.size = 100;

    (void) nxt_unit_msg_test_send_records_to(ctx2, &rec, 1, 0);

    nxt_unit_msg_test_full_fd = sp[0];

    alarm(10);

    (void) nxt_unit_msg_test_send_segment(PORT_MMAP_SIZE, 12);

    alarm(0);

    nxt_unit_msg_test_full_fd = -1;

    printf("grok713: wake via callback %d, dropped %d\n",
           nxt_unit_msg_test_wake_cb, nxt_unit_msg_test_wake_dropped);
    fflush(stdout);

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * The main context gets a QUIT and broadcasts it while the socket of ctx2 is
 * full.  The broadcast holds lib->mutex and must not block.  A QUIT whose
 * datagram is dropped stays pending, and ctx2 quits on its next message.
 */
static int
nxt_unit_msg_test_quit_full_case(void *data)
{
    int             sp[2];
    char            junk[64];
    nxt_unit_ctx_t  *ctx2;

    struct {
        nxt_port_msg_t  msg;
        uint8_t         quit_param;
    } nxt_packed m;

    ctx2 = nxt_unit_ctx_alloc(nxt_unit_msg_test_ctx, NULL);
    if (ctx2 == NULL || socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) == -1) {
        return 1;
    }

    memset(junk, 0, sizeof(junk));

    while (send(sp[0], junk, sizeof(junk), MSG_DONTWAIT) > 0) {
        /* fill */
    }

    memset(&m, 0, sizeof(m));

    m.msg.pid = getpid();
    m.msg.type = _NXT_PORT_MSG_QUIT;
    m.quit_param = 0;  /* NXT_PORT_QUIT_NORMAL */

    nxt_unit_msg_test_full_fd = sp[0];

    alarm(10);

    (void) nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, &m, sizeof(m), -1);

    alarm(0);

    nxt_unit_msg_test_full_fd = -1;

    /*
     * ctx2 has a port queue, so the QUIT itself went into the queue and only
     * its READ_QUEUE nudge met the full socket.  A port without a queue, or
     * a full queue, leaves the QUIT pending instead; the test sets that
     * state directly and checks that ctx2 quits on its next message.
     */
    if (nxt_unit_msg_test_quit_dropped == 0) {
        nxt_unit_test_ctx_set_quit_pending(ctx2, 0);
    }

    if (!nxt_unit_test_ctx_online(ctx2)) {
        return 2;
    }

    m.msg.type = _NXT_PORT_MSG_RPC_READY;

    (void) nxt_unit_test_process_msg(ctx2, &m, sizeof(m.msg), -1);

    printf("grok713: quit dropped %d, ctx2 online after next message %d\n",
           nxt_unit_msg_test_quit_dropped,
           (int) nxt_unit_test_ctx_online(ctx2));
    fflush(stdout);

    return nxt_unit_test_ctx_online(ctx2) ? 3
                                          : NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


static int
nxt_unit_msg_test_count_maps(void)
{
#if (NXT_LINUX)
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

#elif (NXT_MACOSX)
    int                             n;
    kern_return_t                   kr;
    mach_port_t                     object;
    mach_vm_size_t                  size, page;
    mach_vm_address_t               address;
    mach_msg_type_number_t          count;
    vm_region_basic_info_data_64_t  info;

    /*
     * Count the regions of exactly one segment, PORT_MMAP_SIZE rounded up
     * to a page.  The share mode cannot identify them: a segment that is
     * mapped but not touched reports SM_PRIVATE.
     */

    page = getpagesize();
    address = 0;
    n = 0;

    for ( ;; ) {
        count = VM_REGION_BASIC_INFO_COUNT_64;

        kr = mach_vm_region(mach_task_self(), &address, &size,
                            VM_REGION_BASIC_INFO_64,
                            (vm_region_info_t) &info, &count, &object);

        if (kr == KERN_INVALID_ADDRESS) {
            break;
        }

        if (kr != KERN_SUCCESS) {
            return -1;
        }

        if (object != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), object);
        }

        if (size >= PORT_MMAP_SIZE && size < PORT_MMAP_SIZE + page) {
            n++;
        }

        if (size == 0 || address + size < address) {
            break;
        }

        address += size;
    }

    return n;

#else
    return -1;
#endif
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

#if !(NXT_LINUX || NXT_MACOSX)
    if (before < 0) {
        /* No mapping count method available on this platform. */
        printf("unit msg test: mapping count skipped, "
               "no mapping count method\n");
        before = 0;
        after = 0;
    }
#endif

    if (before < 0 || after != before) {
        printf("unit msg test: %d segment mappings, was %d\n", after, before);
        return 2;
    }

    rec.mmap_id = 0;
    rec.chunk_id = 0;
    rec.size = 100;

    return NXT_UNIT_MSG_TEST_RC(nxt_unit_msg_test_send_records(&rec, 1, 0));
}


/*
 * A request as the router lays it out (nxt_router_prepare_msg()): the
 * struct, fields[], the strings, then the preread body.  Written at "p",
 * with every sptr set; the size is returned.
 */
static uint32_t
nxt_unit_msg_test_write_request(u_char *p)
{
    u_char              *s;
    nxt_unit_field_t    *f;
    nxt_unit_request_t  *r;

    static const struct {
        const char  *name;
        const char  *value;
    } fields[] = {
        { "Host", "localhost" },
        { "X-Dup", "1" },
        { "x-dup", "2" },
    };

    r = (nxt_unit_request_t *) p;

    memset(r, 0, sizeof(nxt_unit_request_t));

    r->fields_count = nxt_nitems(fields);

    r->content_length_field = NXT_UNIT_NONE_FIELD;
    r->content_type_field = NXT_UNIT_NONE_FIELD;
    r->cookie_field = NXT_UNIT_NONE_FIELD;
    r->authorization_field = NXT_UNIT_NONE_FIELD;

    r->content_length = nxt_length(NXT_UNIT_MSG_TEST_BODY);

    s = (u_char *) &r->fields[r->fields_count];

#define NXT_UNIT_MSG_TEST_STR(sptr, len, str)                                 \
    do {                                                                      \
        nxt_unit_sptr_set(&r->sptr, s);                                       \
        r->len = nxt_length(str);                                             \
        s = nxt_cpymem(s, str, nxt_length(str));                              \
        *s++ = '\0';                                                          \
    } while (0)

    NXT_UNIT_MSG_TEST_STR(method, method_length, "GET");
    NXT_UNIT_MSG_TEST_STR(version, version_length, "HTTP/1.1");
    NXT_UNIT_MSG_TEST_STR(remote, remote_length, "127.0.0.1");
    NXT_UNIT_MSG_TEST_STR(local_addr, local_addr_length, "127.0.0.1");
    NXT_UNIT_MSG_TEST_STR(local_port, local_port_length, "8080");
    NXT_UNIT_MSG_TEST_STR(server_name, server_name_length, "localhost");
    NXT_UNIT_MSG_TEST_STR(target, target_length, "/a?b=c");
    NXT_UNIT_MSG_TEST_STR(path, path_length, "/a");
    NXT_UNIT_MSG_TEST_STR(query, query_length, "b=c");

#undef NXT_UNIT_MSG_TEST_STR

    for (f = r->fields; f < &r->fields[r->fields_count]; f++) {
        f->hash = nxt_unit_field_hash(fields[f - r->fields].name,
                                      strlen(fields[f - r->fields].name));
        f->skip = 0;
        f->hopbyhop = 0;

        nxt_unit_sptr_set(&f->name, s);
        f->name_length = strlen(fields[f - r->fields].name);
        s = nxt_cpymem(s, fields[f - r->fields].name, f->name_length);
        *s++ = '\0';

        nxt_unit_sptr_set(&f->value, s);
        f->value_length = strlen(fields[f - r->fields].value);
        s = nxt_cpymem(s, fields[f - r->fields].value, f->value_length);
        *s++ = '\0';
    }

    nxt_unit_sptr_set(&r->preread_content, s);
    s = nxt_cpymem(s, NXT_UNIT_MSG_TEST_BODY,
                   nxt_length(NXT_UNIT_MSG_TEST_BODY));

    return s - p;
}


/* One message about the request, with one record over "chunk". */
static int
nxt_unit_msg_test_send_chunk(uint8_t type, uint8_t last, nxt_chunk_id_t chunk,
    uint32_t size)
{
    u_char               buf[sizeof(nxt_port_msg_t)
                             + sizeof(nxt_port_mmap_msg_t)];
    nxt_port_msg_t       msg;
    nxt_port_mmap_msg_t  rec;

    memset(&msg, 0, sizeof(msg));

    /* The router port was registered with the pid libunit started with. */
    msg.stream = nxt_unit_msg_test_stream;
    msg.pid = nxt_unit_msg_test_pid;
    msg.reply_port = nxt_unit_msg_test_reply_port;
    msg.type = type;
    msg.last = last;
    msg.mmap = 1;

    rec.mmap_id = 0;
    rec.chunk_id = chunk;
    rec.size = size;

    memcpy(buf, &msg, sizeof(msg));
    memcpy(buf + sizeof(msg), &rec, sizeof(rec));

    return nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, buf, sizeof(buf),
                                     -1);
}


/* Writes a request into "chunk", lets "edit" damage it, and sends it. */
static int
nxt_unit_msg_test_send_request(nxt_chunk_id_t chunk,
    void (*edit)(nxt_unit_request_t *r))
{
    u_char    *p;
    uint32_t  size;

    p = nxt_port_mmap_chunk_start(nxt_unit_msg_test_seg0, chunk);

    size = nxt_unit_msg_test_write_request(p);

    if (edit != NULL) {
        edit((nxt_unit_request_t *) p);
    }

    nxt_unit_msg_test_handler_calls = 0;
    nxt_unit_msg_test_handler_ok = 0;
    nxt_unit_msg_test_rpc_errors = 0;

    return nxt_unit_msg_test_send_chunk(_NXT_PORT_MSG_REQ_HEADERS, 1, chunk,
                                        size);
}


/*
 * A request with a body arrives while a graceful QUIT is pending (the
 * QUIT datagram was dropped on a full socket).  The QUIT used to be taken
 * before the message in hand, so the context went offline and the request
 * from that same buffer was created on it; its body then had no reader and
 * the request hung.  The QUIT must be taken after the message: the context
 * stays online until the body arrives and the request is done, and then
 * quits.  alarm() turns a hang into a failure.
 */

static void
nxt_unit_msg_test_edit_more_body(nxt_unit_request_t *r)
{
    r->content_length = nxt_length(NXT_UNIT_MSG_TEST_BODY "cd");
}


static int
nxt_unit_msg_test_quit_pending_body_case(void *data)
{
    int     rc;
    u_char  *p;

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_READ_BODY;

    alarm(10);

    nxt_unit_test_ctx_set_quit_pending(nxt_unit_msg_test_ctx,
                                       1 /* NXT_QUIT_GRACEFUL */);

    rc = nxt_unit_msg_test_send_request(2, nxt_unit_msg_test_edit_more_body);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 0
        || !nxt_unit_test_ctx_online(nxt_unit_msg_test_ctx))
    {
        printf("unit msg test: after headers rc %d, handler %d, online %d, "
               "expected rc %d, handler 0, online 1\n", rc,
               nxt_unit_msg_test_handler_calls,
               (int) nxt_unit_test_ctx_online(nxt_unit_msg_test_ctx),
               NXT_UNIT_OK);
        fflush(stdout);

        return 1;
    }

    p = nxt_port_mmap_chunk_start(nxt_unit_msg_test_seg0, 3);
    memcpy(p, "cd", 2);

    rc = nxt_unit_msg_test_send_chunk(_NXT_PORT_MSG_REQ_BODY, 1, 3, 2);

    alarm(0);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 1
        || nxt_unit_test_ctx_online(nxt_unit_msg_test_ctx))
    {
        printf("unit msg test: after body rc %d, handler %d, online %d, "
               "expected rc %d, handler 1, online 0\n", rc,
               nxt_unit_msg_test_handler_calls,
               (int) nxt_unit_test_ctx_online(nxt_unit_msg_test_ctx),
               NXT_UNIT_OK);
        fflush(stdout);

        return 2;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * A request whose body starts in preread_content ("ab") and continues in
 * content_fd ("cd").  nxt_unit_request_read() must advance dst and reduce
 * the remaining size after reading from content_buf so that read() appends
 * at dst + buf_res without overwriting the preread bytes or overflowing dst.
 */
static int
nxt_unit_msg_test_split_body_fd_case(void *data)
{
    int             rc, pfd[2];
    nxt_port_msg_t  msg;

    if (pipe(pfd) != 0) {
        return 1;
    }

    if (write(pfd[1], "cdef", 4) != 4) {
        close(pfd[0]);
        close(pfd[1]);
        return 1;
    }

    close(pfd[1]);

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_READ_SPLIT_BODY;

    rc = nxt_unit_msg_test_send_request(2, nxt_unit_msg_test_edit_more_body);
    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 0) {
        close(pfd[0]);
        return 2;
    }

    memset(&msg, 0, sizeof(msg));

    msg.stream = nxt_unit_msg_test_stream;
    msg.pid = nxt_unit_msg_test_pid;
    msg.reply_port = nxt_unit_msg_test_reply_port;
    msg.type = _NXT_PORT_MSG_REQ_BODY;
    msg.last = 1;

    rc = nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, &msg, sizeof(msg),
                                   pfd[0]);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 1
        || !nxt_unit_msg_test_handler_ok)
    {
        printf("unit msg test: split body fd rc %d, handler %d, ok %d\n",
               rc, nxt_unit_msg_test_handler_calls,
               nxt_unit_msg_test_handler_ok);
        fflush(stdout);

        return 3;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * When read() on content_fd fails (e.g. EBADF on a write-only pipe end)
 * after preread bytes ("ab") have already been copied from content_buf,
 * nxt_unit_request_read() must return the 2 preread bytes rather than -1.
 */
static int
nxt_unit_msg_test_split_body_fd_ebadf_case(void *data)
{
    int             rc, pfd[2];
    nxt_port_msg_t  msg;

    if (pipe(pfd) != 0) {
        return 1;
    }

    close(pfd[0]);

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_READ_SPLIT_BODY_EBADF;

    rc = nxt_unit_msg_test_send_request(2, nxt_unit_msg_test_edit_more_body);
    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 0) {
        close(pfd[1]);
        return 2;
    }

    memset(&msg, 0, sizeof(msg));

    msg.stream = nxt_unit_msg_test_stream;
    msg.pid = nxt_unit_msg_test_pid;
    msg.reply_port = nxt_unit_msg_test_reply_port;
    msg.type = _NXT_PORT_MSG_REQ_BODY;
    msg.last = 1;

    rc = nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, &msg, sizeof(msg),
                                   pfd[1]);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 1
        || !nxt_unit_msg_test_handler_ok)
    {
        printf("unit msg test: split body fd ebadf rc %d, handler %d, ok %d\n",
               rc, nxt_unit_msg_test_handler_calls,
               nxt_unit_msg_test_handler_ok);
        fflush(stdout);

        return 3;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


static void
nxt_unit_msg_test_edit_target(nxt_unit_request_t *r)
{
    r->target.offset = 0x7FFFFFF0;
}


static void
nxt_unit_msg_test_edit_preread(nxt_unit_request_t *r)
{
    r->preread_content.offset = 0x7FFFFFF0;
}


static void
nxt_unit_msg_test_edit_count(nxt_unit_request_t *r)
{
    r->fields_count = 0x10000000;
}


static void
nxt_unit_msg_test_edit_index(nxt_unit_request_t *r)
{
    r->cookie_field = r->fields_count;
}


static void
nxt_unit_msg_test_edit_field_name(nxt_unit_request_t *r)
{
    r->fields[1].name.offset = 0x7FFFFFF0;
}


static void
nxt_unit_msg_test_edit_field_inside(nxt_unit_request_t *r)
{
    nxt_unit_sptr_set(&r->fields[1].name, &r->fields[0]);
}


static void
nxt_unit_msg_test_edit_handshake(nxt_unit_request_t *r)
{
    r->websocket_handshake = 1;
}


typedef struct {
    const char                *name;
    void                      (*edit)(nxt_unit_request_t *r);
    nxt_unit_msg_test_mode_t  mode;
    nxt_chunk_id_t            chunk;
    int                       expect;
    int                       handler_calls;
    /*
     * RPC_ERROR messages for the stream.  A refused request gets one, so
     * that the router does not wait for an acknowledgement.  A handler
     * that fails the request sends one through nxt_unit_request_done().
     */
    int                       rpc_errors;
} nxt_unit_msg_test_request_t;


static const nxt_unit_msg_test_request_t  requests[] = {
    { "well-formed request reaches the handler", NULL,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_OK, 1, 1 },
    { "target sptr out of buffer is refused", nxt_unit_msg_test_edit_target,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0, 1 },
    { "preread sptr out of buffer is refused", nxt_unit_msg_test_edit_preread,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0, 1 },
    { "fields_count past the buffer is refused", nxt_unit_msg_test_edit_count,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0, 1 },
    { "cached field index out of range is refused",
      nxt_unit_msg_test_edit_index,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0, 1 },
    { "field name sptr out of buffer is refused",
      nxt_unit_msg_test_edit_field_name,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0, 1 },
    { "field name inside fields[] is refused",
      nxt_unit_msg_test_edit_field_inside,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0, 1 },
    { "duplicate fields are grouped", NULL,
      NXT_UNIT_MSG_TEST_GROUP_DUP, 2, NXT_UNIT_OK, 1, 1 },
    { "fields_count raised after the check stays in the buffer", NULL,
      NXT_UNIT_MSG_TEST_RAISE_COUNT, PORT_MMAP_CHUNK_COUNT - 1, NXT_UNIT_OK,
      1, 1 },
};


static int
nxt_unit_msg_test_request_case(void *data)
{
    int                                rc;
    const nxt_unit_msg_test_request_t  *t = data;

    nxt_unit_msg_test_mode = t->mode;

    rc = nxt_unit_msg_test_send_request(t->chunk, t->edit);

    if (nxt_unit_msg_test_handler_calls != t->handler_calls) {
        printf("unit msg test: handler called %d times, expected %d\n",
               nxt_unit_msg_test_handler_calls, t->handler_calls);
        return 1;
    }

    if (t->handler_calls != 0 && !nxt_unit_msg_test_handler_ok) {
        printf("unit msg test: handler saw a wrong request\n");
        return 2;
    }

    if (nxt_unit_msg_test_rpc_errors != t->rpc_errors) {
        printf("unit msg test: %d RPC_ERROR messages, expected %d\n",
               nxt_unit_msg_test_rpc_errors, t->rpc_errors);

        /* The child ends with _exit(), which does not flush stdout. */
        fflush(stdout);

        return 3;
    }

    return NXT_UNIT_MSG_TEST_RC(rc);
}


/*
 * A request that a worker takes from the shared queue waits in the router
 * for REQ_HEADERS_ACK.  libunit failed some requests before that
 * acknowledgement and did not tell the router, so the router waited for
 * ever.  Each case below fails a request before the acknowledgement, and
 * the router must get exactly one RPC_ERROR for its stream.
 */

static int
nxt_unit_msg_test_send_not_mmap(void)
{
    nxt_port_msg_t  msg;

    memset(&msg, 0, sizeof(msg));

    msg.stream = NXT_UNIT_MSG_TEST_STREAM;
    msg.pid = nxt_unit_msg_test_pid;
    msg.reply_port = nxt_unit_msg_test_reply_port;
    msg.type = _NXT_PORT_MSG_REQ_HEADERS;
    msg.last = 1;

    return nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, &msg, sizeof(msg),
                                     -1);
}


static int
nxt_unit_msg_test_send_short(void)
{
    return nxt_unit_msg_test_send_chunk(_NXT_PORT_MSG_REQ_HEADERS, 1, 2, 8);
}


static int
nxt_unit_msg_test_send_bad_record(void)
{
    return nxt_unit_msg_test_send_chunk(_NXT_PORT_MSG_REQ_HEADERS, 1,
                                        PORT_MMAP_CHUNK_COUNT, 100);
}


static int
nxt_unit_msg_test_send_no_req_info(void)
{
    nxt_unit_test_request_info_failures(1);

    return nxt_unit_msg_test_send_request(2, NULL);
}


typedef struct {
    const char  *name;
    int         (*send)(void);
} nxt_unit_msg_test_unacked_t;


static const nxt_unit_msg_test_unacked_t  unacked[] = {
    { "request not in shared memory gets RPC_ERROR",
      nxt_unit_msg_test_send_not_mmap },
    { "request shorter than its header gets RPC_ERROR",
      nxt_unit_msg_test_send_short },
    { "request with a bad mmap record gets RPC_ERROR",
      nxt_unit_msg_test_send_bad_record },
    { "request with no request info gets RPC_ERROR",
      nxt_unit_msg_test_send_no_req_info },
};


static int
nxt_unit_msg_test_unacked_case(void *data)
{
    int                                rc;
    const nxt_unit_msg_test_unacked_t  *t = data;

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_READ_BODY;
    nxt_unit_msg_test_handler_calls = 0;
    nxt_unit_msg_test_rpc_errors = 0;

    rc = t->send();

    if (rc != NXT_UNIT_ERROR || nxt_unit_msg_test_handler_calls != 0
        || nxt_unit_msg_test_rpc_errors != 1)
    {
        printf("unit msg test: rc %d, handler %d, %d RPC_ERROR messages, "
               "expected rc %d, handler 0, 1 RPC_ERROR\n", rc,
               nxt_unit_msg_test_handler_calls, nxt_unit_msg_test_rpc_errors,
               NXT_UNIT_ERROR);

        /* The child ends with _exit(), which does not flush stdout. */
        fflush(stdout);

        return 1;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * When libunit has no descriptor for the reply port, it cannot send
 * RPC_ERROR.  It must then release the request.  It kept the request
 * active instead, with ->response_port not set, or set to a port with no
 * descriptor.  The next quit called the close handler, and RPC_ERROR went
 * through that pointer.
 *
 * "get_port_fails" 0: the router's pid is unknown ("process not found").
 * 1: the reply port is unknown, and GET_PORT cannot be sent.
 */

static int
nxt_unit_msg_test_send_to_port(pid_t pid, uint16_t reply_port)
{
    u_char               *p;
    u_char               buf[sizeof(nxt_port_msg_t)
                             + sizeof(nxt_port_mmap_msg_t)];
    uint32_t             size;
    nxt_port_msg_t       msg;
    nxt_port_mmap_msg_t  rec;

    p = nxt_port_mmap_chunk_start(nxt_unit_msg_test_seg0, 2);

    size = nxt_unit_msg_test_write_request(p);

    memset(&msg, 0, sizeof(msg));

    msg.stream = NXT_UNIT_MSG_TEST_STREAM;
    msg.pid = pid;
    msg.reply_port = reply_port;
    msg.type = _NXT_PORT_MSG_REQ_HEADERS;
    msg.last = 1;
    msg.mmap = 1;

    rec.mmap_id = 0;
    rec.chunk_id = 2;
    rec.size = size;

    memcpy(buf, &msg, sizeof(msg));
    memcpy(buf + sizeof(msg), &rec, sizeof(rec));

    return nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, buf, sizeof(buf),
                                     -1);
}


static int
nxt_unit_msg_test_no_port_case(void *data)
{
    int             rc, get_port_fails;
    nxt_port_msg_t  msg;

    get_port_fails = *(const int *) data;

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_READ_BODY;

    nxt_unit_msg_test_handler_calls = 0;
    nxt_unit_msg_test_rpc_errors = 0;
    nxt_unit_msg_test_send_fails = get_port_fails;

    rc = nxt_unit_msg_test_send_to_port(get_port_fails
                                        ? nxt_unit_msg_test_pid
                                        : nxt_unit_msg_test_pid + 1, 9);

    nxt_unit_msg_test_send_fails = 0;

    if (rc != NXT_UNIT_ERROR || nxt_unit_msg_test_handler_calls != 0
        || nxt_unit_msg_test_rpc_errors != 0)
    {
        printf("unit msg test: rc %d, handler %d, %d RPC_ERROR messages\n",
               rc, nxt_unit_msg_test_handler_calls,
               nxt_unit_msg_test_rpc_errors);
        fflush(stdout);

        return 1;
    }

    /* A QUIT with no parameter is NXT_PORT_QUIT_NORMAL. */
    memset(&msg, 0, sizeof(msg));

    msg.pid = nxt_unit_msg_test_pid;
    msg.type = _NXT_PORT_MSG_QUIT;
    msg.last = 1;

    nxt_unit_msg_test_close_calls = 0;

    (void) nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, &msg, sizeof(msg),
                                     -1);

    if (nxt_unit_msg_test_close_calls != 0
        || nxt_unit_msg_test_rpc_errors != 0)
    {
        printf("unit msg test: the quit found the request active: "
               "%d close calls, %d RPC_ERROR messages\n",
               nxt_unit_msg_test_close_calls, nxt_unit_msg_test_rpc_errors);
        fflush(stdout);

        return 2;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * A port whose GET_PORT failed stayed in lib->ports with no descriptor.
 * A later request to it found it, waited for a NEW_PORT that nobody asked
 * for, and did not send GET_PORT again.
 */

static int
nxt_unit_msg_test_get_port_again_case(void *data)
{
    int  rc;

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_READ_BODY;

    nxt_unit_msg_test_send_fails = 1;

    rc = nxt_unit_msg_test_send_to_port(nxt_unit_msg_test_pid, 9);

    nxt_unit_msg_test_send_fails = 0;

    if (rc != NXT_UNIT_ERROR) {
        printf("unit msg test: rc %d, expected %d\n", rc, NXT_UNIT_ERROR);
        fflush(stdout);

        return 1;
    }

    nxt_unit_msg_test_get_ports = 0;

    rc = nxt_unit_msg_test_send_to_port(nxt_unit_msg_test_pid, 9);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_get_ports != 1) {
        printf("unit msg test: the second request: rc %d, %d GET_PORT "
               "messages, expected rc %d, 1 GET_PORT\n", rc,
               nxt_unit_msg_test_get_ports, NXT_UNIT_OK);
        fflush(stdout);

        return 2;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * A request that fails before its acknowledgement while the GET_PORT of an
 * earlier request to the same reply port is still in flight.  The port is in
 * lib->ports but not ready, so libunit cannot send RPC_ERROR; it logs that,
 * and the warning must name the port as not ready, not as not known.
 */

static int
nxt_unit_msg_test_port_not_ready_case(void *data)
{
    int             rc, saved, fds[2];
    char            log[4096];
    ssize_t         n;
    nxt_port_msg_t  msg;

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_READ_BODY;

    nxt_unit_msg_test_get_ports = 0;

    rc = nxt_unit_msg_test_send_to_port(nxt_unit_msg_test_pid, 9);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_get_ports != 1) {
        printf("unit msg test: the first request: rc %d, %d GET_PORT "
               "messages, expected rc %d, 1 GET_PORT\n", rc,
               nxt_unit_msg_test_get_ports, NXT_UNIT_OK);
        fflush(stdout);

        return 1;
    }

    if (pipe(fds) == -1) {
        return 2;
    }

    saved = dup(STDERR_FILENO);

    if (saved == -1
        || dup2(fds[1], STDERR_FILENO) == -1
        || fcntl(fds[0], F_SETFL, O_NONBLOCK) == -1)
    {
        return 2;
    }

    memset(&msg, 0, sizeof(msg));

    msg.stream = NXT_UNIT_MSG_TEST_STREAM;
    msg.pid = nxt_unit_msg_test_pid;
    msg.reply_port = 9;
    msg.type = _NXT_PORT_MSG_REQ_HEADERS;
    msg.last = 1;

    nxt_unit_msg_test_rpc_errors = 0;

    rc = nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, &msg, sizeof(msg),
                                   -1);

    (void) dup2(saved, STDERR_FILENO);
    close(saved);
    close(fds[1]);

    n = read(fds[0], log, sizeof(log) - 1);
    close(fds[0]);

    log[n > 0 ? n : 0] = '\0';

    if (rc != NXT_UNIT_ERROR || nxt_unit_msg_test_rpc_errors != 0
        || strstr(log, "is not ready") == NULL
        || strstr(log, "is not known") != NULL)
    {
        printf("unit msg test: the failed request: rc %d, %d RPC_ERROR "
               "messages, expected rc %d, 0 RPC_ERROR and a \"not ready\" "
               "warning; log: %s\n", rc, nxt_unit_msg_test_rpc_errors,
               NXT_UNIT_ERROR, log);
        fflush(stdout);

        return 3;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * A sibling process that keeps changing the preread offset while the
 * request is checked and then used.  libunit resolved the sptr once for
 * the check and again for the body start, so a change in between put the
 * body start 2 GB past the buffer, and the handler's read went there.
 */

typedef struct {
    volatile uint32_t  *offset;
    uint32_t           good;
    volatile int       stop;
} nxt_unit_msg_test_racer_t;


static void *
nxt_unit_msg_test_racer(void *data)
{
    nxt_unit_msg_test_racer_t  *racer = data;

    while (!racer->stop) {
        *racer->offset = 0x7FFFFFF0;
        *racer->offset = racer->good;
    }

    return NULL;
}


static int
nxt_unit_msg_test_preread_race_case(void *data)
{
    int                        i, rc, accepted;
    u_char                     *p;
    uint32_t                   size;
    pthread_t                  thread;
    nxt_unit_request_t         *r;
    nxt_unit_msg_test_racer_t  racer;

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_READ_BODY;

    p = nxt_port_mmap_chunk_start(nxt_unit_msg_test_seg0, 2);
    r = (nxt_unit_request_t *) p;

    size = nxt_unit_msg_test_write_request(p);

    racer.offset = &r->preread_content.offset;
    racer.good = r->preread_content.offset;
    racer.stop = 0;

    if (pthread_create(&thread, NULL, nxt_unit_msg_test_racer, &racer) != 0) {
        return 1;
    }

    accepted = 0;

    for (i = 0; i < 20000; i++) {
        /* Releasing the request wipes the chunk; write it again. */
        (void) nxt_unit_msg_test_write_request(p);

        nxt_unit_msg_test_handler_calls = 0;
        nxt_unit_msg_test_handler_ok = 0;

        rc = nxt_unit_msg_test_send_chunk(_NXT_PORT_MSG_REQ_HEADERS, 1, 2,
                                          size);

        if (rc == NXT_UNIT_OK) {
            if (nxt_unit_msg_test_handler_calls != 1
                || !nxt_unit_msg_test_handler_ok)
            {
                racer.stop = 1;
                pthread_join(thread, NULL);

                printf("unit msg test: accepted request, wrong body\n");
                return 2;
            }

            accepted++;
        }
    }

    racer.stop = 1;
    pthread_join(thread, NULL);

    if (accepted == 0) {
        printf("unit msg test: no request was accepted under the race\n");
        return 3;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * A Drupal REST error: a header of many fields, over 1024 bytes, in shared
 * memory, and a short JSON body.
 */
static int
nxt_unit_msg_test_respond(nxt_unit_request_info_t *req)
{
    int   i, rc;
    char  name[] = "X-Test-00";

    static const char  value[] = "config:field.storage.entity_test.field_test";

    rc = nxt_unit_response_init(req, 404, NXT_UNIT_MSG_TEST_FIELDS,
                                NXT_UNIT_MSG_TEST_FIELDS
                                * (sizeof(name) + sizeof(value)));

    for (i = 0; rc == NXT_UNIT_OK && i < NXT_UNIT_MSG_TEST_FIELDS; i++) {
        name[7] = '0' + i / 10;
        name[8] = '0' + i % 10;

        rc = nxt_unit_response_add_field(req, name, nxt_length(name),
                                         value, nxt_length(value));
    }

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_response_send(req);
    }

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_response_write(req, NXT_UNIT_MSG_TEST_JSON,
                                     nxt_length(NXT_UNIT_MSG_TEST_JSON));
    }

    return rc;
}


/* A hello world, as the PHP module sends it: header, body, then last. */
static int
nxt_unit_msg_test_respond_small(nxt_unit_request_info_t *req)
{
    int  rc;

    rc = nxt_unit_response_init(req, 200, 1,
                                nxt_length("Content-Type")
                                + nxt_length("text/html; charset=UTF-8"));

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_response_add_field(req, "Content-Type",
                                         nxt_length("Content-Type"),
                                         "text/html; charset=UTF-8",
                                         nxt_length("text/html; "
                                                    "charset=UTF-8"));
    }

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_response_send(req);
    }

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_response_write(req, NXT_UNIT_MSG_TEST_HELLO,
                                     nxt_length(NXT_UNIT_MSG_TEST_HELLO));
    }

    return rc;
}


/*
 * A buffer allocated before the header is sent and sent after a later
 * write, as the Java module does with its output buffer.
 */
static int
nxt_unit_msg_test_respond_held(nxt_unit_request_info_t *req)
{
    int             rc;
    char            data[NXT_UNIT_MSG_TEST_WRITE_SIZE];
    nxt_unit_buf_t  *buf;

    rc = nxt_unit_response_init(req, 200, 1,
                                nxt_length("Content-Type")
                                + nxt_length("text/plain"));

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_response_add_field(req, "Content-Type",
                                         nxt_length("Content-Type"),
                                         "text/plain",
                                         nxt_length("text/plain"));
    }

    if (rc != NXT_UNIT_OK) {
        return rc;
    }

    buf = nxt_unit_response_buf_alloc(req, NXT_UNIT_MSG_TEST_HELD_SIZE);
    if (buf == NULL) {
        return NXT_UNIT_ERROR;
    }

    memset(buf->free, 'C', NXT_UNIT_MSG_TEST_HELD_SIZE);
    buf->free += NXT_UNIT_MSG_TEST_HELD_SIZE;

    memset(data, 'B', sizeof(data));

    rc = nxt_unit_response_send(req);

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_response_write(req, data, sizeof(data));
    }

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_buf_send(buf);

    } else {
        nxt_unit_buf_free(buf);
    }

    return rc;
}


/* A header in shared memory, then a body part that finds no free chunk. */
static int
nxt_unit_msg_test_respond_no_chunk(nxt_unit_request_info_t *req)
{
    int   rc;
    char  part[NXT_UNIT_MSG_TEST_PART_SIZE];

    rc = nxt_unit_response_init(req, 200, 1,
                                nxt_length("Content-Type")
                                + nxt_length("text/plain"));

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_response_add_field(req, "Content-Type",
                                         nxt_length("Content-Type"),
                                         "text/plain",
                                         nxt_length("text/plain"));
    }

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_response_send(req);
    }

    if (rc == NXT_UNIT_OK) {
        memset(part, 'B', sizeof(part));

        nxt_unit_test_shm_probe_failures(1);

        rc = nxt_unit_response_write(req, part, sizeof(part));
    }

    return rc;
}


/*
 * A body written before nxt_unit_response_send(), as the WSGI module does.
 * nxt_unit_response_write_nb() puts into the fields message as many of the
 * first body bytes as the fields buffer has left.  A small buffer has room
 * for NXT_UNIT_RESPONSE_PLAIN_ALLOC bytes, so it takes the whole small body
 * as piggyback content.  The router gets the ack, the combined fields+body
 * message and the last message: "ABL".
 */
static int
nxt_unit_msg_test_respond_write_first(nxt_unit_request_info_t *req)
{
    int       rc;
    uint32_t  left;

    rc = nxt_unit_response_init(req, 200, 1,
                                nxt_length("Content-Type")
                                + nxt_length("text/html; charset=UTF-8"));

    if (rc == NXT_UNIT_OK) {
        rc = nxt_unit_response_add_field(req, "Content-Type",
                                         nxt_length("Content-Type"),
                                         "text/html; charset=UTF-8",
                                         nxt_length("text/html; "
                                                    "charset=UTF-8"));
    }

    if (rc != NXT_UNIT_OK) {
        return rc;
    }

    left = req->response_buf->end - req->response_buf->free;

    if (left != NXT_UNIT_RESPONSE_PLAIN_ALLOC - 78) {
        printf("unit msg test: the fields buffer has %d bytes left, "
               "expected %d\n", (int) left,
               (int) (NXT_UNIT_RESPONSE_PLAIN_ALLOC - 78));
        fflush(stdout);

        return NXT_UNIT_ERROR;
    }

    rc = nxt_unit_response_write(req, NXT_UNIT_MSG_TEST_HELLO,
                                 nxt_length(NXT_UNIT_MSG_TEST_HELLO));
    if (rc != NXT_UNIT_OK) {
        return rc;
    }

    if (!nxt_unit_response_is_sent(req)) {
        printf("unit msg test: response was not sent after write\n");
        fflush(stdout);

        return NXT_UNIT_ERROR;
    }

    return NXT_UNIT_OK;
}


/* A router engine port with a shared memory queue, as NEW_PORT adds it. */
static void *
nxt_unit_msg_test_engine_port(void)
{
    int   rc, pair[2];
    void  *queue;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == -1) {
        perror("socketpair");
        return NULL;
    }

    queue = mmap(NULL, sizeof(nxt_port_queue_t), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (queue == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    nxt_port_queue_init(queue);

    rc = nxt_unit_test_add_queue_port(nxt_unit_msg_test_ctx,
                                      nxt_unit_msg_test_pid,
                                      NXT_UNIT_MSG_TEST_ENGINE_PORT, pair[0],
                                      queue);
    if (rc != NXT_UNIT_OK) {
        printf("unit msg test: engine port not added\n");
        return NULL;
    }

    nxt_unit_msg_test_reply_port = NXT_UNIT_MSG_TEST_ENGINE_PORT;

    return queue;
}


/*
 * Sends a request on "stream" to the engine port and lets the handler
 * answer it in "mode".  Returns 0 if the handler answered.
 */
static int
nxt_unit_msg_test_engine_request(nxt_unit_msg_test_mode_t mode,
    uint32_t stream, nxt_chunk_id_t chunk)
{
    int  rc;

    nxt_unit_msg_test_mode = mode;
    nxt_unit_msg_test_stream = stream;
    nxt_unit_msg_test_ndgrams = 0;

    rc = nxt_unit_msg_test_send_request(chunk, NULL);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 1
        || !nxt_unit_msg_test_handler_ok
        || nxt_unit_msg_test_ndgrams == nxt_nitems(nxt_unit_msg_test_dgrams))
    {
        printf("unit msg test: response failed: %d\n", rc);
        fflush(stdout);

        return -1;
    }

    return 0;
}


/* Process "Y": a marker in the queue now, its datagram on the socket later. */
static int
nxt_unit_msg_test_marker_y(void *queue)
{
    int      notify;
    uint8_t  type;

    type = _NXT_PORT_MSG_READ_SOCKET;

    return nxt_port_queue_send(queue, &type, 1, &notify) == NXT_OK ? 0 : -1;
}


static void
nxt_unit_msg_test_dgram_y(void)
{
    nxt_unit_msg_test_dgram_t  *d;

    d = &nxt_unit_msg_test_dgrams[nxt_unit_msg_test_ndgrams++];

    memset(d, 0, sizeof(*d));

    d->msg.stream = 0x7FFF;
    d->msg.type = _NXT_PORT_MSG_DATA;
}


/*
 * Reads the queue as the router does (nxt_port_queue_read_handler()): a
 * queue message, or for each READ_SOCKET marker the next datagram, from
 * datagram "next" on.  Writes the messages of "stream" into "seq": 'A' the
 * acknowledgement, 'E' an error, 'L' the last message, 'B' data of "body"
 * bytes, 'C' of "body2" bytes, 'H' other data.  Returns 0 at the end of the
 * queue, 1 at a marker without a datagram, or -1 for a bad queue item.
 */
static int
nxt_unit_msg_test_router_run(void *queue, uint32_t stream, uint32_t body,
    uint32_t body2, size_t next, char *seq, size_t len)
{
    int                        rc;
    size_t                     n;
    ssize_t                    size;
    uint8_t                    qmsg[NXT_PORT_QUEUE_MSG_SIZE];
    nxt_port_mmap_msg_t        rec;
    nxt_unit_msg_test_dgram_t  d;

    n = 0;
    rc = 0;

    while (n < len - 1) {
        size = nxt_port_queue_recv(queue, qmsg);
        if (size < 0) {
            break;
        }

        if (size == 1 && qmsg[0] == _NXT_PORT_MSG_READ_SOCKET) {
            if (next >= nxt_unit_msg_test_ndgrams) {
                rc = 1;
                break;
            }

            d = nxt_unit_msg_test_dgrams[next++];

        } else if (size >= (ssize_t) sizeof(nxt_port_msg_t)) {
            memcpy(&d.msg, qmsg, sizeof(nxt_port_msg_t));

            d.size = size - sizeof(nxt_port_msg_t);

            if (d.msg.mmap
                && size >= (ssize_t) (sizeof(nxt_port_msg_t) + sizeof(rec)))
            {
                memcpy(&rec, qmsg + sizeof(nxt_port_msg_t), sizeof(rec));
                d.size = rec.size;
            }

        } else {
            printf("unit msg test: a %d-byte queue message\n", (int) size);
            return -1;
        }

        if (d.msg.stream != stream) {
            continue;
        }

        seq[n++] = (d.msg.type == _NXT_PORT_MSG_REQ_HEADERS_ACK) ? 'A'
                   : (d.msg.type == _NXT_PORT_MSG_RPC_ERROR) ? 'E'
                   : d.msg.last ? 'L'
                   : (d.size == body) ? 'B'
                   : (d.size == body2) ? 'C' : 'H';
    }

    seq[n] = '\0';

    return rc;
}


/*
 * Reads the whole queue with nxt_unit_msg_test_router_run(), every marker
 * with its datagram.  Counts the datagrams of "stream" into "*ndgrams".
 * Returns 0, or -1 for a marker without a datagram or a bad queue item.
 */
static int
nxt_unit_msg_test_router_read(void *queue, uint32_t stream, uint32_t body,
    uint32_t body2, char *seq, size_t len, size_t *ndgrams)
{
    int     rc;
    size_t  i;

    *ndgrams = 0;

    for (i = 0; i < nxt_unit_msg_test_ndgrams; i++) {
        *ndgrams += (nxt_unit_msg_test_dgrams[i].msg.stream == stream);
    }

    rc = nxt_unit_msg_test_router_run(queue, stream, body, body2, 0, seq,
                                      len);

    if (rc == 1) {
        printf("unit msg test: a marker without a datagram\n");
        return -1;
    }

    return rc;
}


static int
nxt_unit_msg_test_expect(const char *seq, const char *expect)
{
    if (strcmp(seq, expect) != 0) {
        printf("unit msg test: the router got \"%s\", expected \"%s\" "
               "(ack, error, header, body, last)\n", seq, expect);

        /* The child ends with _exit(), which does not flush stdout. */
        fflush(stdout);

        return -1;
    }

    return 0;
}


/*
 * A router engine port has a shared memory queue.  A message that goes to
 * the socket puts only a READ_SOCKET marker into the queue, and for each
 * marker the router reads the next datagram, from whichever process sent
 * it (nxt_port_queue_read_handler()).  Process "Y" puts its marker in
 * before this request and sends its datagram after it, as if Y was
 * preempted between the two.  The router must still get the messages of
 * the request in the order libunit sent them: ack, header, body, last.
 * libunit put the header into the queue and the body on the socket, so the
 * router read the body for Y's marker and parsed it as the header:
 * "response buffer too small for fields count: 574235237" and a 503.
 */
static int
nxt_unit_msg_test_order_case(void *data)
{
    char    seq[16];
    void    *queue;
    size_t  ndgrams;

    queue = nxt_unit_msg_test_engine_port();
    if (queue == NULL) {
        return 2;
    }

    if (nxt_unit_msg_test_marker_y(queue) != 0) {
        return 3;
    }

    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_ORDER,
                                         NXT_UNIT_MSG_TEST_STREAM, 2)
        != 0)
    {
        return 4;
    }

    nxt_unit_msg_test_dgram_y();

    if (nxt_unit_msg_test_router_read(queue, NXT_UNIT_MSG_TEST_STREAM,
                                      nxt_length(NXT_UNIT_MSG_TEST_JSON), 0,
                                      seq, sizeof(seq), &ndgrams)
        != 0)
    {
        return 5;
    }

    if (nxt_unit_msg_test_expect(seq, "AHBL") != 0) {
        return 7;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * Once this process has a shared memory segment, a small response sends one
 * datagram, the acknowledgement.  The header (in shared memory), the body (a
 * plain message that fits into a queue item) and the last message go into
 * the queue.  Before, the acknowledgement, the header and the body went to
 * the socket.
 */
static int
nxt_unit_msg_test_small_case(void *data)
{
    char     seq[16];
    void     *queue;
    size_t   ndgrams;
    uint8_t  qmsg[NXT_PORT_QUEUE_MSG_SIZE];

    queue = nxt_unit_msg_test_engine_port();
    if (queue == NULL) {
        return 2;
    }

    /* The first response makes the segment. */
    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_ORDER,
                                         NXT_UNIT_MSG_TEST_STREAM - 1, 2)
        != 0)
    {
        return 4;
    }

    while (nxt_port_queue_recv(queue, qmsg) >= 0) { /* void */ }

    if (nxt_unit_msg_test_marker_y(queue) != 0) {
        return 3;
    }

    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_SMALL,
                                         NXT_UNIT_MSG_TEST_STREAM, 3)
        != 0)
    {
        return 4;
    }

    nxt_unit_msg_test_dgram_y();

    if (nxt_unit_msg_test_router_read(queue, NXT_UNIT_MSG_TEST_STREAM,
                                      nxt_length(NXT_UNIT_MSG_TEST_HELLO), 0,
                                      seq, sizeof(seq), &ndgrams)
        != 0)
    {
        return 5;
    }

    if (nxt_unit_msg_test_expect(seq, "AHBL") != 0) {
        return 7;
    }

    if (ndgrams != 1) {
        printf("unit msg test: a small response sent %d datagrams, "
               "expected 1\n", (int) ndgrams);
        fflush(stdout);

        return 8;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * A small buffer of up to 1008 bytes is allocated at 1008 bytes to leave
 * room for a small body.  When the body is written before
 * nxt_unit_response_send(), it is sent inside the fields message as
 * piggyback content.  The router gets the ack ('A'), the combined message
 * ('B' for data of 84 bytes), and the last message ('L'): "ABL".
 */
static int
nxt_unit_msg_test_exact_fields_case(void *data)
{
    char     seq[16];
    void     *queue;
    size_t   ndgrams;
    uint8_t  qmsg[NXT_PORT_QUEUE_MSG_SIZE];

    queue = nxt_unit_msg_test_engine_port();
    if (queue == NULL) {
        return 2;
    }

    /* The first response makes the segment. */
    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_ORDER,
                                         NXT_UNIT_MSG_TEST_STREAM - 1, 2)
        != 0)
    {
        return 4;
    }

    while (nxt_port_queue_recv(queue, qmsg) >= 0) { /* void */ }

    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_WRITE_FIRST,
                                         NXT_UNIT_MSG_TEST_STREAM, 3)
        != 0)
    {
        return 4;
    }

    if (nxt_unit_msg_test_router_read(queue, NXT_UNIT_MSG_TEST_STREAM,
                                      78 + nxt_length(NXT_UNIT_MSG_TEST_HELLO),
                                      0, seq, sizeof(seq), &ndgrams)
        != 0)
    {
        return 5;
    }

    if (nxt_unit_msg_test_expect(seq, "ABL") != 0) {
        return 7;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * Without shared memory a request stays on the socket until its last
 * message, which goes into the queue.  The case needs a process with no
 * segment yet; the parent made none before the fork, which the count of 3
 * datagrams checks.
 */
static int
nxt_unit_msg_test_no_shm_case(void *data)
{
    char    seq[16];
    void    *queue;
    size_t  ndgrams;

    queue = nxt_unit_msg_test_engine_port();
    if (queue == NULL) {
        return 2;
    }

    nxt_unit_test_new_mmap_failures(1);

    if (nxt_unit_msg_test_marker_y(queue) != 0) {
        return 3;
    }

    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_SMALL,
                                         NXT_UNIT_MSG_TEST_STREAM, 2)
        != 0)
    {
        return 4;
    }

    nxt_unit_msg_test_dgram_y();

    if (nxt_unit_msg_test_router_read(queue, NXT_UNIT_MSG_TEST_STREAM,
                                      nxt_length(NXT_UNIT_MSG_TEST_HELLO), 0,
                                      seq, sizeof(seq), &ndgrams)
        != 0)
    {
        return 5;
    }

    if (nxt_unit_msg_test_expect(seq, "AHBL") != 0) {
        return 7;
    }

    if (ndgrams != 3) {
        printf("unit msg test: %d datagrams without shared memory, "
               "expected 3\n", (int) ndgrams);
        fflush(stdout);

        return 8;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * A plain buffer over 15 bytes cannot go into the queue.  When one is
 * allocated, the request stays on the socket until its last message, even
 * if a later write gets shared memory.  Otherwise that write would go into
 * the queue, and the plain buffer, sent after it, would have to wait for the
 * router to take it (here, with no router, until the wait fails).
 */
static int
nxt_unit_msg_test_plain_held_case(void *data)
{
    char    seq[16];
    void    *queue;
    size_t  ndgrams;

    queue = nxt_unit_msg_test_engine_port();
    if (queue == NULL) {
        return 2;
    }

    /* The header gets no segment, so plain memory; the 2000-byte write does. */
    nxt_unit_test_new_mmap_failures(1);

    if (nxt_unit_msg_test_marker_y(queue) != 0) {
        return 3;
    }

    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_PLAIN_HELD,
                                         NXT_UNIT_MSG_TEST_STREAM, 2)
        != 0)
    {
        return 4;
    }

    nxt_unit_msg_test_dgram_y();

    if (nxt_unit_msg_test_router_read(queue, NXT_UNIT_MSG_TEST_STREAM,
                                      NXT_UNIT_MSG_TEST_WRITE_SIZE,
                                      NXT_UNIT_MSG_TEST_HELD_SIZE,
                                      seq, sizeof(seq), &ndgrams)
        != 0)
    {
        return 5;
    }

    if (nxt_unit_msg_test_expect(seq, "AHBCL") != 0) {
        return 7;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * The frames of a websocket can wait in the router for a slow client, so
 * they stay in plain memory and on the socket, as in 1.37.0, even when this
 * process has shared memory.  The 101 response goes to the socket.
 */
static int
nxt_unit_msg_test_ws_socket_case(void *data)
{
    char     seq[16];
    void     *queue;
    size_t   ndgrams;
    uint8_t  qmsg[NXT_PORT_QUEUE_MSG_SIZE];

    queue = nxt_unit_msg_test_engine_port();
    if (queue == NULL) {
        return 2;
    }

    /* The first response makes the segment. */
    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_ORDER,
                                         NXT_UNIT_MSG_TEST_STREAM - 1, 2)
        != 0)
    {
        return 4;
    }

    while (nxt_port_queue_recv(queue, qmsg) >= 0) { /* void */ }

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_UPGRADE;
    nxt_unit_msg_test_stream = NXT_UNIT_MSG_TEST_STREAM;
    nxt_unit_msg_test_ndgrams = 0;

    if (nxt_unit_msg_test_send_request(3, nxt_unit_msg_test_edit_handshake)
        != NXT_UNIT_OK
        || nxt_unit_msg_test_handler_calls != 1
        || !nxt_unit_msg_test_handler_ok)
    {
        printf("unit msg test: upgrade failed\n");
        fflush(stdout);

        return 4;
    }

    if (nxt_unit_msg_test_router_read(queue, NXT_UNIT_MSG_TEST_STREAM, 0, 0,
                                      seq, sizeof(seq), &ndgrams)
        != 0)
    {
        return 5;
    }

    if (nxt_unit_msg_test_expect(seq, "AH") != 0) {
        return 7;
    }

    if (ndgrams != 2) {
        printf("unit msg test: a websocket sent %d datagrams, expected 2\n",
               (int) ndgrams);
        fflush(stdout);

        return 8;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/* The router as a thread, for nxt_unit_msg_test_drain_case(). */
typedef struct {
    void               *queue;
    nxt_nncq_atomic_t  tail;        /* After Y's marker. */
    char               seq[16];
    size_t             n;
    int                early;       /* Data of the request came before Y's. */
} nxt_unit_msg_test_router_t;


static void *
nxt_unit_msg_test_router_thread(void *data)
{
    int                         i, done, y, armed;
    size_t                      next, k;
    ssize_t                     size;
    uint8_t                     qmsg[NXT_PORT_QUEUE_MSG_SIZE];
    nxt_port_queue_t            *q;
    nxt_port_mmap_msg_t         rec;
    nxt_unit_msg_test_dgram_t   d;
    nxt_unit_msg_test_router_t  *r;

    r = data;
    q = r->queue;
    next = 0;
    done = 0;
    y = 0;
    armed = -1;

    /* About two seconds in steps of 100 us. */
    for (i = 0; !done && i < 20000 && r->n < sizeof(r->seq) - 1; i++) {
        size = nxt_port_queue_recv(q, qmsg);
        if (size < 0) {
            (void) usleep(100);
            continue;
        }

        if (size == 1 && qmsg[0] == _NXT_PORT_MSG_READ_SOCKET) {

            for ( ;; ) {
                pthread_mutex_lock(&nxt_unit_msg_test_dgrams_mutex);

                if (next < nxt_unit_msg_test_ndgrams) {
                    d = nxt_unit_msg_test_dgrams[next++];
                    pthread_mutex_unlock(&nxt_unit_msg_test_dgrams_mutex);
                    break;
                }

                /*
                 * Y sends its datagram 20 ms after the request put its header
                 * into the queue: the ack's marker and the header follow Y's
                 * marker.  A data datagram of the request before that one is
                 * too early.
                 */
                if (!y && armed < 0
                    && (int32_t) (nxt_nncq_tail(&q->queue) - (r->tail + 2))
                       >= 0)
                {
                    armed = i;
                }

                if (!y && armed >= 0 && i - armed >= 200) {
                    for (k = 0; k < nxt_unit_msg_test_ndgrams; k++) {
                        if (nxt_unit_msg_test_dgrams[k].msg.stream
                                == NXT_UNIT_MSG_TEST_STREAM
                            && nxt_unit_msg_test_dgrams[k].msg.type
                               != _NXT_PORT_MSG_REQ_HEADERS_ACK)
                        {
                            r->early = 1;
                        }
                    }

                    nxt_unit_msg_test_dgram_y();
                    y = 1;
                }

                pthread_mutex_unlock(&nxt_unit_msg_test_dgrams_mutex);

                (void) usleep(100);

                if (++i >= 20000) {
                    return NULL;
                }
            }

        } else if (size >= (ssize_t) sizeof(nxt_port_msg_t)) {
            memcpy(&d.msg, qmsg, sizeof(nxt_port_msg_t));

            d.size = size - sizeof(nxt_port_msg_t);

            if (d.msg.mmap
                && size >= (ssize_t) (sizeof(nxt_port_msg_t) + sizeof(rec)))
            {
                memcpy(&rec, qmsg + sizeof(nxt_port_msg_t), sizeof(rec));
                d.size = rec.size;
            }

        } else {
            return NULL;
        }

        if (d.msg.stream != NXT_UNIT_MSG_TEST_STREAM) {
            continue;
        }

        r->seq[r->n++] = (d.msg.type == _NXT_PORT_MSG_REQ_HEADERS_ACK) ? 'A'
                         : d.msg.last ? 'L'
                         : (d.size == NXT_UNIT_MSG_TEST_PART_SIZE) ? 'B'
                         : 'H';

        done = d.msg.last;
    }

    return NULL;
}


/*
 * A request has its header in the queue, and its body part finds no free
 * chunk, so it gets plain memory.  It must not send that part to the
 * socket while the router still has its header ahead: the ack's marker is
 * in the queue before the header, and the router waits there for a
 * datagram (the ack took Y's marker), which comes 20 ms later from Y.  The
 * request waits until the router has taken the header, then sends the part
 * to the socket.
 */
static int
nxt_unit_msg_test_drain_case(void *data)
{
    void                        *queue;
    size_t                      i, ndgrams;
    uint8_t                     qmsg[NXT_PORT_QUEUE_MSG_SIZE];
    pthread_t                   thread;
    nxt_unit_msg_test_router_t  r;

    queue = nxt_unit_msg_test_engine_port();
    if (queue == NULL) {
        return 2;
    }

    /* The first response makes the segment. */
    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_ORDER,
                                         NXT_UNIT_MSG_TEST_STREAM - 1, 2)
        != 0)
    {
        return 4;
    }

    while (nxt_port_queue_recv(queue, qmsg) >= 0) { /* void */ }

    if (nxt_unit_msg_test_marker_y(queue) != 0) {
        return 3;
    }

    memset(&r, 0, sizeof(r));
    r.queue = queue;
    r.tail = nxt_nncq_tail(&((nxt_port_queue_t *) queue)->queue);

    nxt_unit_msg_test_ndgrams = 0;

    if (pthread_create(&thread, NULL, nxt_unit_msg_test_router_thread, &r)
        != 0)
    {
        return 2;
    }

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_NO_CHUNK;
    nxt_unit_msg_test_stream = NXT_UNIT_MSG_TEST_STREAM;

    if (nxt_unit_msg_test_send_request(3, NULL) != NXT_UNIT_OK
        || nxt_unit_msg_test_handler_calls != 1
        || !nxt_unit_msg_test_handler_ok)
    {
        printf("unit msg test: response failed\n");
        fflush(stdout);

        (void) pthread_join(thread, NULL);
        return 4;
    }

    (void) pthread_join(thread, NULL);

    r.seq[r.n] = '\0';

    ndgrams = 0;

    for (i = 0; i < nxt_unit_msg_test_ndgrams; i++) {
        ndgrams += (nxt_unit_msg_test_dgrams[i].msg.stream
                    == NXT_UNIT_MSG_TEST_STREAM);
    }

    if (r.early) {
        printf("unit msg test: the body part went out before the router took "
               "the header\n");
        fflush(stdout);

        return 9;
    }

    if (nxt_unit_msg_test_expect(r.seq, "AHBL") != 0) {
        return 7;
    }

    /* The ack and the body part went to the socket. */
    if (ndgrams != 2) {
        printf("unit msg test: %d datagrams after the drain, expected 2\n",
               (int) ndgrams);
        fflush(stdout);

        return 8;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * Runs in the send callback for nxt_unit_msg_test_own_item_case().  When
 * the request sends its ack, the router takes the ack's marker.  When the
 * header of the request goes into the queue, libunit sends READ_QUEUE.
 * Then the router takes the header.  Process "Z" puts a marker into the
 * queue and stops before its datagram, and another process puts in an
 * item "W".  The router takes Z's marker and waits for its datagram.  All
 * of this comes before libunit returns from sending the header.
 */
static void
nxt_unit_msg_test_stall_step(const nxt_port_msg_t *msg)
{
    int                        notify;
    ssize_t                    size;
    uint8_t                    qmsg[NXT_PORT_QUEUE_MSG_SIZE];
    nxt_nncq_t                 *q;
    nxt_port_msg_t             w, h;
    nxt_unit_msg_test_stall_t  *st;

    st = &nxt_unit_msg_test_stall;

    if (st->stage == 0 && msg->type == _NXT_PORT_MSG_REQ_HEADERS_ACK) {
        size = nxt_port_queue_recv(st->queue, qmsg);

        st->stage = (size == 1 && qmsg[0] == _NXT_PORT_MSG_READ_SOCKET)
                    ? 1 : -1;
        return;
    }

    if (st->stage != 1 || msg->type != _NXT_PORT_MSG_READ_QUEUE) {
        return;
    }

    st->stage = -1;

    size = nxt_port_queue_recv(st->queue, qmsg);
    if (size < (ssize_t) sizeof(nxt_port_msg_t)) {
        return;
    }

    memcpy(&h, qmsg, sizeof(nxt_port_msg_t));

    if (h.stream != NXT_UNIT_MSG_TEST_STREAM || h.last) {
        return;
    }

    if (st->lap) {
        q = &((nxt_port_queue_t *) st->queue)->queue;

        if (nxt_nncq_tail(q) != nxt_nncq_head(q)) {
            return;
        }

        /*
         * The header was at head - 1.  After a lap of the counters, the
         * head is one item behind that position and the tail is at it:
         * one later item waits, and the header is not it.
         */
        st->head = nxt_nncq_head(q);
        q->head = st->head - 2;
        q->tail = st->head - 1;

        st->stage = 2;
        return;
    }

    memset(&w, 0, sizeof(w));
    w.stream = 0x7FFF;
    w.type = _NXT_PORT_MSG_DATA;

    if (nxt_unit_msg_test_marker_y(st->queue) != 0
        || nxt_port_queue_send(st->queue, &w, sizeof(w), &notify) != NXT_OK)
    {
        return;
    }

    size = nxt_port_queue_recv(st->queue, qmsg);

    if (size == 1 && qmsg[0] == _NXT_PORT_MSG_READ_SOCKET) {
        st->stage = 2;
    }
}


/*
 * nxt_port_queue_taken() across the wrap of the 32-bit queue counters.
 * The router can take more than 2^31 items after the last queue item of a
 * long response.  A signed half-range check then read such an item as not
 * taken, and the request waited NXT_UNIT_DRAIN_TIMEOUT for nothing.
 *
 * After 2^32 items, the head comes back to the position of the item.  The
 * items that wait are those from the head up to the tail, so the item is
 * taken unless the tail is past its position too.  A check against the
 * queue size alone read it as not taken while the head was up to the queue
 * size behind.  The one case that stays is the head behind the position
 * and the tail past it ("the residual").  A tail behind the head is not a
 * state of the queue: the window is then the queue size, as before.
 */
static int
nxt_unit_msg_test_queue_wrap_case(void *data)
{
    nxt_uint_t                i;
    nxt_port_queue_t          *q;
    nxt_nncq_atomic_t         pos;

    static const struct {
        nxt_nncq_atomic_t     head;
        nxt_nncq_atomic_t     tail;
        nxt_bool_t            taken;
    } heads[] = {
        { 0,                      1,                      0 },
        { 1,                      1,                      1 },
        { 1,                      NXT_PORT_QUEUE_SIZE + 1, 1 },
        { 0x7fffffff,             0x80000007,             1 },
        { 0x80000000,             0x80000008,             1 },
        { 0x80000001,             0x80000001,             1 },
        { 0xffff0000,             0xffff0100,             1 },
        { 0xfffff000,             1,                      0 },
        { 0 - NXT_PORT_QUEUE_SIZE + 1u, 1,                0 },
        { 0xffffffff,             1,                      0 },

        /* After a lap: the tail is not past the position. */
        { 0,                      0,                      1 },
        { 0xffffffff,             0,                      1 },
        { 0xffffffff,             0xffffffff,             1 },
        { 0xfffff000,             0,                      1 },
        { 0 - NXT_PORT_QUEUE_SIZE + 1u, 0,                1 },
        { 0 - NXT_PORT_QUEUE_SIZE + 1u,
          0 - NXT_PORT_QUEUE_SIZE + 1u,                   1 },

        /* The residual: the head is behind and the tail is past. */
        { 0xffffffff,             1,                      0 },
        { 0 - NXT_PORT_QUEUE_SIZE + 1u, 1,                0 },

        /* A tail behind the head: the window is the queue size. */
        { 0,                      0xfffffffb,             0 },
        { 1,                      0xfffffffb,             1 },
        { 0 - NXT_PORT_QUEUE_SIZE + 0u, 0,                1 },
    };

    q = calloc(1, sizeof(nxt_port_queue_t));
    if (q == NULL) {
        return 2;
    }

    for (pos = 0xfffffff0; pos != 0x10; pos += 0x10) {

        for (i = 0; i < nxt_nitems(heads); i++) {
            q->queue.head = pos + heads[i].head;
            q->queue.tail = pos + heads[i].tail;

            if (nxt_port_queue_taken(q, pos) != heads[i].taken) {
                printf("unit msg test: head %08x, tail %08x, item %08x: "
                       "taken %d, expected %d\n", (unsigned) q->queue.head,
                       (unsigned) q->queue.tail, (unsigned) pos,
                       (int) !heads[i].taken, (int) heads[i].taken);
                fflush(stdout);

                free(q);
                return 3;
            }
        }
    }

    free(q);

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * A request waits only for its own queue items.  The router has taken the
 * header of the request and waits at the marker of process "Z", which put
 * it in after the header.  The body part of the request finds no free chunk
 * and must go to the socket.  It need not wait for Z: its datagram is read
 * for Z's marker, after the header.  Before, the request waited for the
 * queue tail, which was past Z's marker, and failed after 1 s:
 * "response failed, stage 2".
 */
static int
nxt_unit_msg_test_own_item_case(void *data)
{
    int                        rc;
    char                       seq[16];
    void                       *queue;
    uint8_t                    qmsg[NXT_PORT_QUEUE_MSG_SIZE];
    nxt_unit_msg_test_dgram_t  *d;

    queue = nxt_unit_msg_test_engine_port();
    if (queue == NULL) {
        return 2;
    }

    /* The first response makes the segment. */
    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_ORDER,
                                         NXT_UNIT_MSG_TEST_STREAM - 1, 2)
        != 0)
    {
        return 4;
    }

    while (nxt_port_queue_recv(queue, qmsg) >= 0) { /* void */ }

    nxt_unit_msg_test_stall.queue = queue;
    nxt_unit_msg_test_stall.stage = 0;

    nxt_unit_msg_test_ndgrams = 0;
    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_NO_CHUNK;
    nxt_unit_msg_test_stream = NXT_UNIT_MSG_TEST_STREAM;

    rc = nxt_unit_msg_test_send_request(3, NULL);

    nxt_unit_msg_test_stall.queue = NULL;

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 1
        || !nxt_unit_msg_test_handler_ok)
    {
        printf("unit msg test: response failed, stage %d\n",
               nxt_unit_msg_test_stall.stage);
        fflush(stdout);

        return 4;
    }

    if (nxt_unit_msg_test_stall.stage != 2) {
        printf("unit msg test: stage %d, expected 2\n",
               nxt_unit_msg_test_stall.stage);
        fflush(stdout);

        return 5;
    }

    /* The ack, then the body part, which Z's marker takes. */
    d = &nxt_unit_msg_test_dgrams[1];

    if (nxt_unit_msg_test_ndgrams != 2
        || d->msg.stream != NXT_UNIT_MSG_TEST_STREAM
        || d->msg.type != _NXT_PORT_MSG_DATA || d->msg.last
        || d->size != NXT_UNIT_MSG_TEST_PART_SIZE)
    {
        printf("unit msg test: %d datagrams, expected the ack and the body "
               "part\n", (int) nxt_unit_msg_test_ndgrams);
        fflush(stdout);

        return 6;
    }

    /* Z's datagram comes late, for the marker of the body part. */
    nxt_unit_msg_test_dgram_y();

    if (nxt_unit_msg_test_router_run(queue, NXT_UNIT_MSG_TEST_STREAM,
                                     NXT_UNIT_MSG_TEST_PART_SIZE, 0, 2,
                                     seq, sizeof(seq))
        != 0)
    {
        return 8;
    }

    if (nxt_unit_msg_test_expect(seq, "L") != 0) {
        return 7;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * The queue counters lapped since the header of the request went into the
 * queue, and the head stands one item behind the position of the header
 * while one later item waits.  The body part of the request finds no free
 * chunk and must go to the socket.  The header is not among the items that
 * wait, so the request need not wait: it sends the part at once.  Before,
 * the drain read the header as not taken, as the head was less than the
 * queue size behind it, and failed after 1 s: "response failed, stage 2".
 */
static int
nxt_unit_msg_test_lap_case(void *data)
{
    int                        rc;
    char                       seq[16];
    void                       *queue;
    uint8_t                    qmsg[NXT_PORT_QUEUE_MSG_SIZE];
    nxt_nncq_t                 *q;
    nxt_unit_msg_test_dgram_t  *d;

    queue = nxt_unit_msg_test_engine_port();
    if (queue == NULL) {
        return 2;
    }

    /* The first response makes the segment. */
    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_ORDER,
                                         NXT_UNIT_MSG_TEST_STREAM - 1, 2)
        != 0)
    {
        return 4;
    }

    while (nxt_port_queue_recv(queue, qmsg) >= 0) { /* void */ }

    nxt_unit_msg_test_stall.queue = queue;
    nxt_unit_msg_test_stall.stage = 0;
    nxt_unit_msg_test_stall.lap = 1;

    nxt_unit_msg_test_ndgrams = 0;
    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_NO_CHUNK;
    nxt_unit_msg_test_stream = NXT_UNIT_MSG_TEST_STREAM;

    rc = nxt_unit_msg_test_send_request(3, NULL);

    nxt_unit_msg_test_stall.queue = NULL;

    if (nxt_unit_msg_test_stall.stage != 2) {
        printf("unit msg test: stage %d, expected 2\n",
               nxt_unit_msg_test_stall.stage);
        fflush(stdout);

        return 5;
    }

    /* The head is where the router left it; the later items follow. */
    q = &((nxt_port_queue_t *) queue)->queue;
    q->head = nxt_unit_msg_test_stall.head;

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 1
        || !nxt_unit_msg_test_handler_ok)
    {
        printf("unit msg test: response failed, stage %d\n",
               nxt_unit_msg_test_stall.stage);
        fflush(stdout);

        return 4;
    }

    /* The ack, then the body part. */
    d = &nxt_unit_msg_test_dgrams[1];

    if (nxt_unit_msg_test_ndgrams != 2
        || d->msg.stream != NXT_UNIT_MSG_TEST_STREAM
        || d->msg.type != _NXT_PORT_MSG_DATA || d->msg.last
        || d->size != NXT_UNIT_MSG_TEST_PART_SIZE)
    {
        printf("unit msg test: %d datagrams, expected the ack and the body "
               "part\n", (int) nxt_unit_msg_test_ndgrams);
        fflush(stdout);

        return 6;
    }

    /* The marker of the body part, then the last message. */
    if (nxt_unit_msg_test_router_run(queue, NXT_UNIT_MSG_TEST_STREAM,
                                     NXT_UNIT_MSG_TEST_PART_SIZE, 0, 1,
                                     seq, sizeof(seq))
        != 0)
    {
        return 8;
    }

    if (nxt_unit_msg_test_expect(seq, "BL") != 0) {
        return 7;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * Process "Z" put a marker into the queue and died before its datagram, so
 * the router waits there.  Its next datagram is taken one marker too early
 * for good.  The header of the request is in the queue, and its body part
 * finds no free chunk, so the drain fails after NXT_UNIT_DRAIN_TIMEOUT.
 * The request must still end at the router without the datagram of a later
 * request: its error goes to the socket, with a marker, and is read for the
 * marker of the ack.  The header comes after the error, and the router drops
 * it.  Nothing follows the error.  Before, the error went into the queue
 * behind the header, and the router got only "A" until a later request.
 */
static int
nxt_unit_msg_test_drain_timeout_case(void *data)
{
    int      rc;
    char     seq[16];
    void     *queue;
    size_t   i, ndgrams;
    uint8_t  qmsg[NXT_PORT_QUEUE_MSG_SIZE];

    queue = nxt_unit_msg_test_engine_port();
    if (queue == NULL) {
        return 2;
    }

    /* The first response makes the segment. */
    if (nxt_unit_msg_test_engine_request(NXT_UNIT_MSG_TEST_ORDER,
                                         NXT_UNIT_MSG_TEST_STREAM - 1, 2)
        != 0)
    {
        return 4;
    }

    while (nxt_port_queue_recv(queue, qmsg) >= 0) { /* void */ }

    if (nxt_unit_msg_test_marker_y(queue) != 0) {
        return 3;
    }

    nxt_unit_msg_test_ndgrams = 0;
    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_NO_CHUNK;
    nxt_unit_msg_test_stream = NXT_UNIT_MSG_TEST_STREAM;

    if (nxt_unit_msg_test_send_request(3, NULL) != NXT_UNIT_OK
        || nxt_unit_msg_test_handler_calls != 1)
    {
        return 4;
    }

    if (nxt_unit_msg_test_handler_ok) {
        printf("unit msg test: the write did not fail\n");
        fflush(stdout);

        return 6;
    }

    rc = nxt_unit_msg_test_router_run(queue, NXT_UNIT_MSG_TEST_STREAM,
                                      NXT_UNIT_MSG_TEST_PART_SIZE, 0, 0,
                                      seq, sizeof(seq));

    /* The marker of the error waits for the datagram that Z owes. */
    if (rc != 1) {
        printf("unit msg test: the router read %d, expected a marker "
               "without a datagram\n", rc);
        fflush(stdout);

        return 5;
    }

    if (nxt_unit_msg_test_expect(seq, "AEH") != 0) {
        return 7;
    }

    ndgrams = 0;

    for (i = 0; i < nxt_unit_msg_test_ndgrams; i++) {
        ndgrams += (nxt_unit_msg_test_dgrams[i].msg.stream
                    == NXT_UNIT_MSG_TEST_STREAM);
    }

    /* What is left in the queue behind the marker of the error. */
    if (nxt_unit_msg_test_router_run(queue, NXT_UNIT_MSG_TEST_STREAM,
                                     NXT_UNIT_MSG_TEST_PART_SIZE, 0,
                                     nxt_unit_msg_test_ndgrams,
                                     seq, sizeof(seq))
        != 0)
    {
        return 8;
    }

    if (ndgrams != 2 || seq[0] != '\0') {
        printf("unit msg test: %d datagrams and \"%s\" after the error, "
               "expected 2 and nothing\n", (int) ndgrams, seq);
        fflush(stdout);

        return 9;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/* A request upgraded to a websocket, then one frame in shared memory. */
static int
nxt_unit_msg_test_websocket_case(void *data)
{
    int           rc, whole;
    u_char        *p;

    static const u_char  mask[4] = { 0x10, 0x20, 0x30, 0x40 };

    whole = *(const int *) data;

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_UPGRADE;

    rc = nxt_unit_msg_test_send_request(2, nxt_unit_msg_test_edit_handshake);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 1
        || !nxt_unit_msg_test_handler_ok)
    {
        printf("unit msg test: upgrade failed: %d\n", rc);
        return 1;
    }

    /* A masked text frame with a 2-byte extended length: "abc". */
    p = nxt_port_mmap_chunk_start(nxt_unit_msg_test_seg0, 3);

    p[0] = 0x81;
    p[1] = 0x80 | 126;
    p[2] = 0;
    p[3] = 3;
    memcpy(p + 4, mask, 4);
    p[8] = 'a' ^ mask[0];
    p[9] = 'b' ^ mask[1];
    p[10] = 'c' ^ mask[2];

    nxt_unit_msg_test_ws_calls = 0;
    nxt_unit_msg_test_ws_ok = 0;

    rc = nxt_unit_msg_test_send_chunk(_NXT_PORT_MSG_WEBSOCKET, 0, 3,
                                      whole ? 11 : 2);

    if (whole) {
        if (rc != NXT_UNIT_OK || nxt_unit_msg_test_ws_calls != 1
            || !nxt_unit_msg_test_ws_ok)
        {
            printf("unit msg test: frame: rc %d, handler %d ok %d\n", rc,
                   nxt_unit_msg_test_ws_calls, nxt_unit_msg_test_ws_ok);
            return 2;
        }

    } else {
        /* Two bytes of a header that says it is eight. */
        if (rc != NXT_UNIT_ERROR || nxt_unit_msg_test_ws_calls != 0) {
            printf("unit msg test: short frame: rc %d, handler %d\n", rc,
                   nxt_unit_msg_test_ws_calls);
            return 3;
        }
    }

    nxt_unit_msg_test_close_calls = 0;

    rc = nxt_unit_msg_test_send_chunk(_NXT_PORT_MSG_WEBSOCKET, 1, 3, 0);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_close_calls != 1) {
        printf("unit msg test: close: rc %d, handler %d\n", rc,
               nxt_unit_msg_test_close_calls);
        return 4;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/* A masked one-byte text frame, as the router puts it into a chunk. */
static void
nxt_unit_msg_test_write_frame(u_char *p, u_char c)
{
    static const u_char  mask[4] = { 0x10, 0x20, 0x30, 0x40 };

    p[0] = 0x81;
    p[1] = 0x80 | 1;
    memcpy(p + 2, mask, 4);
    p[6] = c ^ mask[0];
}


/*
 * A websocket frame message with one record over "chunk" of "mmap_id".  With
 * "shared", it is processed at once, as from the shared port.
 */
static int
nxt_unit_msg_test_send_frame_via(uint32_t mmap_id, nxt_chunk_id_t chunk,
    int shared)
{
    u_char               buf[sizeof(nxt_port_msg_t)
                             + sizeof(nxt_port_mmap_msg_t)];
    nxt_port_msg_t       msg;
    nxt_port_mmap_msg_t  rec;

    memset(&msg, 0, sizeof(msg));

    msg.stream = NXT_UNIT_MSG_TEST_STREAM;
    msg.pid = nxt_unit_msg_test_pid;
    msg.reply_port = nxt_unit_msg_test_reply_port;
    msg.type = _NXT_PORT_MSG_WEBSOCKET;
    msg.mmap = 1;

    rec.mmap_id = mmap_id;
    rec.chunk_id = chunk;
    rec.size = 7;

    memcpy(buf, &msg, sizeof(msg));
    memcpy(buf + sizeof(msg), &rec, sizeof(rec));

    if (shared) {
        return nxt_unit_test_process_shared_msg(nxt_unit_msg_test_ctx, buf,
                                                sizeof(buf), -1);
    }

    return nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, buf, sizeof(buf),
                                     -1);
}


static int
nxt_unit_msg_test_send_frame(uint32_t mmap_id, nxt_chunk_id_t chunk)
{
    return nxt_unit_msg_test_send_frame_via(mmap_id, chunk, 0);
}


/*
 * The router puts frames "a" and "c" into segment 1, which libunit has not
 * mapped yet, and frame "b" between them into segment 0.  libunit parks
 * "a" and asks for segment 1.  It processed "b" at once, ahead of "a", and
 * parked "c" behind "a".  When the segment came, it replayed the parked
 * frames in reverse.  The application got "bca" and not "abc": in a
 * fragmented message, the FIN fragment came before the frames still
 * parked, and the message was short.
 */
static int
nxt_unit_msg_test_ws_order_case(void *data)
{
    int                     rc, fd;
    u_char                  *p;
    nxt_port_msg_t          msg;
    nxt_port_mmap_header_t  *hdr;

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_UPGRADE;

    rc = nxt_unit_msg_test_send_request(2, nxt_unit_msg_test_edit_handshake);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 1
        || !nxt_unit_msg_test_handler_ok)
    {
        printf("unit msg test: upgrade failed: %d\n", rc);
        return 1;
    }

    fd = nxt_unit_msg_test_shm(PORT_MMAP_SIZE);
    if (fd == -1) {
        return 2;
    }

    hdr = mmap(NULL, PORT_MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               0);
    if (hdr == MAP_FAILED) {
        perror("mmap");
        return 2;
    }

    hdr->id = 1;
    hdr->src_pid = getpid();
    hdr->dst_pid = getpid();

    nxt_unit_msg_test_write_frame(nxt_port_mmap_chunk_start(hdr, 0), 'a');
    nxt_unit_msg_test_write_frame(nxt_port_mmap_chunk_start(hdr, 1), 'c');

    p = nxt_port_mmap_chunk_start(nxt_unit_msg_test_seg0, 3);
    nxt_unit_msg_test_write_frame(p, 'b');

    nxt_unit_msg_test_ws_norder = 0;

    if (nxt_unit_msg_test_send_frame(1, 0) == NXT_UNIT_ERROR
        || nxt_unit_msg_test_send_frame(0, 3) == NXT_UNIT_ERROR
        || nxt_unit_msg_test_send_frame(1, 1) == NXT_UNIT_ERROR)
    {
        return 3;
    }

    /* The router's answer to the GET_MMAP. */
    memset(&msg, 0, sizeof(msg));

    msg.pid = getpid();
    msg.type = _NXT_PORT_MSG_MMAP;
    msg.last = 1;

    rc = nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, &msg, sizeof(msg),
                                   fd);

    nxt_unit_msg_test_ws_order[nxt_unit_msg_test_ws_norder] = '\0';

    if (rc != NXT_UNIT_OK
        || strcmp(nxt_unit_msg_test_ws_order, "abc") != 0)
    {
        printf("unit msg test: frames came as \"%s\", expected \"abc\": "
               "rc %d\n", nxt_unit_msg_test_ws_order, rc);

        /* The child ends with _exit(), which does not flush stdout. */
        fflush(stdout);

        return 4;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


/*
 * nxt_unit_run_shared() and nxt_unit_dequeue_request() process a message
 * at once, so a context can have more than one buffer parked on a segment.
 * Here frames "a" and "b" both wait for segment 1.  nxt_unit_unpark_rbufs()
 * put each one at the head of pending_rbuf, so they were replayed as "ba".
 * It now walks from the tail, and they keep their order.
 */
static int
nxt_unit_msg_test_ws_unpark_case(void *data)
{
    int                     rc, fd;
    nxt_port_msg_t          msg;
    nxt_port_mmap_header_t  *hdr;

    nxt_unit_msg_test_mode = NXT_UNIT_MSG_TEST_UPGRADE;

    rc = nxt_unit_msg_test_send_request(2, nxt_unit_msg_test_edit_handshake);

    if (rc != NXT_UNIT_OK || nxt_unit_msg_test_handler_calls != 1
        || !nxt_unit_msg_test_handler_ok)
    {
        printf("unit msg test: upgrade failed: %d\n", rc);
        return 1;
    }

    fd = nxt_unit_msg_test_shm(PORT_MMAP_SIZE);
    if (fd == -1) {
        return 2;
    }

    hdr = mmap(NULL, PORT_MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               0);
    if (hdr == MAP_FAILED) {
        perror("mmap");
        return 2;
    }

    hdr->id = 1;
    hdr->src_pid = getpid();
    hdr->dst_pid = getpid();

    nxt_unit_msg_test_write_frame(nxt_port_mmap_chunk_start(hdr, 0), 'a');
    nxt_unit_msg_test_write_frame(nxt_port_mmap_chunk_start(hdr, 1), 'b');

    nxt_unit_msg_test_ws_norder = 0;

    if (nxt_unit_msg_test_send_frame_via(1, 0, 1) == NXT_UNIT_ERROR
        || nxt_unit_msg_test_send_frame_via(1, 1, 1) == NXT_UNIT_ERROR)
    {
        return 3;
    }

    if (nxt_unit_msg_test_ws_norder != 0) {
        printf("unit msg test: a frame was processed before its segment\n");
        fflush(stdout);

        return 3;
    }

    /* The router's answer to the GET_MMAP. */
    memset(&msg, 0, sizeof(msg));

    msg.pid = getpid();
    msg.type = _NXT_PORT_MSG_MMAP;
    msg.last = 1;

    rc = nxt_unit_test_process_msg(nxt_unit_msg_test_ctx, &msg, sizeof(msg),
                                   fd);

    nxt_unit_msg_test_ws_order[nxt_unit_msg_test_ws_norder] = '\0';

    if (rc != NXT_UNIT_OK
        || strcmp(nxt_unit_msg_test_ws_order, "ab") != 0)
    {
        printf("unit msg test: parked frames came as \"%s\", expected "
               "\"ab\": rc %d\n", nxt_unit_msg_test_ws_order, rc);

        /* The child ends with _exit(), which does not flush stdout. */
        fflush(stdout);

        return 4;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


#define NXT_UNIT_MSG_TEST_NWAIT  8


/*
 * While nxt_unit_wait_shm_ack() waits for SHM_ACK, it reads on and puts
 * every other message into pending_rbuf.  Each one took a whole read
 * buffer, over 16 KiB, for a message of 20 bytes.  Now each one is a
 * shrunk copy.
 */
static int
nxt_unit_msg_test_shm_ack_case(void *data)
{
    int              notify;
    void             *queue;
    size_t           i, held;
    nxt_unit_port_t  *port;

    struct {
        nxt_port_msg_t  msg;
        uint32_t        payload;
    } m;

    port = nxt_unit_test_ctx_read_port(nxt_unit_msg_test_ctx);
    queue = nxt_unit_test_port_queue(port);

    if (queue == NULL) {
        printf("unit msg test: the read port has no queue\n");
        return 1;
    }

    memset(&m, 0, sizeof(m));

    m.msg.stream = NXT_UNIT_MSG_TEST_STREAM;
    m.msg.pid = getpid();
    m.msg.type = _NXT_PORT_MSG_DATA;

    for (i = 0; i < NXT_UNIT_MSG_TEST_NWAIT; i++) {
        if (nxt_port_queue_send(queue, &m, sizeof(m), &notify) != NXT_OK) {
            return 2;
        }
    }

    m.msg.type = _NXT_PORT_MSG_SHM_ACK;

    if (nxt_port_queue_send(queue, &m.msg, sizeof(m.msg), &notify) != NXT_OK) {
        return 2;
    }

    if (nxt_unit_test_wait_shm_ack(nxt_unit_msg_test_ctx, &held)
        != NXT_UNIT_OK)
    {
        return 3;
    }

    if (held == 0 || held >= NXT_UNIT_MSG_TEST_NWAIT * 1024) {
        printf("unit msg test: %d messages of %d bytes wait in %d bytes\n",
               NXT_UNIT_MSG_TEST_NWAIT, (int) sizeof(m), (int) held);

        /* The child ends with _exit(), which does not flush stdout. */
        fflush(stdout);

        return 4;
    }

    return NXT_UNIT_MSG_TEST_RC(NXT_UNIT_OK);
}


int
main(void)
{
    int                  rc, ready[2], router[2], read[2], shared[2];
    char                 name[64];
    size_t               tail, i;
    nxt_unit_init_t      init;
    nxt_port_mmap_msg_t  rec[2];

    static const int  whole_frame = 1, short_frame = 0;
    static const int  get_port_fails = 1, process_not_found = 0;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, ready) == -1
        || socketpair(AF_UNIX, SOCK_DGRAM, 0, router) == -1
        || socketpair(AF_UNIX, SOCK_DGRAM, 0, read) == -1
        || socketpair(AF_UNIX, SOCK_DGRAM, 0, shared) == -1)
    {
        perror("socketpair");
        return 1;
    }

    nxt_unit_msg_test_pid = getpid();

    memset(&init, 0, sizeof(init));

    init.callbacks.request_handler = nxt_unit_msg_test_handler;
    init.callbacks.websocket_handler = nxt_unit_msg_test_websocket;
    init.callbacks.close_handler = nxt_unit_msg_test_close;
    init.callbacks.port_send = nxt_unit_msg_test_send;
    init.callbacks.port_recv = nxt_unit_msg_test_recv;
    init.callbacks.quit = nxt_unit_msg_test_quit;

    init.ready_port.id.pid = getpid();
    init.ready_port.id.id = 1;
    init.ready_port.in_fd = -1;
    init.ready_port.out_fd = ready[0];

    init.ready_stream = 1;

    init.router_port.id.pid = getpid();
    init.router_port.id.id = NXT_UNIT_MSG_TEST_ROUTER_PORT;
    init.router_port.in_fd = -1;
    init.router_port.out_fd = router[0];

    init.read_port.id.pid = getpid();
    init.read_port.id.id = 3;
    init.read_port.in_fd = read[0];
    init.read_port.out_fd = read[1];

    nxt_unit_msg_test_read_fds[0] = read[0];
    nxt_unit_msg_test_read_fds[1] = read[1];

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

    if (getenv("GROK713_ONLY") != NULL) {
        rc = nxt_unit_msg_test_send_segment(PORT_MMAP_SIZE, 0);
        nxt_unit_msg_test_in_child("grok713 full-socket wake does not block",
                                   nxt_unit_msg_test_full_wake_case, NULL,
                                   NXT_UNIT_OK);
        nxt_unit_msg_test_in_child("grok713 QUIT over a full socket does not "
                                   "block and is not lost",
                                   nxt_unit_msg_test_quit_full_case, NULL,
                                   NXT_UNIT_OK);
        nxt_unit_msg_test_in_child("grok713 request with a body during a "
                                   "pending quit completes",
                                   nxt_unit_msg_test_quit_pending_body_case,
                                   NULL, NXT_UNIT_OK);
        return nxt_unit_msg_test_failures != 0;
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

    /*
     * Capped at NXT_PORT_MMAPS_MAX, not by the shm limit: the router's
     * outgoing segments are not bounded by it (#172).
     */

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

    nxt_unit_msg_test_in_child("segment lists stay linked when the array "
                               "grows", nxt_unit_msg_test_mmaps_grow_case,
                               NULL, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a freed context leaves no read buffer "
                               "waiting", nxt_unit_msg_test_ctx_free_case,
                               NULL, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a freed context hands back its segment "
                               "request",
                               nxt_unit_msg_test_ctx_free_asker_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a QUIT over a full socket does not block and "
                               "is not lost",
                               nxt_unit_msg_test_quit_full_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a request with a body during a pending quit "
                               "completes",
                               nxt_unit_msg_test_quit_pending_body_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a request body split across content_buf and "
                               "content_fd is read without overflow",
                               nxt_unit_msg_test_split_body_fd_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a content_fd read error after preread bytes "
                               "returns the preread bytes",
                               nxt_unit_msg_test_split_body_fd_ebadf_case,
                               NULL, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("duplicate segment id does not leak a mapping",
                               nxt_unit_msg_test_dup_id_case, NULL,
                               NXT_UNIT_OK);

    for (i = 0; i < nxt_nitems(requests); i++) {
        nxt_unit_msg_test_in_child(requests[i].name,
                                   nxt_unit_msg_test_request_case,
                                   (void *) &requests[i], requests[i].expect);
    }

    for (i = 0; i < nxt_nitems(unacked); i++) {
        nxt_unit_msg_test_in_child(unacked[i].name,
                                   nxt_unit_msg_test_unacked_case,
                                   (void *) &unacked[i], NXT_UNIT_OK);
    }

    nxt_unit_msg_test_in_child("request from an unknown process is released",
                               nxt_unit_msg_test_no_port_case,
                               (void *) &process_not_found, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("request whose GET_PORT failed is released",
                               nxt_unit_msg_test_no_port_case,
                               (void *) &get_port_fails, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("port whose GET_PORT failed is asked for "
                               "again", nxt_unit_msg_test_get_port_again_case,
                               NULL, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("request that fails while its port is not "
                               "ready is reported as not ready",
                               nxt_unit_msg_test_port_not_ready_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("preread offset changed after the check is "
                               "not followed",
                               nxt_unit_msg_test_preread_race_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("response messages keep their order through "
                               "the queue and the socket",
                               nxt_unit_msg_test_order_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a small response sends one datagram",
                               nxt_unit_msg_test_small_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a small response carries its body in one "
                               "message",
                               nxt_unit_msg_test_exact_fields_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("without shared memory a response keeps its "
                               "order on the socket",
                               nxt_unit_msg_test_no_shm_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a plain buffer keeps its request on the "
                               "socket", nxt_unit_msg_test_plain_held_case,
                               NULL, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a websocket stays on the socket",
                               nxt_unit_msg_test_ws_socket_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a request leaves the queue after the router "
                               "took it", nxt_unit_msg_test_drain_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("the drain check holds across the queue "
                               "counter wrap",
                               nxt_unit_msg_test_queue_wrap_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a request waits only for its own queue "
                               "messages", nxt_unit_msg_test_own_item_case,
                               NULL, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a lapped queue item does not stall the "
                               "drain", nxt_unit_msg_test_lap_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("a request whose drain fails ends with an "
                               "error on the socket",
                               nxt_unit_msg_test_drain_timeout_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("masked websocket frame in shared memory",
                               nxt_unit_msg_test_websocket_case,
                               (void *) &whole_frame, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("truncated websocket frame is refused",
                               nxt_unit_msg_test_websocket_case,
                               (void *) &short_frame, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("websocket frames keep their order across "
                               "a segment wait",
                               nxt_unit_msg_test_ws_order_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("parked websocket frames keep their order "
                               "when the segment comes",
                               nxt_unit_msg_test_ws_unpark_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("messages read while waiting for SHM_ACK "
                               "wait shrunk",
                               nxt_unit_msg_test_shm_ack_case, NULL,
                               NXT_UNIT_OK);

    /*
     * Release the library.  Otherwise the global context pointer keeps it
     * reachable at exit, and LeakSanitizer does not see a leak of anything
     * the library owns.
     */
    nxt_unit_done(nxt_unit_msg_test_ctx);

    if (nxt_unit_msg_test_failures != 0) {
        printf("unit msg test: %d failure(s)\n", nxt_unit_msg_test_failures);
        return 1;
    }

    printf("unit msg test: all passed\n");

    return 0;
}
