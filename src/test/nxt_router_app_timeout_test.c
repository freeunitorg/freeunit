/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for the request deadline answering a request a worker is
 * already running.
 *
 * nxt_router_app_timeout() serves two deadlines.  After an acknowledgement a
 * worker holds the request and 503 is the answer.  Before one the request is
 * still in the application's shared port queue, and whether it can be taken
 * back is decided by a single CAS: nxt_app_queue_cancel() and the worker's
 * nxt_unit_app_queue_recv() run the same compare-and-swap on the queue item's
 * tracking word, so exactly one of them wins.  The handler used to answer 503
 * and unlink regardless of who won, which failed a request that was executing
 * -- and ran it a second time if the client retried.
 *
 * Nothing about that window can be reached by racing a real application: it is
 * one CAS wide.  So the CAS is driven here instead.  The "worker" is this test
 * calling nxt_app_queue_recv() and nxt_app_queue_cancel() exactly as
 * nxt_unit_app_queue_recv() (src/nxt_unit.c) does, before or after the
 * deadline fires, and the handler runs through the engine's real timer
 * machinery.
 *
 * Three things are pinned down:
 *
 *   - a claimed request is not answered and its message is left intact, since
 *     the tail of that chain is the body the acknowledgement still has to
 *     send;
 *   - a later expiry does not answer it either: until the acknowledgement
 *     arrives, an unlink would drop that body and cancel the RPC, and libunit
 *     would keep the request for ever;
 *   - a request nobody claimed is still answered at once, and its retraction
 *     really takes the message out of the queue.
 *
 * The request is a hand-built nxt_http_request_t with ->header_sent set and no
 * protocol: nxt_http_request_error() then stops at its own guard and reduces
 * to nxt_http_request_error_handler(), which sets ->error and nothing else.
 * That makes ->error the mark of "this expiry answered the request" without
 * dragging a connection into the fixture.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_rpc.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_port_memory_int.h>
#include <nxt_router_request.h>
#include <nxt_app_queue.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


/* File scope: the deadline hands the application to the router's
   reference-counting helpers, so its storage must outlive the frame. */
static nxt_app_t  nxt_router_app_timeout_test_app;

static nxt_uint_t  nxt_router_app_timeout_test_completions;


typedef struct {
    nxt_http_request_t      *r;
    nxt_request_rpc_data_t  *req_rpc_data;
    nxt_buf_t               *header;
    nxt_buf_t               *body;
} nxt_router_app_timeout_test_req_t;


static void
nxt_router_app_timeout_test_completion(nxt_task_t *task, void *obj, void *data)
{
    nxt_router_app_timeout_test_completions++;
}


/*
 * Commit the pending timer changes and run what is due, the way
 * nxt_event_engine_start() does.  nxt_timer_find() folds the change list into
 * the rbtree, nxt_timer_expire() queues the handlers that are due, and the
 * drain runs them -- including a handler that re-arms its own timer, which is
 * what the claimed request does.
 */

static void
nxt_router_app_timeout_test_tick(nxt_task_t *task, nxt_event_engine_t *engine,
    nxt_msec_t advance)
{
    nxt_task_t          *wq_task;
    nxt_work_handler_t  handler;
    void                *obj, *data;

    (void) nxt_timer_find(engine);

    nxt_timer_expire(engine, engine->timers.now + advance);

    /* nxt_work_queue_pop() dereferences the head unconditionally. */

    while (engine->fast_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine->fast_work_queue, &wq_task, &obj,
                                     &data);

        if (handler == NULL) {
            continue;
        }

        handler(wq_task != NULL ? wq_task : task, obj, data);
    }
}


/*
 * What a worker does with a queue item, from nxt_unit_app_queue_recv():
 * dequeue it, then claim it with the same CAS the router's retraction uses.
 * Answers whether this side won.
 */

static nxt_bool_t
nxt_router_app_timeout_test_claim(nxt_app_queue_t *queue)
{
    uint32_t        cookie;
    ssize_t         size;
    uint8_t         rbuf[NXT_APP_QUEUE_MSG_SIZE];
    nxt_port_msg_t  *port_msg;

    size = nxt_app_queue_recv(queue, rbuf, &cookie);

    if (size < (ssize_t) sizeof(nxt_port_msg_t)) {
        return 0;
    }

    port_msg = (nxt_port_msg_t *) rbuf;

    return nxt_app_queue_cancel(queue, cookie, port_msg->stream);
}


/*
 * A request in the state nxt_router_app_port_get() leaves behind: its message
 * in the shared port queue, itself parked in ack_waiting_req with no worker,
 * and the deadline armed.  Each leg gets its own request, because the unlink
 * that ends one leaves that request's timer armed for the pool release.
 */

static nxt_int_t
nxt_router_app_timeout_test_arm(nxt_task_t *task, nxt_event_engine_t *engine,
    nxt_mp_t *mp, nxt_app_t *app, uint32_t stream,
    nxt_router_app_timeout_test_req_t *t)
{
    int                     notify;
    nxt_buf_t               *header, *body;
    nxt_http_request_t      *r;
    nxt_request_rpc_data_t  *req_rpc_data;

    struct {
        nxt_port_msg_t       pm;
        nxt_port_mmap_msg_t  mm;
    } nxt_packed msg;

    r = nxt_mp_zalloc(mp, sizeof(nxt_http_request_t));
    req_rpc_data = nxt_mp_zalloc(mp, sizeof(nxt_request_rpc_data_t));
    header = nxt_mp_zalloc(mp, sizeof(nxt_buf_t));
    body = nxt_mp_zalloc(mp, sizeof(nxt_buf_t));

    if (nxt_slow_path(r == NULL || req_rpc_data == NULL
                      || header == NULL || body == NULL))
    {
        return NXT_ERROR;
    }

    r->mem_pool = mp;
    r->engine = engine;

    /* See the file comment: keeps the 503 out of the protocol layer. */
    r->header_sent = 1;
    r->proto.any = NULL;

    r->timer.task = &engine->task;
    r->timer.work_queue = &engine->fast_work_queue;
    r->timer.log = engine->task.log;
    r->timer.bias = NXT_TIMER_DEFAULT_BIAS;
    r->timer.handler = nxt_router_test_app_timeout;
    r->timer_data = req_rpc_data;

    r->req_rpc_data = req_rpc_data;

    req_rpc_data->stream = stream;
    req_rpc_data->app = app;
    req_rpc_data->app_port = app->shared_port;
    req_rpc_data->apr_action = NXT_APR_REQUEST_FAILED;
    req_rpc_data->request = r;
    req_rpc_data->msg_info.body_fd = -1;

    /* The RPC registry holds nothing for this stream. */
    req_rpc_data->rpc_cancel = 0;

    nxt_port_inc_use(app->shared_port);

    /*
     * The message chain nxt_router_app_prepare_request() leaves behind: the
     * header buffer that went through the queue, marked sent, and the body
     * behind it, which nxt_router_req_headers_ack_handler() sends only once a
     * worker acknowledges.
     */

    header->completion_handler = nxt_router_app_timeout_test_completion;
    header->is_port_mmap_sent = 1;
    header->next = body;

    body->completion_handler = nxt_router_app_timeout_test_completion;

    req_rpc_data->msg_info.buf = header;

    nxt_memzero(&msg, sizeof(msg));
    msg.pm.stream = stream;
    msg.pm.type = NXT_PORT_MSG_REQ_HEADERS;
    msg.pm.mmap = 1;

    if (nxt_slow_path(nxt_app_queue_send(app->shared_port->queue, &msg,
                          sizeof(msg), stream, &notify,
                          &req_rpc_data->msg_info.tracking_cookie) != NXT_OK))
    {
        return NXT_ERROR;
    }

    nxt_thread_mutex_lock(&app->mutex);

    app->active_requests++;
    nxt_queue_insert_tail(&app->ack_waiting_req, &r->app_link);

    nxt_thread_mutex_unlock(&app->mutex);

    nxt_mp_retain(mp);

    nxt_timer_add(engine, &r->timer, app->timeout);

    t->r = r;
    t->req_rpc_data = req_rpc_data;
    t->header = header;
    t->body = body;

    return NXT_OK;
}


nxt_int_t
nxt_router_app_timeout_test(nxt_thread_t *thr)
{
    nxt_mp_t                           *mp, *req_mp;
    nxt_app_t                          *app;
    nxt_int_t                          ret;
    nxt_task_t                         *task;
    nxt_port_t                         *shared_port;
    nxt_bool_t                         app_mutex, timers;
    nxt_runtime_t                      *rt, *saved_rt;
    nxt_app_queue_t                    *queue;
    nxt_event_engine_t                 engine, *saved_engine;
    nxt_router_app_timeout_test_req_t  claimed, unclaimed;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router app timeout test started");

    ret = NXT_ERROR;
    shared_port = NULL;
    req_mp = NULL;
    app_mutex = 0;
    timers = 0;
    app = &nxt_router_app_timeout_test_app;

    task = thr->task;
    task->thread = thr;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;

    if (nxt_slow_path(nxt_port_rpc_init() != NXT_OK)) {
        return NXT_ERROR;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    queue = nxt_mp_zalloc(mp, sizeof(nxt_app_queue_t));
    if (nxt_slow_path(rt == NULL || queue == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    rt->mem_pool = mp;

    if (nxt_slow_path(nxt_thread_mutex_create(&rt->processes_mutex) != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");
    engine.mem_pool = mp;

    /* The deadline runs on the engine's own task, as it does in the router. */

    engine.task.thread = thr;
    engine.task.log = thr->log;

    /*
     * Real timer machinery: the re-arm under test is an nxt_timer_add() from
     * inside the timer's own handler, and a memzero'd nxt_timers_t would let a
     * re-arm that never lands look correct.
     */

    if (nxt_slow_path(nxt_timers_init(&engine.timers, 64) != NXT_OK)) {
        goto done;
    }

    timers = 1;

    thr->runtime = rt;
    thr->engine = &engine;
    rt->main_engine = &engine;

    /* The port every worker of the application reads, and its queue. */

    shared_port = nxt_port_new(task, NXT_SHARED_PORT_ID, nxt_pid,
                               NXT_PROCESS_APP);
    if (nxt_slow_path(shared_port == NULL)) {
        goto done;
    }

    shared_port->pair[0] = -1;
    shared_port->pair[1] = -1;
    shared_port->socket.fd = -1;
    shared_port->queue_fd = -1;

    nxt_app_queue_init(queue);
    shared_port->queue = queue;

    nxt_memzero(app, sizeof(nxt_app_t));

    if (nxt_slow_path(nxt_thread_mutex_create(&app->mutex) != NXT_OK)) {
        goto done;
    }

    app_mutex = 1;

    nxt_queue_init(&app->ports);
    nxt_queue_init(&app->spare_ports);
    nxt_queue_init(&app->idle_ports);
    nxt_queue_init(&app->ack_waiting_req);

    nxt_str_set(&app->name, "app-timeout-test");

    app->shared_port = shared_port;
    app->timeout = 1000;
    app->processes = 1;

    /* Never reached by the releases below, so the deadline cannot free it. */
    app->use_count = 8;

    /*
     * The pool the requests live in.  Retained beyond what the legs release,
     * so nothing frees a request out from under the assertions that follow.
     */

    req_mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(req_mp == NULL)) {
        goto done;
    }

    nxt_mp_retain(req_mp);
    nxt_mp_retain(req_mp);

    /*
     * Leg 1: a worker claims the slot before the deadline fires.  The
     * retraction inside the handler must lose, and the handler must then leave
     * the request alone.
     */

    if (nxt_slow_path(nxt_router_app_timeout_test_arm(task, &engine, req_mp,
                                                      app, 0x4321, &claimed)
                      != NXT_OK))
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: could not arm a request");
        goto done;
    }

    if (nxt_slow_path(!nxt_router_app_timeout_test_claim(queue))) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: the worker could not claim "
                      "the queued message");
        goto done;
    }

    nxt_router_app_timeout_test_completions = 0;

    nxt_router_app_timeout_test_tick(task, &engine, app->timeout);

    if (claimed.req_rpc_data->msg_info.cancel != NXT_MSG_CLAIMED) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: the deadline did not see the "
                      "claim (cancel %d)",
                      (int) claimed.req_rpc_data->msg_info.cancel);
        goto done;
    }

    if (claimed.r->error != 0 || claimed.r->req_rpc_data == NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: a claimed request was "
                      "answered (error %d, rpc data %p)",
                      (int) claimed.r->error, claimed.r->req_rpc_data);
        goto done;
    }

    if (claimed.req_rpc_data->msg_info.buf != claimed.header
        || claimed.header->next != claimed.body
        || nxt_router_app_timeout_test_completions != 0)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: the message of a claimed "
                      "request was completed (%ui buffers), so the "
                      "acknowledgement has no body left to send",
                      nxt_router_app_timeout_test_completions);
        goto done;
    }

    if (claimed.r->timer.enabled) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: the deadline was re-armed "
                      "for a claimed request with no acknowledgement");
        goto done;
    }

    /*
     * Leg 2: the acknowledgement is late.  Later expiries must not answer the
     * request, because the acknowledgement still has to send the body.
     */

    nxt_router_app_timeout_test_tick(task, &engine, app->timeout);
    nxt_router_app_timeout_test_tick(task, &engine, 2 * app->timeout);

    if (claimed.r->error != 0 || claimed.r->req_rpc_data == NULL
        || claimed.req_rpc_data->msg_info.buf != claimed.header
        || claimed.header->next != claimed.body
        || nxt_router_app_timeout_test_completions != 0)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: a claimed request was "
                      "answered before its acknowledgement (error %d, "
                      "%ui buffers)",
                      (int) claimed.r->error,
                      nxt_router_app_timeout_test_completions);
        goto done;
    }

    /*
     * Leg 3: the control.  Nobody claims this one, so the retraction wins and
     * the deadline answers at once -- the behaviour the fix must not change.
     */

    if (nxt_slow_path(nxt_router_app_timeout_test_arm(task, &engine, req_mp,
                                                      app, 0x4322, &unclaimed)
                      != NXT_OK))
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: could not arm a request");
        goto done;
    }

    nxt_router_app_timeout_test_completions = 0;

    nxt_router_app_timeout_test_tick(task, &engine, app->timeout);

    if (unclaimed.req_rpc_data->msg_info.cancel != NXT_MSG_RETRACTED
        || unclaimed.r->error == 0 || unclaimed.r->req_rpc_data != NULL)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: an unclaimed request was not "
                      "answered (cancel %d, error %d)",
                      (int) unclaimed.req_rpc_data->msg_info.cancel,
                      (int) unclaimed.r->error);
        goto done;
    }

    if (nxt_router_app_timeout_test_completions != 2
        || unclaimed.header->is_port_mmap_sent != 0)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: a retracted message did not "
                      "give its shared memory back (%ui buffers, sent %d)",
                      nxt_router_app_timeout_test_completions,
                      (int) unclaimed.header->is_port_mmap_sent);
        goto done;
    }

    /*
     * The retraction has to be real, not just reported: a worker reaching the
     * item now must find it cancelled and skip it, which is what
     * nxt_unit_app_queue_recv() does with a CAS that answers false.
     */

    if (nxt_router_app_timeout_test_claim(queue)) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router app timeout test: a worker still claimed a "
                      "retracted message");
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router app timeout test passed");

    ret = NXT_OK;

done:

    if (shared_port != NULL) {
        nxt_port_use(task, shared_port, -1);
    }

    thr->engine = saved_engine;
    thr->runtime = saved_rt;

    if (app_mutex) {
        nxt_thread_mutex_destroy(&app->mutex);
    }

    if (timers) {
        nxt_free(engine.timers.changes);
    }

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_thread_mutex_destroy(&rt->processes_mutex);

    if (req_mp != NULL) {
        nxt_mp_destroy(req_mp);
    }

    nxt_mp_destroy(mp);

    return ret;
}
