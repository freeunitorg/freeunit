
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Malformed port messages at libunit's receive side (src/nxt_unit.c), fed
 * through nxt_unit_test_process_msg().  nxt_unit_mmap_read() used to read a
 * partial last record past the message, mmap_id 0xFFFFFFFF wrapped the
 * lib->incoming index, and a segment of any size was accepted.  Cases that
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
#include "nxt_unit_websocket.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>


/* What the request handler does with a request that arrived. */
typedef enum {
    NXT_UNIT_MSG_TEST_READ_BODY = 0,
    NXT_UNIT_MSG_TEST_GROUP_DUP,
    NXT_UNIT_MSG_TEST_RAISE_COUNT,
    NXT_UNIT_MSG_TEST_UPGRADE,
} nxt_unit_msg_test_mode_t;


#define NXT_UNIT_MSG_TEST_ROUTER_PORT  2
#define NXT_UNIT_MSG_TEST_STREAM       7
#define NXT_UNIT_MSG_TEST_BODY         "ab"


static int                      nxt_unit_msg_test_failures;
static int                      nxt_unit_msg_test_send_fails;
static int                      nxt_unit_msg_test_quit_called;
static int                      nxt_unit_msg_test_handler_calls;
static int                      nxt_unit_msg_test_handler_ok;
static int                      nxt_unit_msg_test_ws_calls;
static int                      nxt_unit_msg_test_ws_ok;
static int                      nxt_unit_msg_test_close_calls;
static nxt_unit_msg_test_mode_t nxt_unit_msg_test_mode;
static pid_t                    nxt_unit_msg_test_pid;
static nxt_unit_ctx_t           *nxt_unit_msg_test_ctx;
static nxt_unit_ctx_t           *nxt_unit_msg_test_follower;
static nxt_port_mmap_header_t   *nxt_unit_msg_test_seg0;

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
    }

    nxt_unit_request_done(req, NXT_UNIT_ERROR);
}


static void
nxt_unit_msg_test_websocket(nxt_unit_websocket_frame_t *ws)
{
    char     buf[16];
    ssize_t  n;

    nxt_unit_msg_test_ws_calls++;

    n = nxt_unit_websocket_read(ws, buf, sizeof(buf));

    nxt_unit_msg_test_ws_ok = ws->payload_len == 3 && n == 3
                              && memcmp(buf, "abc", 3) == 0;

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
    msg.stream = NXT_UNIT_MSG_TEST_STREAM;
    msg.pid = nxt_unit_msg_test_pid;
    msg.reply_port = NXT_UNIT_MSG_TEST_ROUTER_PORT;
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

    return nxt_unit_msg_test_send_chunk(_NXT_PORT_MSG_REQ_HEADERS, 1, chunk,
                                        size);
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
} nxt_unit_msg_test_request_t;


static const nxt_unit_msg_test_request_t  requests[] = {
    { "well-formed request reaches the handler", NULL,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_OK, 1 },
    { "target sptr out of buffer is refused", nxt_unit_msg_test_edit_target,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0 },
    { "preread sptr out of buffer is refused", nxt_unit_msg_test_edit_preread,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0 },
    { "fields_count past the buffer is refused", nxt_unit_msg_test_edit_count,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0 },
    { "cached field index out of range is refused",
      nxt_unit_msg_test_edit_index,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0 },
    { "field name sptr out of buffer is refused",
      nxt_unit_msg_test_edit_field_name,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0 },
    { "field name inside fields[] is refused",
      nxt_unit_msg_test_edit_field_inside,
      NXT_UNIT_MSG_TEST_READ_BODY, 2, NXT_UNIT_ERROR, 0 },
    { "duplicate fields are grouped", NULL,
      NXT_UNIT_MSG_TEST_GROUP_DUP, 2, NXT_UNIT_OK, 1 },
    { "fields_count raised after the check stays in the buffer", NULL,
      NXT_UNIT_MSG_TEST_RAISE_COUNT, PORT_MMAP_CHUNK_COUNT - 1, NXT_UNIT_OK,
      1 },
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

    return NXT_UNIT_MSG_TEST_RC(rc);
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


int
main(void)
{
    int                  rc, ready[2], router[2], read[2], shared[2];
    char                 name[64];
    size_t               tail, i;
    nxt_unit_init_t      init;
    nxt_port_mmap_msg_t  rec[2];

    static const int  whole_frame = 1, short_frame = 0;

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

    for (i = 0; i < nxt_nitems(requests); i++) {
        nxt_unit_msg_test_in_child(requests[i].name,
                                   nxt_unit_msg_test_request_case,
                                   (void *) &requests[i], requests[i].expect);
    }

    nxt_unit_msg_test_in_child("preread offset changed after the check is "
                               "not followed",
                               nxt_unit_msg_test_preread_race_case, NULL,
                               NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("masked websocket frame in shared memory",
                               nxt_unit_msg_test_websocket_case,
                               (void *) &whole_frame, NXT_UNIT_OK);

    nxt_unit_msg_test_in_child("truncated websocket frame is refused",
                               nxt_unit_msg_test_websocket_case,
                               (void *) &short_frame, NXT_UNIT_OK);

    if (nxt_unit_msg_test_failures != 0) {
        printf("unit msg test: %d failure(s)\n", nxt_unit_msg_test_failures);
        return 1;
    }

    printf("unit msg test: all passed\n");

    return 0;
}
