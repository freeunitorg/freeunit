/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * libFuzzer target for libunit's receive side: one port message, as the
 * router would send it, through nxt_unit_process_msg() -- the mmap record
 * walk (nxt_unit_mmap_read()), request arrival validation
 * (nxt_unit_process_req_headers(), nxt_unit_sptr_in_buf()), request body
 * and websocket frames -- and, for a request that comes out ready, the
 * application's request handler, which reads every field and finishes it.
 *
 * Input layout:
 *
 *   [0..1]      little-endian length L of the port message (capped at the
 *               16 KB read buffer)
 *   [2..2+L)    the port message: nxt_port_msg_t, then its payload -- for
 *               an mmap message an array of nxt_port_mmap_msg_t
 *   [2+L..)     the shared memory payload (up to 1 MB), placed in chunks
 *               of segment 0 that the records then point into
 *
 * The trust model is libunit's: the router is the peer.  The harness plays
 * a correct router for everything that is its job rather than the
 * message's, and leaves the message itself to the fuzzer:
 *
 *   - Segment 0 is a real shared memory segment, handed to libunit the way
 *     the router hands one over.  The payload goes into chunks the harness
 *     allocates in the segment's free map, as the router does, and a record
 *     whose (chunk_id, size) is in range is moved into that allocation:
 *     without that, the next input overwrites chunks a request that is
 *     still waiting for its body holds -- memory the router would never
 *     reuse -- and the "finding" is the harness's own.  Out-of-range
 *     records are left alone, to exercise the range check.
 *   - Every whole record's mmap_id is forced to 0 unless it is 0xFFFFFFFF.
 *     Any other id names a segment libunit does not have, and libunit then
 *     (correctly) parks the message and asks the router for it; parked
 *     messages would pile up across runs.  The id is not capped by design
 *     (#172), so an id near 2^32 would also grow the segment array to that
 *     size -- an allocation, not a finding.
 *   - QUIT and REMOVE_PID are skipped: they tear down state every later run
 *     depends on.
 *   - The sender pid and reply port are set to the router port libunit was
 *     started with, so that a request can complete instead of waiting for a
 *     port that never arrives.
 *
 * Requests that wait for a body keep their chunks, and nothing may ever
 * send that body; when the segment runs out, the whole libunit instance is
 * torn down (which finishes those requests) and set up again.
 *
 * Built only with --fuzz, which also compiles in the
 * nxt_unit_test_process_msg() hook (NXT_FUZZ_BUILD).
 */

#include <nxt_main.h>
#include <nxt_port_memory_int.h>
#include <nxt_app_queue.h>
#include <nxt_unit.h>
#include <nxt_unit_request.h>

#include <sys/mman.h>
#include <sys/syscall.h>


#define NXT_UNIT_MSG_FUZZ_MSG_MAX      16384
#define NXT_UNIT_MSG_FUZZ_DATA_MAX     (1024 * 1024)
#define NXT_UNIT_MSG_FUZZ_ROUTER_PORT  2


extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);


static nxt_unit_ctx_t          *nxt_unit_msg_fuzz_ctx;
static nxt_port_mmap_header_t  *nxt_unit_msg_fuzz_hdr;
static int                     nxt_unit_msg_fuzz_fds[3] = { -1, -1, -1 };
static int                     nxt_unit_msg_fuzz_null = -1;
static volatile uint64_t       nxt_unit_msg_fuzz_sink;


static u_char
nxt_unit_msg_fuzz_last(nxt_unit_sptr_t *sptr, uint32_t length)
{
    return length == 0 ? 0
                       : ((u_char *) nxt_unit_sptr_get(sptr))[length - 1];
}


static void
nxt_unit_msg_fuzz_handler(nxt_unit_request_info_t *req)
{
    uint32_t            i;
    uint64_t            sum;
    nxt_unit_field_t    *f;
    nxt_unit_request_t  *r;

    /* As the Python and Java modules do before reading the fields. */
    nxt_unit_request_group_dup_fields(req);

    r = req->request;
    sum = 0;

    /*
     * Touch what an application reads: the last byte of every string the
     * arrival checks vetted, at the length they vetted.  The request lives
     * in the shared segment, which ASan does not shadow, so what this
     * catches is a read that leaves the mapping altogether.
     */
    sum += nxt_unit_msg_fuzz_last(&r->method, r->method_length);
    sum += nxt_unit_msg_fuzz_last(&r->version, r->version_length);
    sum += nxt_unit_msg_fuzz_last(&r->remote, r->remote_length);
    sum += nxt_unit_msg_fuzz_last(&r->local_addr, r->local_addr_length);
    sum += nxt_unit_msg_fuzz_last(&r->local_port, r->local_port_length);
    sum += nxt_unit_msg_fuzz_last(&r->server_name, r->server_name_length);
    sum += nxt_unit_msg_fuzz_last(&r->target, r->target_length);
    sum += nxt_unit_msg_fuzz_last(&r->path, r->path_length);
    sum += nxt_unit_msg_fuzz_last(&r->query, r->query_length);

    for (i = 0; i < r->fields_count; i++) {
        f = &r->fields[i];

        sum += nxt_unit_msg_fuzz_last(&f->name, f->name_length);
        sum += nxt_unit_msg_fuzz_last(&f->value, f->value_length);
    }

    if (r->content_length_field != NXT_UNIT_NONE_FIELD) {
        sum += r->fields[r->content_length_field].hash;
    }

    if (r->content_type_field != NXT_UNIT_NONE_FIELD) {
        sum += r->fields[r->content_type_field].hash;
    }

    if (r->cookie_field != NXT_UNIT_NONE_FIELD) {
        sum += r->fields[r->cookie_field].hash;
    }

    if (r->authorization_field != NXT_UNIT_NONE_FIELD) {
        sum += r->fields[r->authorization_field].hash;
    }

    nxt_unit_msg_fuzz_sink = sum;

    nxt_unit_request_done(req, NXT_UNIT_ERROR);
}


static ssize_t
nxt_unit_msg_fuzz_send(nxt_unit_ctx_t *ctx, nxt_unit_port_t *port,
    const void *buf, size_t buf_size, const void *oob, size_t oob_size)
{
    return buf_size;
}


static ssize_t
nxt_unit_msg_fuzz_recv(nxt_unit_ctx_t *ctx, nxt_unit_port_t *port, void *buf,
    size_t buf_size, void *oob, size_t *oob_size)
{
    *oob_size = 0;

    return 0;
}


static int
nxt_unit_msg_fuzz_shm(size_t size)
{
    int  fd;

    fd = syscall(SYS_memfd_create, "unit.msg.fuzz", 0);
    if (fd == -1) {
        return -1;
    }

    if (ftruncate(fd, size) == -1) {
        close(fd);
        return -1;
    }

    return fd;
}


/* A libunit instance with segment 0 handed over, as a router would. */
static int
nxt_unit_msg_fuzz_setup(void)
{
    int                     fd, ready[2], router[2], read[2], shared[2];
    nxt_port_msg_t          msg;
    nxt_unit_init_t         init;
    nxt_port_mmap_header_t  *hdr;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, ready) == -1
        || socketpair(AF_UNIX, SOCK_DGRAM, 0, router) == -1
        || socketpair(AF_UNIX, SOCK_DGRAM, 0, read) == -1
        || socketpair(AF_UNIX, SOCK_DGRAM, 0, shared) == -1)
    {
        return -1;
    }

    /* The peer ends, which libunit is not given and does not close. */
    nxt_unit_msg_fuzz_fds[0] = ready[1];
    nxt_unit_msg_fuzz_fds[1] = router[1];
    nxt_unit_msg_fuzz_fds[2] = shared[1];

    memset(&init, 0, sizeof(init));

    init.callbacks.request_handler = nxt_unit_msg_fuzz_handler;
    init.callbacks.port_send = nxt_unit_msg_fuzz_send;
    init.callbacks.port_recv = nxt_unit_msg_fuzz_recv;

    init.ready_port.id.pid = getpid();
    init.ready_port.id.id = 1;
    init.ready_port.in_fd = -1;
    init.ready_port.out_fd = ready[0];
    init.ready_stream = 1;

    init.router_port.id.pid = getpid();
    init.router_port.id.id = NXT_UNIT_MSG_FUZZ_ROUTER_PORT;
    init.router_port.in_fd = -1;
    init.router_port.out_fd = router[0];

    init.read_port.id.pid = getpid();
    init.read_port.id.id = 3;
    init.read_port.in_fd = read[0];
    init.read_port.out_fd = read[1];

    init.shared_port_fd = shared[0];
    init.shared_queue_fd = nxt_unit_msg_fuzz_shm(sizeof(nxt_app_queue_t));

    /* libunit's own log lines would drown libFuzzer's. */
    init.log_fd = nxt_unit_msg_fuzz_null;

    if (init.shared_queue_fd == -1) {
        return -1;
    }

    nxt_unit_msg_fuzz_ctx = nxt_unit_init(&init);
    if (nxt_unit_msg_fuzz_ctx == NULL) {
        return -1;
    }

    fd = nxt_unit_msg_fuzz_shm(PORT_MMAP_SIZE);
    if (fd == -1) {
        return -1;
    }

    hdr = mmap(NULL, PORT_MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               0);
    if (hdr == MAP_FAILED) {
        return -1;
    }

    /* As nxt_port_new_port_mmap() initialises a segment. */
    memset(hdr->free_map, 0xFF, sizeof(hdr->free_map));
    nxt_port_mmap_set_chunk_busy(hdr->free_map, PORT_MMAP_CHUNK_COUNT);

    hdr->id = 0;
    hdr->src_pid = getpid();
    hdr->dst_pid = getpid();
    hdr->sent_over = 0xFFFFu;

    nxt_unit_msg_fuzz_hdr = hdr;

    memset(&msg, 0, sizeof(msg));

    msg.pid = getpid();
    msg.type = _NXT_PORT_MSG_MMAP;
    msg.last = 1;

    /* libunit maps the segment itself and closes the descriptor. */
    if (nxt_unit_test_process_msg(nxt_unit_msg_fuzz_ctx, &msg, sizeof(msg),
                                  fd)
        != NXT_UNIT_OK)
    {
        return -1;
    }

    return 0;
}


static int
nxt_unit_msg_fuzz_reset(void)
{
    int  i;

    nxt_unit_done(nxt_unit_msg_fuzz_ctx);

    munmap(nxt_unit_msg_fuzz_hdr, PORT_MMAP_SIZE);

    for (i = 0; i < 3; i++) {
        close(nxt_unit_msg_fuzz_fds[i]);
        nxt_unit_msg_fuzz_fds[i] = -1;
    }

    return nxt_unit_msg_fuzz_setup();
}


/* "n" consecutive free chunks, marked busy, as the router allocates. */
static int
nxt_unit_msg_fuzz_alloc(size_t n, nxt_chunk_id_t *base)
{
    nxt_free_map_t  *m;
    nxt_chunk_id_t  c, i;

    m = nxt_unit_msg_fuzz_hdr->free_map;

    for (c = 0; c + n <= PORT_MMAP_CHUNK_COUNT; c += i + 1) {

        for (i = 0; i < n; i++) {
            if ((m[FREE_IDX(c + i)] & FREE_MASK(c + i)) == 0) {
                break;
            }
        }

        if (i == n) {
            for (i = 0; i < n; i++) {
                nxt_port_mmap_set_chunk_busy(m, c + i);
            }

            *base = c;

            return 0;
        }
    }

    return -1;
}


int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
    nxt_unit_msg_fuzz_null = open("/dev/null", O_WRONLY);

    if (nxt_unit_msg_fuzz_null == -1 || nxt_unit_msg_fuzz_setup() != 0) {
        abort();
    }

    return 0;
}


int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    u_char               buf[NXT_UNIT_MSG_FUZZ_MSG_MAX];
    size_t               len, dlen, n, off, nchunks, room;
    nxt_chunk_id_t       base, c;
    nxt_port_msg_t       *msg;
    nxt_port_mmap_msg_t  rec;

    if (size < 2) {
        return 0;
    }

    len = data[0] | (data[1] << 8);
    data += 2;
    size -= 2;

    len = nxt_min(len, nxt_min(size, sizeof(buf)));

    memcpy(buf, data, len);

    if (len < sizeof(nxt_port_msg_t)) {
        /* Still worth sending: the "too small" path. */
        (void) nxt_unit_test_process_msg(nxt_unit_msg_fuzz_ctx, buf, len, -1);
        return 0;
    }

    msg = (nxt_port_msg_t *) buf;

    if (msg->type == _NXT_PORT_MSG_QUIT
        || msg->type == _NXT_PORT_MSG_REMOVE_PID)
    {
        return 0;
    }

    /*
     * The reply goes to the router port libunit was started with; any other
     * (pid, port) is unknown, and a request for it waits for a NEW_PORT
     * that never comes.
     */
    msg->pid = getpid();
    msg->reply_port = NXT_UNIT_MSG_FUZZ_ROUTER_PORT;

    base = 0;
    n = 0;

    if (msg->mmap && len >= sizeof(nxt_port_msg_t)
                              + sizeof(nxt_port_mmap_msg_t))
    {
        dlen = nxt_min(size - len, NXT_UNIT_MSG_FUZZ_DATA_MAX);
        n = (dlen + PORT_MMAP_CHUNK_SIZE - 1) / PORT_MMAP_CHUNK_SIZE;
        n = nxt_max(n, 1);

        if (nxt_unit_msg_fuzz_alloc(n, &base) != 0) {
            if (nxt_unit_msg_fuzz_reset() != 0) {
                abort();
            }

            if (nxt_unit_msg_fuzz_alloc(n, &base) != 0) {
                abort();
            }
        }

        memcpy(nxt_port_mmap_chunk_start(nxt_unit_msg_fuzz_hdr, base),
               data + len, dlen);

        for (off = sizeof(nxt_port_msg_t);
             off + sizeof(nxt_port_mmap_msg_t) <= len;
             off += sizeof(nxt_port_mmap_msg_t))
        {
            memcpy(&rec, buf + off, sizeof(rec));

            if (rec.mmap_id != 0xFFFFFFFF) {
                rec.mmap_id = 0;
            }

            if (nxt_port_mmap_chunk_range_valid(rec.chunk_id, rec.size,
                                                &nchunks))
            {
                c = base + rec.chunk_id % n;
                room = (base + n - c) * PORT_MMAP_CHUNK_SIZE;

                rec.chunk_id = c;
                rec.size = nxt_min(rec.size, room);
            }

            memcpy(buf + off, &rec, sizeof(rec));
        }
    }

    (void) nxt_unit_test_process_msg(nxt_unit_msg_fuzz_ctx, buf, len, -1);

    return 0;
}
