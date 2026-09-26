/*
 * Copyright (C) FreeUnit contributors
 */

#include <nxt_main.h>

/*
 * DO NOT TRY THIS AT HOME!  Like the h1p fuzzers, this #includes the real
 * protocol source so LLVMFuzzerTestOneInput() can reach its static
 * callbacks: nxt_h2p_session_create() wires up the exact same nghttp2
 * callbacks and nghttp2_option_* limits as production
 * (nxt_h2p_conn_init()), and nxt_h2p_conn_read()/nxt_h2p_conn_flush() are
 * the real nghttp2_session_mem_recv()/mem_send() drivers.  Everything this
 * fuzzer finds is a bug in nxt_h2proto.c's callbacks, not in a
 * reimplementation of them.
 */
#include "nxt_h2proto.c"

#include <fcntl.h>
#include <sys/socket.h>


/*
 * The seam: a fake nxt_conn_t stands in for the TLS connection.
 *
 * nxt_h2p_conn_init() is the only public entry point, and it assumes ALPN
 * already negotiated "h2" and expects a live nxt_conn_t wired to a real
 * socket.  Rather than reimplementing (and by construction never quite
 * matching) that setup, this harness builds the same nxt_conn_t the router
 * would hand nxt_h2p_conn_init() -- a real one from nxt_conn_create(), on a
 * real event engine from nxt_event_engine_create(), with a real (but inert)
 * socket -- and then calls the two static functions nxt_h2p_conn_init()
 * itself calls: nxt_h2p_session_create() to build the session with
 * production's callbacks and options, and nxt_h2p_conn_read() to feed it,
 * exactly as a read event would.  Skipped: the TLS handshake and ALPN
 * (c->u.tls stays NULL; nothing here reads it) and the router's request
 * dispatch (r->conf->socket_conf->action stays NULL, so
 * nxt_http_request_action() answers 500 without ever reaching a router
 * action -- router/app dispatch is a different attack surface with its own
 * fuzz target). Everything between those two points -- HPACK, pseudo-header
 * and :authority/host validation, header-list and field-name limits, the
 * request line and target/path normalization, DATA accounting and body
 * limits, RST/GOAWAY/CONTINUATION/SETTINGS limits, stream lifecycle -- is
 * the real production code from nxt_h2proto.c.
 *
 * The socket is real but inert: c->socket.fd is one end of a fresh
 * AF_UNIX SOCK_STREAM socketpair() each iteration (a real socket, so
 * epoll_ctl() and shutdown() -- which the production close chain calls
 * unconditionally -- both work; /dev/null answers ENOTSUP/EPERM to both).
 * nxt_conn_write()/nxt_conn_read() are macros that only enqueue work
 * (nxt_h2p_conn_flush() -> nghttp2_session_mem_send() -> "pull frames into
 * c->write" is production's own drain of mem_send; this harness does not
 * reimplement it); this fuzzer then drains every engine work queue for
 * real, so nxt_conn_io_write() really writev()s the response bytes into the
 * socketpair's other end, which this harness never reads: that end, closed
 * at the end of the iteration along with everything still buffered in it,
 * is the sink the task asks for.  A real fd also means every queued item
 * actually runs (see nxt_h2p_fuzz_drain_engine() below): nothing is left
 * half-processed in the engine's work queues between iterations, which is
 * what a long campaign needs to not slowly leak or jam the engine's
 * work-item cache -- and, by routing every iteration through the real
 * nxt_h2p_closing() -> nxt_h1p_closing() close chain (see
 * LLVMFuzzerTestOneInput() below) rather than a hand-rolled teardown, this
 * harness exercises production's own connection teardown too, instead of
 * skipping it.
 */


#define NXT_H2P_FUZZ_MIN_INPUT   1
#define NXT_H2P_FUZZ_MAX_INPUT   (64 * 1024)

/* RFC 9113, 3.4: the fixed 24-byte client connection preface. */
static const uint8_t  nxt_h2p_fuzz_preface[] = {
    'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2', '.', '0',
    '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n',
};


static nxt_mp_t                *nxt_h2p_fuzz_config_mp;
static nxt_event_engine_t      *nxt_h2p_fuzz_engine;
static nxt_socket_conf_t       nxt_h2p_fuzz_skcf;
static nxt_router_conf_t       nxt_h2p_fuzz_rtcf;
static nxt_socket_conf_joint_t nxt_h2p_fuzz_joint;
static nxt_listen_event_t      nxt_h2p_fuzz_lev;
static nxt_sockaddr_t          *nxt_h2p_fuzz_local;

/*
 * Template for nxt_sockaddr_cache_alloc(), which nxt_h1p_conn_free() ->
 * nxt_sockaddr_cache_free() expects c->remote to have come from: it frees
 * through the engine's mem_cache by cache_hint, not through mp, so
 * c->remote cannot be a plain nxt_sockaddr_create() (its cache_hint is
 * whatever nxt_mp_zalloc() left zeroed, which just aliases mem_cache bucket
 * 0) or a pointer shared across iterations (freed once, then reused as a
 * dangling c->remote next time).  A fresh nxt_sockaddr_cache_alloc() per
 * iteration is both correct and, being the real allocator, exercises the
 * same cache production accept() fills c->remote from.
 */
static nxt_listen_socket_t     nxt_h2p_fuzz_remote_template;

/*
 * Set when c->mem_pool is destroyed, by nxt_h2p_fuzz_conn_gone_handler(), a
 * cleanup on that pool.  h2c and its streams live in c->mem_pool, so once
 * this is set they are freed memory, and c itself may already be on the
 * engine's freelist for the next connection.  It lives outside the pool on
 * purpose: nothing inside the pool can be read to learn that the pool is
 * gone.  Reset at the top of every LLVMFuzzerTestOneInput().
 */
static nxt_bool_t              nxt_h2p_fuzz_conn_gone;


extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
extern char  **environ;


static void
nxt_h2p_fuzz_conn_gone_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_h2p_fuzz_conn_gone = 1;
}


static void
nxt_h2p_fuzz_drain_wq(nxt_work_queue_t *wq)
{
    void                *obj, *data;
    nxt_task_t          *task;
    nxt_work_handler_t  handler;

    while (wq->head != NULL) {
        handler = nxt_work_queue_pop(wq, &task, &obj, &data);
        handler(task, obj, data);
    }
}


/*
 * Fires whatever timer is due right now -- and only that one: nxt_conn_
 * close_handler() arms c->write_timer with a 0ms delay when nxt_fd_event_
 * close() reports the fd's epoll registration is not torn down yet (real
 * with a real socket, once nxt_h2p_conn_read()'s tail nxt_conn_read() has
 * armed it for a future read -- unlike a connection nxt_h2p_conn_leaving()
 * drains at once, which never gets that far).  Without processing this
 * timer, that close never reaches nxt_h1p_conn_free() and the connection
 * -- and the joint reference nxt_h2p_conn_cleanup() would have released --
 * leaks for real.  nxt_timer_find() returns the minimum enabled timer's
 * delay without advancing engine->timers.now; only ever expiring a delay
 * of exactly 0 here means idle_timeout, send_timeout and the progress
 * timer (all armed well in the future) are never force-fired -- this
 * harness still does not drive wall-clock timeouts.
 */

static void
nxt_h2p_fuzz_drain_timers(nxt_event_engine_t *engine)
{
    if (nxt_timer_find(engine) == 0) {
        nxt_timer_expire(engine, engine->timers.now);
    }
}


/*
 * Drains every engine work queue this harness can reach, repeatedly: one
 * pass can requeue onto another queue (a write completion re-arming a read,
 * or nxt_h2p_fuzz_drain_timers() queuing a due timer's handler, say), so
 * this keeps going until a full pass moves nothing.  Capped generously
 * against a harness bug turning into a hang rather than a slow finding;
 * the real state machine converges in a handful of rounds.
 */

static void
nxt_h2p_fuzz_drain_engine(nxt_event_engine_t *engine)
{
    nxt_uint_t  round;
    uint32_t    before, after;

    for (round = 0; round < 64; round++) {
        before = engine->connections + engine->closed_conns_cnt;

        nxt_h2p_fuzz_drain_timers(engine);
        nxt_h2p_fuzz_drain_wq(&engine->fast_work_queue);
        nxt_h2p_fuzz_drain_wq(&engine->read_work_queue);
        nxt_h2p_fuzz_drain_wq(&engine->write_work_queue);
        nxt_h2p_fuzz_drain_wq(&engine->socket_work_queue);
        nxt_h2p_fuzz_drain_wq(&engine->shutdown_work_queue);
        nxt_h2p_fuzz_drain_wq(&engine->close_work_queue);
        nxt_h2p_fuzz_drain_wq(&engine->accept_work_queue);
        nxt_h2p_fuzz_drain_wq(&engine->connect_work_queue);

        after = engine->connections + engine->closed_conns_cnt;

        /*
         * A round that only armed a 0ms timer (nxt_conn_close_handler(),
         * once its socket and timer housekeeping leaves events or timers
         * pending) moves nothing on this pass -- every queue empty,
         * before == after -- but has real work waiting for the very next
         * round's nxt_h2p_fuzz_drain_timers() call.  Declaring convergence
         * here would leave it stranded, and the connection (and the joint
         * reference tied to it) would leak for real.
         */
        if (engine->fast_work_queue.head == NULL
            && engine->read_work_queue.head == NULL
            && engine->write_work_queue.head == NULL
            && engine->socket_work_queue.head == NULL
            && engine->shutdown_work_queue.head == NULL
            && engine->close_work_queue.head == NULL
            && engine->accept_work_queue.head == NULL
            && engine->connect_work_queue.head == NULL
            && before == after
            && nxt_timer_find(engine) != 0)
        {
            return;
        }
    }
}


int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
    struct sockaddr_in  sin;

    if (nxt_lib_start("fuzzing", NULL, &environ) != NXT_OK) {
        return NXT_ERROR;
    }

    /* Keep a fuzzing run quiet: nothing below alert is worth printing. */
    nxt_main_log.level = NXT_LOG_ALERT;

    /*
     * nxt_http_init() hashes nxt_http_request_fields_hash (Host,
     * Content-Length, ...) that nxt_h2p_request_headers_done() and
     * nxt_h2p_field_add() rely on; without it every field lookup below
     * misses and the request-line/field-processing code this fuzzer exists
     * to cover would never run.
     */
    if (nxt_http_init(&nxt_main_task) != NXT_OK) {
        return NXT_ERROR;
    }

    /*
     * A real event engine: nxt_h2proto.c reaches task->thread->engine for
     * its work queues and timers throughout (nxt_conn_active(),
     * nxt_h2p_conn_progress(), nxt_conn_write()/nxt_conn_read(), ...).  This
     * is the same nxt_event_engine_create() call src/test/nxt_port_fail_test.c
     * uses to get a real engine outside the router's own startup.
     */
    nxt_h2p_fuzz_engine = nxt_event_engine_create(&nxt_main_task,
                                                  &nxt_epoll_edge_engine,
                                                  NULL, 0, 0);
    if (nxt_h2p_fuzz_engine == NULL) {
        return NXT_ERROR;
    }

    nxt_main_task.thread->engine = nxt_h2p_fuzz_engine;

    /*
     * nxt_event_engine_create() leaves engine->mem_pool NULL; production
     * sets it separately (nxt_runtime_thread_pool_create()).  It backs
     * engine->mem_cache (nxt_event_engine_mem_alloc()), which
     * nxt_event_engine_buf_mem_alloc() -- nxt_h2p_conn_flush()'s write
     * buffers -- needs on its first call.
     */
    nxt_h2p_fuzz_engine->mem_pool = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_h2p_fuzz_engine->mem_pool == NULL) {
        return NXT_ERROR;
    }

    /* Backs the two persistent sockaddrs below; never destroyed. */
    nxt_h2p_fuzz_config_mp = nxt_mp_create(256, 128, 256, 32);
    if (nxt_h2p_fuzz_config_mp == NULL) {
        return NXT_ERROR;
    }

    /*
     * One persistent nxt_socket_conf_t/joint, built once and reused every
     * iteration -- the fields nxt_h2proto.c actually reads (limits,
     * timeouts, the temp path) do not change between inputs.
     *
     * joint.count is pinned at 1 at rest, between iterations.  Production
     * takes two more references and drops each back: one per connection
     * (nxt_h2p_conn_init(), dropped by nxt_h2p_conn_cleanup() when
     * c->mem_pool is destroyed -- see LLVMFuzzerTestOneInput()) and one per
     * stream (nxt_h2p_on_begin_headers(), dropped by
     * nxt_h2p_request_close(), both via nxt_router_conf_release()).
     * Starting at 1 means every iteration this fuzzer drives to completion
     * returns it to exactly 1, and count never reaches 0 --
     * nxt_router_conf_release()'s branch below that point reaches into a
     * real nxt_router_conf_t/nxt_router_t this harness does not have.  A
     * leaked reference (a harness bug, not a target bug) would show up as
     * later iterations' requests never reaching action/error handling, not
     * as a crash -- which is why LLVMFuzzerTestOneInput() aborts if the
     * count is not back to 1 once an iteration is done.
     */
    nxt_memzero(&nxt_h2p_fuzz_skcf, sizeof(nxt_socket_conf_t));
    nxt_memzero(&nxt_h2p_fuzz_joint, sizeof(nxt_socket_conf_joint_t));
    nxt_memzero(&nxt_h2p_fuzz_rtcf, sizeof(nxt_router_conf_t));

    nxt_h2p_fuzz_joint.count = 1;
    nxt_h2p_fuzz_joint.socket_conf = &nxt_h2p_fuzz_skcf;

    /*
     * nxt_http_request_close_handler() -- run for every request this
     * fuzzer completes, on or off the action==NULL error path -- reaches
     * r->conf->socket_conf->router_conf unconditionally for its access-log
     * check.  access_log stays NULL, so that check is skipped and nothing
     * else in nxt_router_conf_t needs a value: the real nxt_router_conf_t
     * this stands in for is only otherwise reached through a router action,
     * which action == NULL never dispatches to.
     */
    nxt_h2p_fuzz_skcf.router_conf = &nxt_h2p_fuzz_rtcf;

    /* Roomy enough that most bodies stay in memory; some still won't. */
    nxt_h2p_fuzz_skcf.large_header_buffer_size = 8192;
    nxt_h2p_fuzz_skcf.large_header_buffers = 4;
    nxt_h2p_fuzz_skcf.body_buffer_size = 4096;
    nxt_h2p_fuzz_skcf.max_body_size = 1024 * 1024;
    nxt_h2p_fuzz_skcf.idle_timeout = 1000;
    nxt_h2p_fuzz_skcf.header_read_timeout = 1000;
    nxt_h2p_fuzz_skcf.body_read_timeout = 1000;
    nxt_h2p_fuzz_skcf.send_timeout = 1000;
    nxt_h2p_fuzz_skcf.log_route = 0;
    nxt_h2p_fuzz_skcf.action = NULL;
    nxt_h2p_fuzz_skcf.forwarded = NULL;
    nxt_h2p_fuzz_skcf.client_ip = NULL;
#if (NXT_TLS)
    nxt_h2p_fuzz_skcf.tls = NULL;
#endif

    /*
     * A real, unlinked temp file under /tmp is exactly what
     * nxt_http_request_body_alloc() does for a body over body_buffer_size
     * in production; this exercises the same file-backed path in
     * nxt_h2p_on_data_chunk_recv() (nxt_buf_is_file()) that an in-memory-only
     * body_buffer_size would skip.
     */
    nxt_str_set(&nxt_h2p_fuzz_skcf.body_temp_path, "/tmp");

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(80);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    nxt_h2p_fuzz_local = nxt_sockaddr_create(nxt_h2p_fuzz_config_mp,
                                             (struct sockaddr *) &sin,
                                             sizeof(sin), 0);
    if (nxt_h2p_fuzz_local == NULL) {
        return NXT_ERROR;
    }

    nxt_h2p_fuzz_skcf.sockaddr = nxt_h2p_fuzz_local;

    sin.sin_port = htons(12345);

    nxt_memzero(&nxt_h2p_fuzz_remote_template, sizeof(nxt_listen_socket_t));

    nxt_h2p_fuzz_remote_template.sockaddr =
        nxt_sockaddr_create(nxt_h2p_fuzz_config_mp, (struct sockaddr *) &sin,
                            sizeof(sin), 0);
    if (nxt_h2p_fuzz_remote_template.sockaddr == NULL) {
        return NXT_ERROR;
    }

    nxt_h2p_fuzz_remote_template.socklen = sizeof(sin);
    nxt_h2p_fuzz_remote_template.address_length = 0;

    /*
     * c->listen->socket.data is the joint; c->listen->draining stays 0.
     * c->listen->count is reset to 2 at the top of every
     * LLVMFuzzerTestOneInput() (see the comment there); this only covers
     * the very first iteration's read.
     */
    nxt_memzero(&nxt_h2p_fuzz_lev, sizeof(nxt_listen_event_t));
    nxt_h2p_fuzz_lev.socket.data = &nxt_h2p_fuzz_joint;
    nxt_h2p_fuzz_lev.count = 2;

    return 0;
}


/*
 * Bare-bones cleanup for the one failure this harness can hit before h2c
 * even exists (its nxt_mp_zget() failing).  Nothing routes through
 * nxt_h2p_closing() -- there is no h2c to give it -- so this does by hand
 * what nxt_h1p_conn_free() otherwise would: free the cache-allocated
 * c->remote, then release the conn itself.  lev->count is untouched
 * (nxt_router_listen_event_release() never runs on this path either), which
 * is why it is reset unconditionally at the top of every iteration instead
 * of only after a normal close.
 */
static void
nxt_h2p_fuzz_conn_abandon(nxt_event_engine_t *engine, nxt_conn_t *c)
{
    if (c->remote != NULL) {
        nxt_sockaddr_cache_free(engine, c);
    }

    nxt_conn_untrack(engine, c);
    nxt_conn_free(&c->task, c);
    nxt_timer_find(engine);
    nxt_conn_recycle_pending(engine);
}


int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    int                 sv[2];
    size_t              off, chunk;
    nxt_mp_t            *mp;
    nxt_buf_t           *b;
    nxt_conn_t          *c;
    nxt_h2proto_t       *h2c;
    nxt_event_engine_t  *engine;

    if (size < NXT_H2P_FUZZ_MIN_INPUT || size > NXT_H2P_FUZZ_MAX_INPUT) {
        return 0;
    }

    engine = nxt_h2p_fuzz_engine;

    /*
     * Reset every iteration to a value nxt_router_listen_event_release()
     * (called at most once per iteration, from inside the real close chain
     * below) can decrement without ever reaching 0 -- which would nxt_free()
     * this static struct -- or its count == 1 && draining branch, which
     * this harness's lev never sets draining for anyway.  See
     * nxt_h2p_fuzz_conn_abandon() for the path that does not decrement it.
     */
    nxt_h2p_fuzz_lev.count = 2;

    nxt_h2p_fuzz_conn_gone = 0;

    mp = nxt_mp_create(2048, 128, 512, 32);
    if (mp == NULL) {
        return 0;
    }

    if (nxt_mp_cleanup(mp, nxt_h2p_fuzz_conn_gone_handler, &engine->task,
                       NULL, NULL)
        != NXT_OK)
    {
        nxt_mp_destroy(mp);
        return 0;
    }

    c = nxt_conn_create(mp, &nxt_main_task);
    if (c == NULL) {
        nxt_mp_destroy(mp);
        return 0;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) != 0) {
        nxt_h2p_fuzz_conn_abandon(engine, c);
        return 0;
    }

    c->socket.fd = sv[0];

    c->remote = nxt_sockaddr_cache_alloc(engine, &nxt_h2p_fuzz_remote_template);
    if (c->remote == NULL) {
        close(sv[0]);
        close(sv[1]);
        c->socket.fd = -1;
        nxt_h2p_fuzz_conn_abandon(engine, c);
        return 0;
    }

    c->listen = &nxt_h2p_fuzz_lev;

    h2c = nxt_mp_zget(c->mem_pool, sizeof(nxt_h2proto_t));
    if (h2c == NULL) {
        close(sv[0]);
        close(sv[1]);
        nxt_h2p_fuzz_conn_abandon(engine, c);
        return 0;
    }

    h2c->conn = c;
    nxt_queue_init(&h2c->streams);

    /*
     * h2c->joint stands in for nxt_h2p_conn_init()'s own joint = c->listen->
     * socket.data: nxt_h2p_conn_leaving() (drain) compares the two, and
     * nxt_h2p_conn_cleanup() (registered right below, exactly as
     * nxt_h2p_conn_init() does) releases this same reference through
     * nxt_router_conf_release() when c->mem_pool is destroyed -- from
     * nxt_h2p_fuzz_conn_abandon()'s nxt_conn_free() on every early-failure
     * path below, or from the real close chain's nxt_conn_free() on the
     * normal path.  Without h2c->joint set, it stays NULL from nxt_mp_zget(),
     * always differs from c->listen->socket.data, and nxt_h2p_conn_leaving()
     * is true from the first read: every connection would drain immediately,
     * skipping most of nxt_h2proto.c's request-handling code.
     */
    h2c->joint = &nxt_h2p_fuzz_joint;

    if (nxt_mp_cleanup(c->mem_pool, nxt_h2p_conn_cleanup, &engine->task, h2c,
                       NULL)
        != NXT_OK)
    {
        h2c->joint = NULL;
        close(sv[0]);
        close(sv[1]);
        nxt_h2p_fuzz_conn_abandon(engine, c);
        return 0;
    }

    /*
     * Paired with nxt_h2p_conn_cleanup()'s nxt_router_conf_release() above:
     * nxt_h2p_conn_init() increments joint->count immediately after
     * registering that same cleanup, before anything that can fail, so
     * every path from here on (this harness's early-failure gotos included)
     * reaches the cleanup with the increment already done.  See the
     * LLVMFuzzerTestOneInput()-top comment on nxt_h2p_fuzz_joint.count for
     * why it never reaches 0.
     */
    nxt_h2p_fuzz_joint.count++;

    /*
     * A session_create() failure past nghttp2_session_server_new2() leaks
     * h2c->session unless the caller deletes it -- nxt_h2p_conn_init() does
     * exactly this in its own fail: label.  h2c->session = NULL afterwards
     * matters now that nxt_h2p_conn_cleanup() is registered: it deletes the
     * session too if this harness has not already, and would double-free
     * it otherwise once c->mem_pool is destroyed below.
     */
    if (nxt_h2p_session_create(&c->task, h2c, &nxt_h2p_fuzz_skcf) != NXT_OK) {
        if (h2c->session != NULL) {
            nghttp2_session_del(h2c->session);
            h2c->session = NULL;
        }

        close(sv[1]);
        c->socket.fd = -1;
        close(sv[0]);
        nxt_h2p_fuzz_conn_abandon(engine, c);
        return 0;
    }

    b = nxt_buf_mem_alloc(c->mem_pool, NXT_H2P_READ_BUFFER_SIZE, 0);
    if (b == NULL) {
        nghttp2_session_del(h2c->session);
        h2c->session = NULL;
        close(sv[1]);
        c->socket.fd = -1;
        close(sv[0]);
        nxt_h2p_fuzz_conn_abandon(engine, c);
        return 0;
    }

    c->read = b;
    c->socket.data = h2c;
    c->read_state = &nxt_h2p_read_state;
    c->write_state = &nxt_h2p_write_state;

    /*
     * Fed as production would deliver it: the fixed preface, then the
     * fuzzed bytes, chunked to the same NXT_H2P_READ_BUFFER_SIZE a real
     * recv() into c->read is capped at, one nxt_h2p_conn_read() per chunk --
     * so a frame or a HEADERS block split across reads is exercised the
     * same way a slow client produces it, not reassembled up front.
     */
    nxt_memcpy(b->mem.free, nxt_h2p_fuzz_preface,
              sizeof(nxt_h2p_fuzz_preface));
    b->mem.free += sizeof(nxt_h2p_fuzz_preface);

    off = 0;

    for ( ;; ) {
        chunk = nxt_min(size - off,
                        (size_t) nxt_buf_mem_free_size(&b->mem));

        nxt_memcpy(b->mem.free, data + off, chunk);
        b->mem.free += chunk;
        off += chunk;

        nxt_h2p_conn_read(&c->task, c, h2c);

        if (off >= size) {
            break;
        }

        /* nxt_h2p_conn_read() only queues a close; still, flag first. */
        if (nxt_h2p_fuzz_conn_gone || h2c->closing || h2c->session == NULL) {
            /* The connection is gone; nothing left to feed it. */
            break;
        }

        /* nxt_h2p_conn_read() resets b->mem to empty once it is consumed. */
    }

    /*
     * Drain first, before failing anything: a request whose headers were
     * already sent (nxt_h2p_request_header_send() queues its body_handler
     * onto fast_work_queue rather than calling it -- "the body handler
     * runs before the frames are pulled", per its own comment) is healthy
     * and mid-completion, not stuck waiting on the client -- failing it
     * out from under that queued completion double-processes the same
     * request (nxt_http_request_error()'s own re-entrancy guard then
     * takes the *other* branch) instead of letting it finish normally.
     * This is what production always has time for too: nothing here forces
     * a real connection's fast_work_queue to stay unpopped the way calling
     * nxt_h2p_streams_fail() first would.
     */
    nxt_h2p_fuzz_drain_engine(engine);

    /*
     * Whatever is left after that drain is genuinely stuck waiting on the
     * client -- headers incomplete, or a body nxt_h2p_request_body_read()
     * is still waiting for (stream->body_wanted) -- the same as a client
     * disappearing or a fatal nghttp2 error would leave behind; a no-op if
     * nxt_h2p_conn_abort() already ran (h2c->session == NULL by now).
     * Draining again before nxt_h2p_closing() matters:
     * nxt_h2p_stream_fail() can only *queue* a stream's error handler
     * (fast_work_queue, for one whose headers are not done), and
     * nxt_h2p_closing() unconditionally frees every remaining stream
     * struct in h2c->streams -- freeing one a queued item still holds as
     * its `data` argument would use it after free once that item runs.
     * Draining lets every failed stream detach (stream->r = NULL, via
     * nxt_h2p_request_close()) before that loop runs.
     *
     * The drain above may already have run the whole close chain: a
     * connection failure (nxt_h2p_conn_fail(), e.g. nxt_h2p_conn_flush()
     * out of memory) with nothing left to write goes to nxt_h2p_closing()
     * at once, and the drain then runs the queued close down to
     * nxt_conn_free(), which destroys c->mem_pool and h2c with it.  From
     * then on neither h2c nor c may be touched, and there is nothing left
     * to close: nxt_h2p_fuzz_conn_gone says so.
     */
    if (!nxt_h2p_fuzz_conn_gone && h2c->session != NULL) {
        nxt_h2p_streams_fail(&c->task, h2c, 1);
        nxt_h2p_fuzz_drain_engine(engine);
    }

    /*
     * The real close chain, run for real: nxt_h2p_closing() deletes the
     * session and frees every stream, drains c->write, and calls
     * nxt_h1p_closing() -- which, since c->u.tls stays NULL, goes straight
     * to nxt_conn_close() (queued) rather than a TLS shutdown.  Draining
     * again runs that queued close, which reaches nxt_h1p_conn_free():
     * nxt_sockaddr_cache_free() + nxt_conn_free() (parks c onto
     * engine->pending_connections; releases c->mem_pool, i.e. mp) +
     * nxt_router_listen_event_release() (decrements lev->count, reset to 2
     * every iteration above).  Idempotent either way nxt_h2p_conn_abort()
     * already reached it during the read loop (h2c->closed guards it), so
     * this runs whenever the pool is still there.  Nothing below may touch
     * c, h2c or mp again.
     */
    if (!nxt_h2p_fuzz_conn_gone) {
        nxt_h2p_closing(&c->task, h2c);
        nxt_h2p_fuzz_drain_engine(engine);
    }

    /* nxt_h1p_conn_free() did not touch this end; nothing else will. */
    close(sv[1]);

    nxt_timer_find(engine);
    nxt_conn_recycle_pending(engine);

    if (!nxt_h2p_fuzz_conn_gone) {
        /* The close chain did not reach nxt_conn_free(): a harness bug. */
        nxt_abort();
    }

    if (nxt_h2p_fuzz_joint.count != 1) {
        /* A harness bug leaked a stream's joint ref; see the comment above. */
        nxt_abort();
    }

    return 0;
}
