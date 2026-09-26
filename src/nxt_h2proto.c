
/*
 * Copyright (C) FreeUnit contributors
 */

#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_http_parse.h>
#include <nxt_h2proto.h>


/*
 * nxt_h2p_conn_ prefix is used for connection handlers.
 * nxt_h2p_request_ prefix is used for the HTTP/2 protocol request hooks.
 * nxt_h2p_on_ prefix is used for the nghttp2 callbacks.
 *
 * Every nghttp2 callback runs inside nghttp2_session_mem_recv() or
 * nghttp2_session_mem_send(); h2c->busy is set around those two calls and
 * nxt_h2p_conn_flush() only records a pending flush while it is set, since
 * nghttp2 forbids re-entering the session from a callback.  For the same
 * reason nxt_h2p_closing() only records a pending close while it is set:
 * a request that a callback fails can close at once and release the last
 * stream, and the session must not be deleted under nghttp2.
 */

static nxt_int_t nxt_h2p_session_create(nxt_task_t *task, nxt_h2proto_t *h2c,
    nxt_socket_conf_t *skcf);
static void nxt_h2p_conn_read(nxt_task_t *task, void *obj, void *data);
static void nxt_h2p_conn_flush(nxt_task_t *task, nxt_h2proto_t *h2c);
static void nxt_h2p_conn_sent(nxt_task_t *task, void *obj, void *data);
static void nxt_h2p_conn_close(nxt_task_t *task, void *obj, void *data);
static void nxt_h2p_conn_error(nxt_task_t *task, void *obj, void *data);
static void nxt_h2p_conn_read_timeout(nxt_task_t *task, void *obj, void *data);
static void nxt_h2p_conn_send_timeout(nxt_task_t *task, void *obj, void *data);
static nxt_msec_t nxt_h2p_conn_timer_value(nxt_conn_t *c, uintptr_t data);
static nxt_msec_t nxt_h2p_conn_progress_timeout(nxt_h2proto_t *h2c,
    nxt_socket_conf_t *skcf);
static nxt_bool_t nxt_h2p_stream_incomplete(nxt_h2proto_t *h2c,
    nxt_h2p_stream_t *stream);
static void nxt_h2p_conn_progress(nxt_h2proto_t *h2c);
static void nxt_h2p_conn_expire(nxt_task_t *task, nxt_h2proto_t *h2c,
    nxt_bool_t all);
static nxt_msec_t nxt_h2p_conn_send_timer_value(nxt_conn_t *c,
    uintptr_t data);
static void nxt_h2p_conn_fail(nxt_task_t *task, nxt_h2proto_t *h2c);
static void nxt_h2p_conn_abort(nxt_task_t *task, nxt_h2proto_t *h2c,
    int64_t goaway);
static nxt_buf_t *nxt_h2p_goaway_frame(nxt_task_t *task, nxt_h2proto_t *h2c,
    uint32_t error_code);
static void nxt_h2p_streams_fail(nxt_task_t *task, nxt_h2proto_t *h2c,
    nxt_bool_t all);
static void nxt_h2p_conn_goaway(nxt_h2proto_t *h2c, int32_t last_id);
static void nxt_h2p_conn_shutdown(nxt_task_t *task, nxt_h2proto_t *h2c);
static void nxt_h2p_closing(nxt_task_t *task, nxt_h2proto_t *h2c);
static void nxt_h2p_stream_free(nxt_h2proto_t *h2c, nxt_h2p_stream_t *stream);
static void nxt_h2p_stream_fail(nxt_task_t *task, nxt_h2p_stream_t *stream);

static int nxt_h2p_on_begin_headers(nghttp2_session *session,
    const nghttp2_frame *frame, void *user_data);
static int nxt_h2p_on_header(nghttp2_session *session,
    const nghttp2_frame *frame, const uint8_t *name, size_t namelen,
    const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data);
static nxt_int_t nxt_h2p_field_add(nxt_http_request_t *r, const uint8_t *name,
    size_t namelen, const uint8_t *value, size_t valuelen);
static int nxt_h2p_on_frame_recv(nghttp2_session *session,
    const nghttp2_frame *frame, void *user_data);
static void nxt_h2p_request_headers_done(nxt_task_t *task,
    nxt_h2p_stream_t *stream);
static nxt_http_status_t nxt_h2p_request_target(nxt_h2p_stream_t *stream,
    nxt_http_request_t *r);
static int nxt_h2p_on_data_chunk_recv(nghttp2_session *session,
    uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len,
    void *user_data);
static void nxt_h2p_request_body_error(nxt_h2p_stream_t *stream,
    nxt_http_status_t status);
static void nxt_h2p_request_end_stream(nxt_task_t *task,
    nxt_h2p_stream_t *stream);
static int nxt_h2p_on_stream_close(nghttp2_session *session,
    int32_t stream_id, uint32_t error_code, void *user_data);
static int nxt_h2p_on_frame_send(nghttp2_session *session,
    const nghttp2_frame *frame, void *user_data);
static ssize_t nxt_h2p_data_read(nghttp2_session *session, int32_t stream_id,
    uint8_t *buf, size_t length, uint32_t *data_flags,
    nghttp2_data_source *source, void *user_data);
static void nxt_h2p_stream_drain(nxt_task_t *task, nxt_h2p_stream_t *stream);


static const nxt_conn_state_t  nxt_h2p_read_state;
static const nxt_conn_state_t  nxt_h2p_write_state;

static const nxt_str_t  nxt_h2p_version = nxt_string("HTTP/2.0");


void
nxt_h2p_conn_init(nxt_task_t *task, nxt_conn_t *c)
{
    size_t                   size;
    nxt_buf_t                *b, *in;
    nxt_h2proto_t            *h2c;
    nxt_event_engine_t       *engine;
    nxt_socket_conf_joint_t  *joint;

    nxt_debug(task, "h2p conn init");

    engine = task->thread->engine;
    joint = c->listen->socket.data;
    h2c = NULL;

    if (nxt_slow_path(joint == NULL || c->listen->draining)) {
        goto fail;
    }

    h2c = nxt_mp_zget(c->mem_pool, sizeof(nxt_h2proto_t));
    if (nxt_slow_path(h2c == NULL)) {
        goto fail;
    }

    h2c->conn = c;
    nxt_queue_init(&h2c->streams);

    b = nxt_buf_mem_alloc(c->mem_pool, NXT_H2P_READ_BUFFER_SIZE, 0);
    if (nxt_slow_path(b == NULL)) {
        goto fail;
    }

    if (nxt_h2p_session_create(task, h2c, joint->socket_conf) != NXT_OK) {
        goto fail;
    }

    /*
     * The idle state read the first bytes (the client preface) into an
     * engine buffer that is smaller than the h2 read buffer; move them.
     */
    in = c->read;

    if (in != NULL) {
        size = nxt_buf_mem_used_size(&in->mem);
        b->mem.free = nxt_cpymem(b->mem.free, in->mem.pos, size);

        nxt_event_engine_buf_mem_free(engine, in);
    }

    c->read = b;
    c->socket.data = h2c;
    c->read_state = &nxt_h2p_read_state;
    c->write_state = &nxt_h2p_write_state;

    nxt_h2p_conn_read(&c->task, c, h2c);

    return;

fail:

    if (h2c != NULL && h2c->session != NULL) {
        nghttp2_session_del(h2c->session);
    }

    if (c->read != NULL) {
        nxt_event_engine_buf_mem_free(engine, c->read);
        c->read = NULL;
    }

    nxt_h1p_closing(task, c);
}


static nxt_int_t
nxt_h2p_session_create(nxt_task_t *task, nxt_h2proto_t *h2c,
    nxt_socket_conf_t *skcf)
{
    int                        rc;
    nghttp2_option             *opt;
    nghttp2_settings_entry     iv[4];
    nghttp2_session_callbacks  *cbs;

    rc = nghttp2_session_callbacks_new(&cbs);
    if (nxt_slow_path(rc != 0)) {
        return NXT_ERROR;
    }

    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs,
                                                nxt_h2p_on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cbs, nxt_h2p_on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs,
                                                nxt_h2p_on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs,
                                                nxt_h2p_on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs,
                                                nxt_h2p_on_stream_close);
    nghttp2_session_callbacks_set_on_frame_send_callback(cbs,
                                                nxt_h2p_on_frame_send);

    rc = nghttp2_option_new(&opt);
    if (nxt_slow_path(rc != 0)) {
        nghttp2_session_callbacks_del(cbs);
        return NXT_ERROR;
    }

    /* ADR 0005, "Security requirements". */
    nghttp2_option_set_stream_reset_rate_limit(opt, NXT_H2P_RST_BURST,
                                               NXT_H2P_RST_RATE);
    nghttp2_option_set_max_continuations(opt, NXT_H2P_MAX_CONTINUATIONS);
    nghttp2_option_set_max_settings(opt, NXT_H2P_MAX_SETTINGS);
    nghttp2_option_set_max_deflate_dynamic_table_size(opt,
                                                NXT_H2P_HEADER_TABLE_SIZE);

    rc = nghttp2_session_server_new2(&h2c->session, cbs, h2c, opt);

    nghttp2_option_del(opt);
    nghttp2_session_callbacks_del(cbs);

    if (nxt_slow_path(rc != 0)) {
        nxt_alert(task, "nghttp2_session_server_new2() failed: %s",
                  nghttp2_strerror(rc));
        return NXT_ERROR;
    }

    iv[0].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    iv[0].value = NXT_H2P_MAX_CONCURRENT_STREAMS;
    iv[1].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
    iv[1].value = NXT_H2P_STREAM_WINDOW;
    iv[2].settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE;
    iv[2].value = skcf->large_header_buffer_size * skcf->large_header_buffers;
    iv[3].settings_id = NGHTTP2_SETTINGS_HEADER_TABLE_SIZE;
    iv[3].value = NXT_H2P_HEADER_TABLE_SIZE;

    rc = nghttp2_submit_settings(h2c->session, NGHTTP2_FLAG_NONE, iv,
                                 nxt_nitems(iv));
    if (nxt_slow_path(rc != 0)) {
        return NXT_ERROR;
    }

    rc = nghttp2_session_set_local_window_size(h2c->session,
                                               NGHTTP2_FLAG_NONE, 0,
                                               NXT_H2P_CONN_WINDOW);
    if (nxt_slow_path(rc != 0)) {
        return NXT_ERROR;
    }

    return NXT_OK;
}


static const nxt_conn_state_t  nxt_h2p_read_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_h2p_conn_read,
    .close_handler = nxt_h2p_conn_close,
    .error_handler = nxt_h2p_conn_error,

    .timer_handler = nxt_h2p_conn_read_timeout,
    .timer_value = nxt_h2p_conn_timer_value,
    .timer_data = offsetof(nxt_socket_conf_t, idle_timeout),
    .timer_autoreset = 1,
};


static const nxt_conn_state_t  nxt_h2p_write_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_h2p_conn_sent,
    .error_handler = nxt_h2p_conn_error,

    .timer_handler = nxt_h2p_conn_send_timeout,
    .timer_value = nxt_h2p_conn_send_timer_value,
    .timer_data = offsetof(nxt_socket_conf_t, send_timeout),
    .timer_autoreset = 1,
};


static void
nxt_h2p_conn_read(nxt_task_t *task, void *obj, void *data)
{
    size_t                   size;
    ssize_t                  n;
    nxt_buf_t                *b;
    nxt_conn_t               *c;
    nxt_h2proto_t            *h2c;
    nxt_socket_conf_joint_t  *joint;

    c = obj;
    h2c = data;

    nxt_debug(task, "h2p conn read");

    if (h2c->closing || h2c->session == NULL) {
        return;
    }

    b = c->read;
    size = nxt_buf_mem_used_size(&b->mem);

    if (size != 0) {
        h2c->busy = 1;

        n = nghttp2_session_mem_recv(h2c->session, b->mem.pos, size);

        h2c->busy = 0;

        b->mem.pos = b->mem.start;
        b->mem.free = b->mem.start;

        if (nxt_slow_path(n < 0)) {
            nxt_log(task, NXT_LOG_INFO, "h2p session error: %s",
                    nghttp2_strerror((int) n));

            /*
             * A fatal error: nghttp2 returns it without a GOAWAY of its own
             * and the session must not be used any more, so the GOAWAY is
             * written here.
             */
            switch (n) {

            case NGHTTP2_ERR_FLOODED:
            case NGHTTP2_ERR_TOO_MANY_SETTINGS:
            case NGHTTP2_ERR_TOO_MANY_CONTINUATIONS:
                nxt_h2p_conn_abort(task, h2c, NGHTTP2_ENHANCE_YOUR_CALM);
                break;

            case NGHTTP2_ERR_NOMEM:
            case NGHTTP2_ERR_CALLBACK_FAILURE:
                nxt_h2p_conn_abort(task, h2c, NGHTTP2_INTERNAL_ERROR);
                break;

            default:
                nxt_h2p_conn_abort(task, h2c, NGHTTP2_PROTOCOL_ERROR);
                break;
            }

            return;
        }

        if (nxt_slow_path(h2c->close_pending)) {
            /* Write what is queued, a GOAWAY say, then close. */
            h2c->close_pending = 0;
            nxt_h2p_conn_shutdown(task, h2c);
            return;
        }
    }

    nxt_h2p_conn_flush(task, h2c);

    if (h2c->closing) {
        return;
    }

    if (h2c->stream_count == 0) {
        joint = c->listen->socket.data;

        if (nxt_slow_path(joint == NULL || c->listen->draining)) {
            /* The listener is closed or draining; leave with GOAWAY. */
            nxt_h2p_conn_goaway(h2c, -1);
            nxt_h2p_conn_shutdown(task, h2c);
            return;
        }
    }

    if (!nghttp2_session_want_read(h2c->session)
        && !nghttp2_session_want_write(h2c->session))
    {
        /*
         * The session is over, a GOAWAY has been received or sent (for a
         * connection error, say); what the requests still attached could
         * answer cannot be sent any more.
         */
        nxt_h2p_conn_expire(task, h2c, 1);
        return;
    }

    nxt_conn_read(task->thread->engine, c);
}


/*
 * Pull frames from nghttp2 into buffers on c->write.  One frame per
 * nghttp2_session_mem_send() call; the nghttp2 data provider runs inside it,
 * so the response buffers are copied here into the frame.  Stop when
 * NXT_H2P_WRITE_LIMIT bytes wait to be written; nxt_h2p_conn_sent() calls
 * again when they are out.
 */

static void
nxt_h2p_conn_flush(nxt_task_t *task, nxt_h2proto_t *h2c)
{
    size_t              size;
    ssize_t             n;
    nxt_buf_t           *b, *head, **tail;
    nxt_conn_t          *c;
    const uint8_t       *data;
    nxt_event_engine_t  *engine;

    if (h2c->busy) {
        h2c->flush_pending = 1;
        return;
    }

    if (h2c->session == NULL || h2c->failed) {
        return;
    }

    h2c->flush_pending = 0;

    c = h2c->conn;
    engine = task->thread->engine;

    head = NULL;
    tail = &head;
    b = NULL;

    while (h2c->write_size < NXT_H2P_WRITE_LIMIT) {
        h2c->busy = 1;

        n = nghttp2_session_mem_send(h2c->session, &data);

        h2c->busy = 0;

        if (n <= 0) {
            if (nxt_slow_path(n < 0)) {
                nxt_log(task, NXT_LOG_INFO, "h2p send error: %s",
                        nghttp2_strerror((int) n));
                goto fail;
            }

            break;
        }

        size = n;

        if (b == NULL || (size_t) nxt_buf_mem_free_size(&b->mem) < size) {
            b = nxt_event_engine_buf_mem_alloc(engine,
                                    nxt_max((size_t) NXT_H2P_WRITE_BUFFER_SIZE,
                                            size));
            if (nxt_slow_path(b == NULL)) {
                goto fail;
            }

            *tail = b;
            tail = &b->next;
        }

        b->mem.free = nxt_cpymem(b->mem.free, data, size);
        h2c->write_size += size;
    }

    if (head != NULL) {
        if (c->write == NULL) {
            c->write = head;
            c->write_state = &nxt_h2p_write_state;

            nxt_conn_write(engine, c);

        } else {
            for (b = c->write; b->next != NULL; b = b->next) { /* void */ }

            b->next = head;
        }
    }

    if (nxt_slow_path(h2c->close_pending) && c->write == NULL) {
        /* Otherwise nxt_h2p_conn_sent() closes once the frames are out. */
        nxt_h2p_closing(task, h2c);
    }

    return;

fail:

    while (head != NULL) {
        b = head;
        head = b->next;

        nxt_event_engine_buf_mem_free(engine, b);
    }

    nxt_h2p_conn_fail(task, h2c);
}


static void
nxt_h2p_conn_sent(nxt_task_t *task, void *obj, void *data)
{
    nxt_buf_t           *b;
    nxt_conn_t          *c;
    nxt_h2proto_t       *h2c;
    nxt_event_engine_t  *engine;

    c = obj;
    h2c = data;

    nxt_debug(task, "h2p conn sent");

    if (h2c == NULL) {
        return;
    }

    if (nxt_slow_path(c->socket.fd == -1)) {
        /*
         * nxt_runtime_close_idle_connections() closed the socket under an
         * idle connection at process shutdown; only the session is left.
         */
        if (h2c->session != NULL) {
            nghttp2_session_del(h2c->session);
            h2c->session = NULL;
        }

        return;
    }

    engine = task->thread->engine;

    c->write = nxt_sendbuf_completion(task, &engine->fast_work_queue, c->write);

    h2c->write_size = 0;

    for (b = c->write; b != NULL; b = b->next) {
        h2c->write_size += nxt_buf_mem_used_size(&b->mem);
    }

    if (c->write != NULL) {
        nxt_conn_write(engine, c);
        return;
    }

    if (h2c->session == NULL) {
        return;
    }

    if (h2c->flush_pending || nghttp2_session_want_write(h2c->session)) {
        nxt_h2p_conn_flush(task, h2c);
    }

    if (h2c->closing && c->write == NULL && h2c->stream_count == 0) {
        nxt_h2p_closing(task, h2c);
    }
}


static void
nxt_h2p_conn_close(nxt_task_t *task, void *obj, void *data)
{
    nxt_debug(task, "h2p conn close");

    nxt_h2p_conn_error(task, obj, data);
}


static void
nxt_h2p_conn_error(nxt_task_t *task, void *obj, void *data)
{
    nxt_conn_t     *c;
    nxt_h2proto_t  *h2c;

    c = obj;
    h2c = data;

    nxt_debug(task, "h2p conn error");

    if (h2c == NULL) {
        /* The h2 layer is already gone; the h1 closing chain runs. */
        return;
    }

    nxt_conn_active(task->thread->engine, c);

    nxt_h2p_conn_fail(task, h2c);
}


/*
 * The read timer has two meanings.  With no stream it is idle_timeout, as
 * for an HTTP/1 keep-alive connection.  While some stream still waits for
 * the client (its header block or its request body is not complete) it is
 * a progress timer: it runs from the last frame that advanced any stream,
 * see nxt_h2p_conn_progress(), and nxt_h2p_conn_timer_value() arms it with
 * what is left of the timeout, so bytes that do not advance a stream (PING,
 * WINDOW_UPDATE, SETTINGS, PRIORITY) do not keep a stalled client alive.
 * With streams open and none of them waiting for the client (a request is
 * with its application) there is no read timer; send_timeout covers writes.
 */

static void
nxt_h2p_conn_read_timeout(nxt_task_t *task, void *obj, void *data)
{
    int32_t                  elapsed;
    nxt_msec_t               timeout;
    nxt_conn_t               *c;
    nxt_timer_t              *timer;
    nxt_h2proto_t            *h2c;
    nxt_event_engine_t       *engine;
    nxt_socket_conf_joint_t  *joint;

    timer = obj;

    c = nxt_read_timer_conn(timer);
    h2c = c->socket.data;

    if (h2c == NULL || h2c->closing || h2c->session == NULL) {
        c->block_read = 1;
        return;
    }

    engine = task->thread->engine;

    if (h2c->stream_count == 0) {
        nxt_debug(task, "h2p conn idle timeout");

        c->block_read = 1;

        nxt_conn_active(engine, c);

        nxt_h2p_conn_goaway(h2c, -1);
        nxt_h2p_conn_shutdown(&c->task, h2c);
        return;
    }

    joint = c->listen->socket.data;

    if (nxt_fast_path(joint != NULL)) {
        timeout = nxt_h2p_conn_progress_timeout(h2c, joint->socket_conf);

        if (timeout == 0) {
            /* Every stream has become complete since the timer was armed. */
            return;
        }

        elapsed = nxt_msec_diff(engine->timers.now, h2c->progress);

        if (elapsed >= 0 && (nxt_msec_t) elapsed < timeout) {
            nxt_timer_add(engine, timer, timeout - elapsed);
            return;
        }

        nxt_log(&c->task, NXT_LOG_INFO,
                "h2p client timed out: no stream progress for %M ms",
                timeout);
    }

    nxt_h2p_conn_expire(&c->task, h2c, 0);
}


/*
 * GOAWAY, fail the requests that still wait for the client (all of them if
 * the session can send nothing more), and close once the GOAWAY is written
 * and the other requests have finished.
 */

static void
nxt_h2p_conn_expire(nxt_task_t *task, nxt_h2proto_t *h2c, nxt_bool_t all)
{
    nxt_conn_t  *c;

    c = h2c->conn;

    c->block_read = 1;
    h2c->closing = 1;

    nxt_h2p_conn_goaway(h2c, -1);
    nxt_h2p_conn_flush(task, h2c);

    if (h2c->failed) {
        /* nxt_h2p_conn_fail() has taken over. */
        return;
    }

    nxt_h2p_streams_fail(task, h2c, all);

    if (h2c->failed) {
        if (h2c->stream_count == 0 && c->write == NULL) {
            nxt_h2p_closing(task, h2c);
        }

        return;
    }

    if (h2c->stream_count == 0) {
        nxt_h2p_conn_shutdown(task, h2c);
    }
}


/*
 * Fail the requests attached to streams: all of them, or those that still
 * wait for the client.  A request after EOF ends through its last buffer
 * anyway.  A failed request may close at once and release its stream, and
 * even the last one; h2c->walking keeps nxt_h2p_closing() from releasing
 * the other streams under this walk, and the caller closes afterwards.
 */

static void
nxt_h2p_streams_fail(nxt_task_t *task, nxt_h2proto_t *h2c, nxt_bool_t all)
{
    nxt_queue_link_t  *lnk, *next;
    nxt_h2p_stream_t  *stream;

    h2c->walking++;

    for (lnk = nxt_queue_first(&h2c->streams);
         lnk != nxt_queue_tail(&h2c->streams);
         lnk = next)
    {
        next = nxt_queue_next(lnk);
        stream = nxt_queue_link_data(lnk, nxt_h2p_stream_t, link);

        if (stream->r != NULL && !stream->eof
            && (all || nxt_h2p_stream_incomplete(h2c, stream)))
        {
            nxt_h2p_stream_fail(task, stream);
        }
    }

    h2c->walking--;
}


static void
nxt_h2p_conn_send_timeout(nxt_task_t *task, void *obj, void *data)
{
    nxt_conn_t     *c;
    nxt_timer_t    *timer;
    nxt_h2proto_t  *h2c;

    timer = obj;

    nxt_debug(task, "h2p conn send timeout");

    c = nxt_write_timer_conn(timer);
    c->block_write = 1;
    h2c = c->socket.data;

    if (h2c == NULL) {
        return;
    }

    nxt_h2p_conn_fail(&c->task, h2c);
}


static nxt_msec_t
nxt_h2p_conn_timer_value(nxt_conn_t *c, uintptr_t data)
{
    int32_t                  elapsed;
    nxt_msec_t               timeout;
    nxt_h2proto_t            *h2c;
    nxt_socket_conf_joint_t  *joint;

    joint = c->listen->socket.data;

    if (nxt_slow_path(joint == NULL)) {
        /* The listening socket has been closed. */
        return 1;
    }

    h2c = c->socket.data;

    if (h2c == NULL || h2c->stream_count == 0) {
        return nxt_value_at(nxt_msec_t, joint->socket_conf, data);
    }

    timeout = nxt_h2p_conn_progress_timeout(h2c, joint->socket_conf);

    if (timeout == 0) {
        /* Every request is complete and waits for its application. */
        return 0;
    }

    elapsed = nxt_msec_diff(c->socket.task->thread->engine->timers.now,
                            h2c->progress);

    if (elapsed < 0) {
        return timeout;
    }

    if ((nxt_msec_t) elapsed >= timeout) {
        return 1;
    }

    return timeout - elapsed;
}


/*
 * header_read_timeout while some stream has not finished its header block,
 * as an HTTP/1 request has that long for its header; body_read_timeout
 * while some stream only has its request body to finish, as HTTP/1 has
 * that long between two reads of a body.  0 if no stream waits for the
 * client.
 */

static nxt_msec_t
nxt_h2p_conn_progress_timeout(nxt_h2proto_t *h2c, nxt_socket_conf_t *skcf)
{
    nxt_bool_t        body;
    nxt_queue_link_t  *lnk;
    nxt_h2p_stream_t  *stream;

    body = 0;

    for (lnk = nxt_queue_first(&h2c->streams);
         lnk != nxt_queue_tail(&h2c->streams);
         lnk = nxt_queue_next(lnk))
    {
        stream = nxt_queue_link_data(lnk, nxt_h2p_stream_t, link);

        if (!nxt_h2p_stream_incomplete(h2c, stream)) {
            continue;
        }

        if (!stream->headers_done) {
            return skcf->header_read_timeout;
        }

        body = 1;
    }

    return body ? skcf->body_read_timeout : 0;
}


/* The client has not ended the stream yet: it owes headers or DATA. */

static nxt_bool_t
nxt_h2p_stream_incomplete(nxt_h2proto_t *h2c, nxt_h2p_stream_t *stream)
{
    if (stream->closed || stream->end_stream || h2c->session == NULL) {
        return 0;
    }

    return nghttp2_session_get_stream_remote_close(h2c->session,
                                                   stream->id) == 0;
}


static void
nxt_h2p_conn_progress(nxt_h2proto_t *h2c)
{
    h2c->progress = h2c->conn->socket.task->thread->engine->timers.now;
}


static nxt_msec_t
nxt_h2p_conn_send_timer_value(nxt_conn_t *c, uintptr_t data)
{
    nxt_socket_conf_joint_t  *joint;

    joint = c->listen->socket.data;

    if (nxt_slow_path(joint == NULL)) {
        return 1;
    }

    return nxt_value_at(nxt_msec_t, joint->socket_conf, data);
}


/*
 * The connection is unusable: fail every request that is still attached to
 * a stream and close once the last of them has released its stream, which
 * may happen at once or later through nxt_h2p_request_close().
 */

static void
nxt_h2p_conn_fail(nxt_task_t *task, nxt_h2proto_t *h2c)
{
    nxt_h2p_conn_abort(task, h2c, -1);
}


/*
 * nxt_h2p_conn_fail() that writes a GOAWAY with this error code first,
 * unless goaway is -1.  The frame is built here and not submitted to
 * nghttp2: after a fatal error the session is not to be used any more.
 * The connection closes when the frame is out (nxt_h2p_conn_sent()), or
 * when writing it fails or times out, which comes back here.
 */

static void
nxt_h2p_conn_abort(nxt_task_t *task, nxt_h2proto_t *h2c, int64_t goaway)
{
    nxt_buf_t   *b;
    nxt_conn_t  *c;

    c = h2c->conn;

    if (h2c->session == NULL || h2c->closed) {
        return;
    }

    /* Whatever waits to be written is not going anywhere. */
    if (c->write != NULL) {
        nxt_sendbuf_drain(task, &task->thread->engine->fast_work_queue,
                          c->write);
        c->write = NULL;
        h2c->write_size = 0;
    }

    if (goaway >= 0 && !h2c->failed && c->socket.error == 0
        && !c->socket.closed)
    {
        b = nxt_h2p_goaway_frame(task, h2c, (uint32_t) goaway);

        if (b != NULL) {
            h2c->goaway_sent = 1;

            c->write = b;
            c->write_state = &nxt_h2p_write_state;
            h2c->write_size = nxt_buf_mem_used_size(&b->mem);

            nxt_conn_write(task->thread->engine, c);
        }
    }

    if (!h2c->failed) {
        /*
         * The error handlers below run the discard hook, which flushes;
         * that is a no-op from now on, so a fatal nghttp2 error cannot
         * come back here through nghttp2_session_mem_send().
         */
        h2c->failed = 1;
        h2c->closing = 1;
        c->block_read = 1;

        nxt_h2p_streams_fail(task, h2c, 1);
    }

    if (h2c->stream_count == 0 && c->write == NULL) {
        nxt_h2p_closing(task, h2c);
    }
}


static nxt_buf_t *
nxt_h2p_goaway_frame(nxt_task_t *task, nxt_h2proto_t *h2c, uint32_t error_code)
{
    u_char     *p;
    int32_t    last_id;
    nxt_buf_t  *b;

    b = nxt_event_engine_buf_mem_alloc(task->thread->engine,
                                       NXT_H2P_WRITE_BUFFER_SIZE);
    if (nxt_slow_path(b == NULL)) {
        return NULL;
    }

    last_id = nghttp2_session_get_last_proc_stream_id(h2c->session);

    p = b->mem.free;

    /* RFC 9113, 6.8: a 9-byte frame header and an 8-byte payload. */
    *p++ = 0;
    *p++ = 0;
    *p++ = 8;
    *p++ = NGHTTP2_GOAWAY;
    *p++ = 0;
    p = nxt_cpymem(p, "\0\0\0\0", 4);

    *p++ = (u_char) ((last_id >> 24) & 0x7f);
    *p++ = (u_char) (last_id >> 16);
    *p++ = (u_char) (last_id >> 8);
    *p++ = (u_char) last_id;

    *p++ = (u_char) (error_code >> 24);
    *p++ = (u_char) (error_code >> 16);
    *p++ = (u_char) (error_code >> 8);
    *p++ = (u_char) error_code;

    b->mem.free = p;

    return b;
}


/*
 * A GOAWAY with the last stream this connection serves; -1 means the last
 * stream nghttp2 has processed.  Streams above it are ignored by nghttp2
 * and reopened by the client on another connection.
 */

static void
nxt_h2p_conn_goaway(nxt_h2proto_t *h2c, int32_t last_id)
{
    if (h2c->goaway_sent || h2c->session == NULL) {
        return;
    }

    h2c->goaway_sent = 1;

    if (last_id < 0) {
        last_id = nghttp2_session_get_last_proc_stream_id(h2c->session);
    }

    (void) nghttp2_submit_goaway(h2c->session, NGHTTP2_FLAG_NONE, last_id,
                                 NGHTTP2_NO_ERROR, NULL, 0);
}


/* Write out what nghttp2 still has to say (GOAWAY), then close. */

static void
nxt_h2p_conn_shutdown(nxt_task_t *task, nxt_h2proto_t *h2c)
{
    h2c->closing = 1;

    nxt_h2p_conn_flush(task, h2c);

    if (h2c->conn->write == NULL && h2c->stream_count == 0) {
        nxt_h2p_closing(task, h2c);
    }
}


static void
nxt_h2p_closing(nxt_task_t *task, nxt_h2proto_t *h2c)
{
    nxt_conn_t        *c;
    nxt_queue_link_t  *lnk, *next;
    nxt_h2p_stream_t  *stream;

    if (h2c->busy || h2c->walking != 0) {
        h2c->close_pending = 1;
        return;
    }

    if (h2c->closed) {
        return;
    }

    nxt_debug(task, "h2p closing");

    h2c->closed = 1;
    h2c->close_pending = 0;

    c = h2c->conn;

    if (c->write != NULL) {
        nxt_sendbuf_drain(task, &task->thread->engine->fast_work_queue,
                          c->write);
        c->write = NULL;
    }

    for (lnk = nxt_queue_first(&h2c->streams);
         lnk != nxt_queue_tail(&h2c->streams);
         lnk = next)
    {
        next = nxt_queue_next(lnk);
        stream = nxt_queue_link_data(lnk, nxt_h2p_stream_t, link);

        nxt_h2p_stream_free(h2c, stream);
    }

    if (h2c->session != NULL) {
        nghttp2_session_del(h2c->session);
        h2c->session = NULL;
    }

    nxt_h1p_closing(&c->task, c);
}


static void
nxt_h2p_stream_free(nxt_h2proto_t *h2c, nxt_h2p_stream_t *stream)
{
    nxt_queue_remove(&stream->link);

    nxt_mp_free(h2c->conn->mem_pool, stream);
}


static void
nxt_h2p_stream_fail(nxt_task_t *task, nxt_h2p_stream_t *stream)
{
    nxt_http_request_t  *r;

    if (stream->failed) {
        return;
    }

    stream->failed = 1;

    r = stream->r;

    if (!stream->headers_done) {
        /*
         * The request has not entered the router yet and its error handler
         * closes it at once, which releases the stream and may release the
         * connection under the caller, an nghttp2 callback or a walk over
         * h2c->streams.  Nothing else can move the request on, so the close
         * waits for the work queue as it does in the other states.
         */
        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           r->state->error_handler, &r->task, r, stream);
        return;
    }

    r->state->error_handler(&r->task, r, stream);
}


/* nghttp2 callbacks. */

static int
nxt_h2p_on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame,
    void *user_data)
{
    nxt_conn_t               *c;
    nxt_task_t               *task;
    nxt_h2proto_t            *h2c;
    nxt_socket_conf_t        *skcf;
    nxt_h2p_stream_t         *stream;
    nxt_http_request_t       *r;
    nxt_socket_conf_joint_t  *joint;

    if (frame->hd.type != NGHTTP2_HEADERS
        || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    {
        return 0;
    }

    h2c = user_data;
    c = h2c->conn;
    task = &c->task;

    nxt_debug(task, "h2p begin headers: stream %D", frame->hd.stream_id);

    joint = c->listen->socket.data;

    if (nxt_slow_path(joint == NULL || c->listen->draining
                      || h2c->goaway_sent))
    {
        nxt_h2p_conn_goaway(h2c, -1);

        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    stream = nxt_mp_zalloc(c->mem_pool, sizeof(nxt_h2p_stream_t));
    if (nxt_slow_path(stream == NULL)) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    r = nxt_http_request_create(task);
    if (nxt_slow_path(r == NULL)) {
        nxt_mp_free(c->mem_pool, stream);
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    stream->h2c = h2c;
    stream->r = r;
    stream->id = frame->hd.stream_id;
    stream->out_tail = &stream->out;

    nxt_queue_insert_tail(&h2c->streams, &stream->link);

    if (h2c->stream_count == 0) {
        nxt_conn_active(task->thread->engine, c);
        nxt_timer_disable(task->thread->engine, &c->read_timer);
    }

    nxt_h2p_conn_progress(h2c);

    h2c->stream_count++;
    h2c->requests_total++;

    r->proto.h2 = stream;
    r->protocol = NXT_HTTP_PROTO_H2;
    r->remote = c->remote;
    r->tls = 1;
    r->version = nxt_h2p_version;

    /* See nxt_h1p_conn_request_init() about the request task. */
    r->task = c->task;

    joint->count++;
    r->conf = joint;
    skcf = joint->socket_conf;
    r->log_route = skcf->log_route;

    if (c->local == NULL) {
        c->local = skcf->sockaddr;
    }

    nghttp2_session_set_stream_user_data(session, stream->id, stream);

    if (h2c->requests_total >= NXT_H2P_MAX_REQUESTS) {
        /* This stream is served; the client opens the next one elsewhere. */
        nxt_h2p_conn_goaway(h2c, stream->id);
    }

    NXT_OTEL_TRACE();

    return 0;
}


static int
nxt_h2p_on_header(nghttp2_session *session, const nghttp2_frame *frame,
    const uint8_t *name, size_t namelen, const uint8_t *value,
    size_t valuelen, uint8_t flags, void *user_data)
{
    nxt_str_t           host;
    nxt_int_t           ret;
    nxt_h2proto_t       *h2c;
    nxt_socket_conf_t   *skcf;
    nxt_h2p_stream_t    *stream;
    nxt_http_request_t  *r;

    if (frame->hd.type != NGHTTP2_HEADERS
        || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    {
        return 0;
    }

    stream = nghttp2_session_get_stream_user_data(session,
                                                  frame->hd.stream_id);
    if (stream == NULL || stream->r == NULL) {
        return 0;
    }

    h2c = user_data;
    r = stream->r;
    skcf = r->conf->socket_conf;

    /*
     * The advertised SETTINGS_MAX_HEADER_LIST_SIZE is advisory to the peer,
     * so the size is counted here.  Once a limit is hit the rest of the
     * block is consumed without being stored, so the HPACK state stays in
     * sync and the stream is answered with a status instead of a reset.
     */
    stream->header_list_size += namelen + valuelen + NXT_H2P_FIELD_OVERHEAD;

    if (stream->header_list_size
        > skcf->large_header_buffer_size * skcf->large_header_buffers
        || namelen > NXT_H2P_MAX_FIELD_NAME)
    {
        if (stream->status == 0) {
            stream->status = NXT_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE;
        }

        return 0;
    }

    if (stream->status != 0) {
        return 0;
    }

    if (namelen > 0 && name[0] == ':') {
        /*
         * nghttp2's HTTP messaging layer has already rejected unknown,
         * duplicate and misplaced pseudo-headers.
         */
        if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
            stream->method.length = valuelen;
            stream->method.start = nxt_mp_nget(r->mem_pool, valuelen);
            if (nxt_slow_path(stream->method.start == NULL)) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }

            nxt_memcpy(stream->method.start, value, valuelen);
            r->method = &stream->method;
            return 0;
        }

        if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
            r->target.length = valuelen;
            r->target.start = nxt_mp_nget(r->mem_pool, valuelen);
            if (nxt_slow_path(r->target.start == NULL)) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }

            nxt_memcpy(r->target.start, value, valuelen);
            return 0;
        }

        if (namelen == 10 && memcmp(name, ":authority", 10) == 0) {
            host.length = valuelen;
            host.start = nxt_mp_nget(r->mem_pool, valuelen);
            if (nxt_slow_path(host.start == NULL)) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }

            nxt_memcpy(host.start, value, valuelen);
            stream->authority = host;

            /*
             * ":authority" is stored as a "host" field: applications read
             * the host from HTTP_HOST, and the neutral hash's Host handler
             * validates it with nxt_http_validate_host() and sets r->host,
             * answering 400 to a bad one exactly as for HTTP/1.  A "host"
             * field the client sends as well is reconciled below.
             */
            ret = nxt_h2p_field_add(r, (const uint8_t *) "host", 4, value,
                                    valuelen);
            if (nxt_slow_path(ret != NXT_OK)) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }

            return 0;
        }

        /* ":scheme" is recorded nowhere: "scheme" routing uses r->tls. */
        return 0;
    }

    if (namelen == 4 && memcmp(name, "host", 4) == 0
        && stream->authority.start != NULL)
    {
        /* RFC 9113, 8.3.1: "host" must agree with ":authority". */
        if (stream->authority.length == valuelen
            && nxt_memcasecmp(stream->authority.start, value, valuelen) == 0)
        {
            return 0;
        }

        nxt_log(&h2c->conn->task, NXT_LOG_INFO,
                "h2p stream %D: \"host\" differs from \":authority\"",
                stream->id);

        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    ret = nxt_h2p_field_add(r, name, namelen, value, valuelen);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}


static nxt_int_t
nxt_h2p_field_add(nxt_http_request_t *r, const uint8_t *name, size_t namelen,
    const uint8_t *value, size_t valuelen)
{
    u_char            *p;
    size_t            i;
    uint32_t          hash;
    nxt_http_field_t  *field;

    field = nxt_http_req_field_zero_add(r);
    if (nxt_slow_path(field == NULL)) {
        return NXT_ERROR;
    }

    p = nxt_mp_nget(r->mem_pool, namelen + valuelen);
    if (nxt_slow_path(p == NULL)) {
        return NXT_ERROR;
    }

    /* HPACK names are lower-case already; the hash is over lower-case. */
    hash = NXT_HTTP_FIELD_HASH_INIT;

    for (i = 0; i < namelen; i++) {
        p[i] = nxt_lowcase(name[i]);
        hash = nxt_http_field_hash_char(hash, p[i]);
    }

    field->hash = nxt_http_field_hash_end(hash);
    field->name_length = namelen;
    field->name = p;

    p += namelen;
    nxt_memcpy(p, value, valuelen);

    field->value_length = valuelen;
    field->value = p;

    return NXT_OK;
}


static int
nxt_h2p_on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame,
    void *user_data)
{
    nxt_task_t        *task;
    nxt_h2proto_t     *h2c;
    nxt_h2p_stream_t  *stream;

    h2c = user_data;
    task = &h2c->conn->task;

    switch (frame->hd.type) {

    case NGHTTP2_HEADERS:
        /*
         * A complete request header block, or trailers that end the
         * stream: each can come only once per stream.  nghttp2 resets a
         * stream on trailers without END_STREAM before this callback, so
         * a HEADERS frame cannot refresh the timer again and again.
         */
        if (frame->headers.cat == NGHTTP2_HCAT_REQUEST
            || (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
        {
            nxt_h2p_conn_progress(h2c);
        }

        stream = nghttp2_session_get_stream_user_data(session,
                                                      frame->hd.stream_id);
        if (stream == NULL || stream->r == NULL) {
            return 0;
        }

        if (frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
            stream->headers_done = 1;

            /* Known before the body decision: no DATA follows. */
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                stream->end_stream = 1;
            }

            nxt_h2p_request_headers_done(task, stream);
            break;
        }

        /* Trailers (NGHTTP2_HCAT_HEADERS) are dropped; nothing reads them. */

        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            nxt_h2p_request_end_stream(task, stream);
        }

        break;

    case NGHTTP2_DATA:
        /*
         * Only payload is progress, and nxt_h2p_on_data_chunk_recv()
         * records it as it arrives.  An empty or padding-only DATA frame
         * does not move the body on, so it must not refresh the timer:
         * else a client could hold an incomplete body forever with one
         * such frame before each body_read_timeout.
         */

        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            stream = nghttp2_session_get_stream_user_data(session,
                                                          frame->hd.stream_id);
            if (stream != NULL && stream->r != NULL) {
                nxt_h2p_request_end_stream(task, stream);
            }
        }

        break;

    default:
        break;
    }

    return 0;
}


/*
 * END_HEADERS of a request: the pseudo-headers become the request line,
 * the fields go through the protocol-neutral hash, the body buffer is
 * allocated if DATA may follow, and the request enters the router pipeline.
 */

static void
nxt_h2p_request_headers_done(nxt_task_t *task, nxt_h2p_stream_t *stream)
{
    u_char              *p;
    size_t              size;
    nxt_int_t           ret;
    nxt_http_status_t   status;
    nxt_http_request_t  *r;

    r = stream->r;

    if (stream->status != 0) {
        status = stream->status;
        goto error;
    }

    if (nxt_slow_path(r->method == NULL)) {
        status = NXT_HTTP_BAD_REQUEST;
        goto error;
    }

    if (nxt_slow_path(r->target.start == NULL)) {
        /* CONNECT has no ":path"; there is no tunnel to open. */
        status = nxt_str_eq(r->method, "CONNECT", 7)
                 ? NXT_HTTP_METHOD_NOT_ALLOWED : NXT_HTTP_BAD_REQUEST;
        goto error;
    }

    status = nxt_h2p_request_target(stream, r);
    if (nxt_slow_path(status != NXT_OK)) {
        goto error;
    }

    /* "$request_line" as the access log expects it. */
    size = r->method->length + 1 + r->target.length + 1
           + nxt_h2p_version.length;

    p = nxt_mp_nget(r->mem_pool, size);
    if (nxt_slow_path(p == NULL)) {
        status = NXT_HTTP_INTERNAL_SERVER_ERROR;
        goto error;
    }

    r->request_line.start = p;
    r->request_line.length = size;

    p = nxt_cpymem(p, r->method->start, r->method->length);
    *p++ = ' ';
    p = nxt_cpymem(p, r->target.start, r->target.length);
    *p++ = ' ';
    nxt_memcpy(p, nxt_h2p_version.start, nxt_h2p_version.length);

    if (nxt_slow_path(r->log_route)) {
        nxt_log(task, NXT_LOG_NOTICE, "http request line \"%V\"",
                &r->request_line);
    }

    ret = nxt_http_fields_process(r->inline_fields, r->num_inline_fields,
                                  r->fields, &nxt_http_request_fields_hash, r);
    if (nxt_slow_path(ret != NXT_OK)) {
        status = ret;
        goto error;
    }

    /*
     * DATA may arrive before, or without, the action ever calling
     * body_read, and nghttp2 hands each chunk over exactly once, so the
     * buffer exists from here on.  A length that is not announced grows
     * into a temporary file like an h1 chunked body.
     */
    if (!stream->end_stream && r->content_length_n != 0) {
        if (r->content_length_n > 0) {
            ret = nxt_http_request_body_alloc(&r->task, r,
                                              (size_t) r->content_length_n);

        } else {
            ret = nxt_http_request_body_alloc(&r->task, r, (size_t) -1);
            r->chunked = 1;
        }

        if (nxt_slow_path(ret != NXT_OK)) {
            status = NXT_HTTP_INTERNAL_SERVER_ERROR;
            goto error;
        }
    }

    r->state->ready_handler(&r->task, r, NULL);

    return;

error:

    nxt_http_request_error(&r->task, r, status);
}


static nxt_http_status_t
nxt_h2p_request_target(nxt_h2p_stream_t *stream, nxt_http_request_t *r)
{
    nxt_int_t                 ret;
    nxt_http_request_parse_t  rp;

    if (r->target.length == 0) {
        return NXT_HTTP_BAD_REQUEST;
    }

    if (r->target.start[0] != '/') {
        /* "*" is only valid for OPTIONS (RFC 9113, 8.3.1). */
        if (!(r->target.length == 1 && r->target.start[0] == '*'
              && nxt_str_eq(r->method, "OPTIONS", 7)))
        {
            return NXT_HTTP_BAD_REQUEST;
        }

        stream->path = r->target;
        r->path = &stream->path;
        r->args = &stream->args;

        return NXT_OK;
    }

    /* The same dot-segment and %XX normalization the h1 parser applies. */
    nxt_memzero(&rp, sizeof(nxt_http_request_parse_t));

    rp.mem_pool = r->mem_pool;
    rp.target_start = r->target.start;
    rp.target_end = r->target.start + r->target.length;

    ret = nxt_http_parse_complex_target(&rp);

    if (nxt_slow_path(ret != NXT_OK)) {
        return (ret == NXT_HTTP_PARSE_INVALID) ? NXT_HTTP_BAD_REQUEST
                                               : NXT_HTTP_INTERNAL_SERVER_ERROR;
    }

    stream->path = rp.path;
    stream->args = rp.args;
    r->path = &stream->path;
    r->args = &stream->args;
    r->quoted_target = rp.quoted_target;

    return NXT_OK;
}


static int
nxt_h2p_on_data_chunk_recv(nghttp2_session *session, uint8_t flags,
    int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    size_t              size;
    ssize_t             res;
    nxt_buf_t           *b;
    nxt_task_t          *task;
    nxt_h2proto_t       *h2c;
    nxt_socket_conf_t   *skcf;
    nxt_h2p_stream_t    *stream;
    nxt_http_request_t  *r;

    /* nghttp2 calls this only for payload bytes, never for padding. */
    nxt_h2p_conn_progress(user_data);

    stream = nghttp2_session_get_stream_user_data(session, stream_id);

    if (stream == NULL || stream->r == NULL || stream->body_error) {
        /* Nobody wants it; the automatic window update lets it flow. */
        return 0;
    }

    r = stream->r;
    b = r->body;

    if (b == NULL || len == 0) {
        return 0;
    }

    h2c = user_data;
    task = &h2c->conn->task;
    skcf = r->conf->socket_conf;

    if (nxt_buf_is_file(b)) {
        res = nxt_fd_write(b->file->fd, (void *) data, len);
        if (nxt_slow_path(res < (ssize_t) len)) {
            goto fail;
        }

        b->file_end += len;

        if ((size_t) b->file_end > skcf->max_body_size) {
            nxt_h2p_request_body_error(stream, NXT_HTTP_PAYLOAD_TOO_LARGE);
            return 0;
        }

        return 0;
    }

    size = nxt_buf_mem_free_size(&b->mem);

    if (nxt_slow_path(len > size)) {
        /* nghttp2 checks DATA against content-length; belt and braces. */
        nxt_h2p_request_body_error(stream, NXT_HTTP_BAD_REQUEST);
        return 0;
    }

    b->mem.free = nxt_cpymem(b->mem.free, data, len);

    return 0;

fail:

    nxt_log(task, NXT_LOG_ALERT, "h2p stream %D: body write failed %E",
            stream_id, nxt_errno);

    nxt_h2p_request_body_error(stream, NXT_HTTP_INTERNAL_SERVER_ERROR);

    return 0;
}


/*
 * The body is stored as it arrives, whatever the request is doing.  Only a
 * request that waits for its body in nxt_h2p_request_body_read() can be
 * answered with the error now; any other one gets it when it asks for the
 * body, or never if it does not read the body at all.  The rest of the DATA
 * is dropped either way.
 */

static void
nxt_h2p_request_body_error(nxt_h2p_stream_t *stream, nxt_http_status_t status)
{
    nxt_http_request_t  *r;

    stream->body_error = 1;
    stream->body_status = status;

    if (stream->body_wanted) {
        stream->body_wanted = 0;

        r = stream->r;
        nxt_http_request_error(&r->task, r, status);
    }
}


static void
nxt_h2p_request_end_stream(nxt_task_t *task, nxt_h2p_stream_t *stream)
{
    nxt_buf_t           *b;
    nxt_http_request_t  *r;

    if (stream->end_stream) {
        return;
    }

    stream->end_stream = 1;

    r = stream->r;
    b = r->body;

    if (b != NULL && nxt_buf_is_file(b)) {
        /* Like h1: a complete file body has no memory part. */
        b->mem.start = NULL;
        b->mem.end = NULL;
        b->mem.pos = NULL;
        b->mem.free = NULL;
    }

    if (stream->body_wanted) {
        stream->body_wanted = 0;
        r->state->ready_handler(&r->task, r, NULL);
    }
}


static int
nxt_h2p_on_stream_close(nghttp2_session *session, int32_t stream_id,
    uint32_t error_code, void *user_data)
{
    nxt_h2proto_t     *h2c;
    nxt_h2p_stream_t  *stream;

    stream = nghttp2_session_get_stream_user_data(session, stream_id);

    if (stream == NULL) {
        return 0;
    }

    h2c = user_data;

    nxt_debug(&h2c->conn->task, "h2p stream %D close: %ui", stream_id,
              (nxt_uint_t) error_code);

    stream->closed = 1;

    if (stream->r == NULL) {
        nxt_h2p_stream_free(h2c, stream);
        return 0;
    }

    /*
     * The request is still attached.  After EOF the last buffer's
     * completion is already queued and ends the request normally; before
     * it the stream went away under the request (client RST_STREAM,
     * GOAWAY, a protocol error), which is a request error.  The request
     * close hook frees the stream in both cases.
     */
    if (!stream->eof) {
        nxt_h2p_stream_fail(&h2c->conn->task, stream);
    }

    return 0;
}


/*
 * RFC 9113, 8.1: a server that has sent a complete response before the
 * client has sent the whole request may ask it to stop with RST_STREAM
 * (NO_ERROR); the rest of the body would be read only to be dropped, and
 * the stream would keep the progress timer running.  nghttp2 leaves this
 * to the application.
 */

static int
nxt_h2p_on_frame_send(nghttp2_session *session, const nghttp2_frame *frame,
    void *user_data)
{
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA)
        && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
        && nghttp2_session_get_stream_remote_close(session,
                                                   frame->hd.stream_id) == 0)
    {
        (void) nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                         frame->hd.stream_id,
                                         NGHTTP2_NO_ERROR);
    }

    return 0;
}


/* Request hooks. */

void
nxt_h2p_request_body_read(nxt_task_t *task, nxt_http_request_t *r)
{
    nxt_h2p_stream_t  *stream;

    stream = r->proto.h2;

    nxt_debug(task, "h2p request body read %O", r->content_length_n);

    if (stream->body_error) {
        nxt_http_request_error(task, r, stream->body_status);
        return;
    }

    if (stream->end_stream) {
        r->state->ready_handler(task, r, NULL);
        return;
    }

    /* The body is stored as it arrives; wait for END_STREAM. */
    stream->body_wanted = 1;
}


void
nxt_h2p_request_local_addr(nxt_task_t *task, nxt_http_request_t *r)
{
    r->local = nxt_conn_local_addr(task, r->proto.h2->h2c->conn);
}


static nxt_bool_t
nxt_h2p_connection_field(nxt_http_field_t *f)
{
    /* RFC 9113, 8.2.2: connection-specific fields are not sent. */
    static const nxt_str_t  names[] = {
        nxt_string("Connection"),
        nxt_string("Keep-Alive"),
        nxt_string("Proxy-Connection"),
        nxt_string("Transfer-Encoding"),
        nxt_string("Upgrade"),
    };

    nxt_uint_t  i;

    for (i = 0; i < nxt_nitems(names); i++) {
        if (f->name_length == names[i].length
            && nxt_memcasecmp(f->name, names[i].start, f->name_length) == 0)
        {
            return 1;
        }
    }

    return 0;
}


void
nxt_h2p_request_header_send(nxt_task_t *task, nxt_http_request_t *r,
    nxt_work_handler_t body_handler, void *data)
{
    int                    rc;
    u_char                 *p, status[3];
    size_t                 n, count;
    nxt_buf_t              *last;
    nxt_h2proto_t          *h2c;
    nghttp2_nv             *nva, *nv;
    nxt_h2p_stream_t       *stream;
    nxt_http_field_t       *f;
    nghttp2_data_provider  prd;

    nxt_debug(task, "h2p request header send");

    NXT_OTEL_TRACE();

    stream = r->proto.h2;
    h2c = stream->h2c;

    r->header_sent = 1;

    if (stream->closed || h2c->session == NULL) {
        /* The stream is gone; let the request finish without a peer. */
        goto discard;
    }

    count = 1;

    nxt_http_fields_each(f, r->resp.inline_fields, r->resp.num_inline_fields,
                         r->resp.fields)
    {
        count++;
    } nxt_http_fields_loop;

    nva = nxt_mp_get(r->mem_pool, count * sizeof(nghttp2_nv));
    if (nxt_slow_path(nva == NULL)) {
        goto fail;
    }

    n = r->status;
    status[2] = '0' + n % 10;
    n /= 10;
    status[1] = '0' + n % 10;
    status[0] = '0' + (n / 10) % 10;

    nv = nva;
    nv->name = (uint8_t *) ":status";
    nv->namelen = 7;
    nv->value = status;
    nv->valuelen = 3;
    nv->flags = NGHTTP2_NV_FLAG_NONE;
    nv++;

    nxt_http_fields_each(f, r->resp.inline_fields, r->resp.num_inline_fields,
                         r->resp.fields)
    {
        if (f->skip || nxt_h2p_connection_field(f)) {
            continue;
        }

        p = nxt_mp_nget(r->mem_pool, f->name_length);
        if (nxt_slow_path(p == NULL)) {
            goto fail;
        }

        nxt_memcpy_lowcase(p, f->name, f->name_length);

        nv->name = p;
        nv->namelen = f->name_length;
        nv->value = f->value;
        nv->valuelen = f->value_length;
        nv->flags = NGHTTP2_NV_FLAG_NONE;
        nv++;

    } nxt_http_fields_loop;

    if (r->no_body || body_handler == NULL) {
        /* END_STREAM goes with the HEADERS; no DATA follows. */
        stream->no_provider = 1;
        stream->eof = 1;

        rc = nghttp2_submit_response(h2c->session, stream->id, nva, nv - nva,
                                     NULL);
    } else {
        prd.source.ptr = stream;
        prd.read_callback = nxt_h2p_data_read;

        rc = nghttp2_submit_response(h2c->session, stream->id, nva, nv - nva,
                                     &prd);
    }

    if (nxt_slow_path(rc != 0)) {
        nxt_log(task, NXT_LOG_INFO, "h2p stream %D: submit response: %s",
                stream->id, nghttp2_strerror(rc));
        goto fail;
    }

    stream->submitted = 1;

    if (body_handler != NULL) {
        /*
         * As in h1, the body handler runs before the frames are pulled,
         * since nxt_h2p_conn_flush() is queued behind it or deferred.
         */
        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           body_handler, task, r, data);

    } else {
        last = nxt_http_buf_last(r);

        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           last->completion_handler, &h2c->conn->task, last,
                           last->parent);
    }

    nxt_h2p_conn_flush(&h2c->conn->task, h2c);

    return;

fail:

    nxt_http_request_error_handler(task, r, r->proto.any);

    return;

discard:

    /*
     * Nothing to send to: complete the request through its last buffer so
     * the router closes it; the stream is freed by the close hook.
     */
    if (body_handler != NULL) {
        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           body_handler, task, r, data);

    } else {
        last = nxt_http_buf_last(r);

        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           last->completion_handler, &h2c->conn->task, last,
                           last->parent);
    }

    stream->no_provider = 1;
}


void
nxt_h2p_request_send(nxt_task_t *task, nxt_http_request_t *r, nxt_buf_t *out)
{
    nxt_buf_t         *b;
    nxt_h2proto_t     *h2c;
    nxt_h2p_stream_t  *stream;

    nxt_debug(task, "h2p request send");

    stream = r->proto.h2;
    h2c = stream->h2c;

    *stream->out_tail = out;

    for (b = out; b->next != NULL; b = b->next) { /* void */ }

    stream->out_tail = &b->next;

    if (stream->no_provider || stream->closed || h2c->session == NULL) {
        /* No DATA can be sent; the buffers are only completed. */
        nxt_h2p_stream_drain(&h2c->conn->task, stream);
        return;
    }

    if (stream->deferred) {
        stream->deferred = 0;
        (void) nghttp2_session_resume_data(h2c->session, stream->id);
    }

    nxt_h2p_conn_flush(&h2c->conn->task, h2c);
}


static void
nxt_h2p_stream_drain(nxt_task_t *task, nxt_h2p_stream_t *stream)
{
    nxt_buf_t  *out;

    out = stream->out;
    stream->out = NULL;
    stream->out_tail = &stream->out;

    if (out != NULL) {
        nxt_sendbuf_drain(task, &task->thread->engine->fast_work_queue, out);
    }
}


/*
 * The nghttp2 data provider: copies the response chain into a DATA frame.
 * A drained buffer is completed through the work queue, so a static file
 * refill or a memory buffer release runs after nghttp2 returns.  The last
 * (sync) buffer ends the stream; its completion ends the request.
 */

static ssize_t
nxt_h2p_data_read(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
    size_t length, uint32_t *data_flags, nghttp2_data_source *source,
    void *user_data)
{
    size_t              size, copied;
    nxt_buf_t           *b, *next;
    nxt_task_t          *task;
    nxt_h2proto_t       *h2c;
    nxt_h2p_stream_t    *stream;
    nxt_work_queue_t    *wq;

    stream = source->ptr;
    h2c = stream->h2c;
    task = &h2c->conn->task;
    wq = &task->thread->engine->fast_work_queue;

    copied = 0;

    while (copied < length) {
        b = stream->out;

        if (b == NULL) {
            break;
        }

        if (!nxt_buf_is_sync(b) && nxt_buf_is_mem(b)) {
            size = nxt_min(length - copied,
                           (size_t) nxt_buf_mem_used_size(&b->mem));

            buf = nxt_cpymem(buf, b->mem.pos, size);
            b->mem.pos += size;
            copied += size;

            if (nxt_buf_mem_used_size(&b->mem) != 0) {
                break;
            }
        }

        next = b->next;
        b->next = NULL;
        stream->out = next;

        if (next == NULL) {
            stream->out_tail = &stream->out;
        }

        if (nxt_buf_is_last(b)) {
            stream->eof = 1;
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }

        nxt_work_queue_add(wq, b->completion_handler, task, b, b->parent);

        if (stream->eof) {
            break;
        }
    }

    stream->body_bytes_sent += copied;

    if (copied == 0 && !stream->eof) {
        stream->deferred = 1;
        return NGHTTP2_ERR_DEFERRED;
    }

    return copied;
}


nxt_off_t
nxt_h2p_request_body_bytes_sent(nxt_task_t *task, nxt_http_proto_t proto)
{
    return proto.h2->body_bytes_sent;
}


void
nxt_h2p_request_discard(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *last)
{
    nxt_h2proto_t     *h2c;
    nxt_h2p_stream_t  *stream;

    nxt_debug(task, "h2p request discard");

    stream = r->proto.h2;
    h2c = stream->h2c;

    stream->failed = 1;

    nxt_h2p_stream_drain(&h2c->conn->task, stream);

    if (!stream->closed && h2c->session != NULL) {
        (void) nghttp2_submit_rst_stream(h2c->session, NGHTTP2_FLAG_NONE,
                                         stream->id, NGHTTP2_INTERNAL_ERROR);

        nxt_h2p_conn_flush(&h2c->conn->task, h2c);
    }

    nxt_sendbuf_drain(&h2c->conn->task,
                      &task->thread->engine->fast_work_queue, last);
}


void
nxt_h2p_request_close(nxt_task_t *task, nxt_http_proto_t proto,
    nxt_socket_conf_joint_t *joint)
{
    nxt_conn_t          *c;
    nxt_h2proto_t       *h2c;
    nxt_h2p_stream_t    *stream;
    nxt_event_engine_t  *engine;

    nxt_debug(task, "h2p request close");

    stream = proto.h2;
    h2c = stream->h2c;
    c = h2c->conn;

    stream->r = NULL;

    nxt_router_conf_release(task, joint);

    /* The request task is released with its pool; see nxt_h1p_request_close. */
    task = &c->task;
    engine = task->thread->engine;

    h2c->stream_count--;

    if (stream->closed) {
        nxt_h2p_stream_free(h2c, stream);
    }

    if (h2c->stream_count != 0) {
        nxt_h2p_conn_flush(task, h2c);
        return;
    }

    if (h2c->closing) {
        if (h2c->session != NULL && !h2c->failed && c->socket.error == 0
            && !c->socket.closed)
        {
            /* Write what is queued, the GOAWAY at least, then close. */
            nxt_h2p_conn_goaway(h2c, -1);
            nxt_h2p_conn_shutdown(task, h2c);
            return;
        }

        if (c->write != NULL && c->socket.error == 0 && !c->socket.closed) {
            /*
             * The GOAWAY of nxt_h2p_conn_abort() is on its way;
             * nxt_h2p_conn_sent() closes once it is out.
             */
            return;
        }

        nxt_h2p_closing(task, h2c);
        return;
    }

    if (h2c->goaway_sent) {
        nxt_h2p_conn_shutdown(task, h2c);
        return;
    }

    nxt_h2p_conn_flush(task, h2c);

    if (h2c->closing) {
        return;
    }

    /* No stream left: idle, with the idle timer armed. */
    nxt_conn_idle(engine, c);
    nxt_conn_timer(engine, c, c->read_state, &c->read_timer);
}
