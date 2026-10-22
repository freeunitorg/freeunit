
/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_router_request.h>
#include <nxt_port_memory_int.h>
#include <nxt_port_queue.h>
#include <nxt_websocket.h>
#include <nxt_websocket_header.h>


/* Milliseconds to wait for room in a full application port queue. */
#define NXT_HTTP_WEBSOCKET_RETRY           5
/* The longest wait between two tries of the close message. */
#define NXT_HTTP_WEBSOCKET_LAST_RETRY_MAX  1000


static void nxt_http_websocket_client(nxt_task_t *task, void *obj, void *data);
static void nxt_http_websocket_client_retry(nxt_task_t *task, void *obj,
    void *data);
static nxt_bool_t nxt_http_websocket_queue_full(nxt_port_t *port);
static void nxt_http_websocket_wait(nxt_task_t *task, nxt_http_request_t *r);
static void nxt_http_websocket_error_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_http_websocket_last(nxt_task_t *task, nxt_http_request_t *r);
static void nxt_http_websocket_last_retry(nxt_task_t *task, void *obj,
    void *data);


const nxt_http_request_state_t  nxt_http_websocket
    nxt_aligned(64) =
{
    .ready_handler = nxt_http_websocket_client,
    .error_handler = nxt_http_websocket_error_handler,
};


static void
nxt_http_websocket_client(nxt_task_t *task, void *obj, void *data)
{
    u_char                  *pos;
    size_t                  frame_size, used_size, copy_size, buf_free_size;
    size_t                  chunk_copy_size, rest;
    nxt_buf_t               *out, *buf, **out_tail, *b, *next;
    nxt_int_t               res;
    nxt_http_request_t      *r;
    nxt_request_rpc_data_t  *req_rpc_data;
    nxt_websocket_header_t  *wsh;

    r = obj;
    req_rpc_data = r->req_rpc_data;

    if (nxt_slow_path(req_rpc_data == NULL)) {
        nxt_debug(task, "websocket client frame for destroyed request");

        return;
    }

    nxt_debug(task, "http websocket client frame");

    if (r->ws_paused && nxt_http_websocket_queue_full(req_rpc_data->app_port)) {
        /*
         * Still full: wait again without a copy.  Each copy takes shared
         * memory and copies the whole frame, up to "max_frame_size".
         */
        nxt_http_websocket_wait(task, r);

        return;
    }

    wsh = (nxt_websocket_header_t *) r->ws_frame->mem.pos;

    frame_size = nxt_websocket_frame_header_size(wsh)
                  + nxt_websocket_frame_payload_len(wsh);

    buf = NULL;
    buf_free_size = 0;
    out = NULL;
    out_tail = &out;

    /*
     * Copy the frame into shared memory, but consume nothing of r->ws_frame
     * yet: the frame stays where it is, and only a retry that finds room in
     * the queue copies it again.
     */

    b = r->ws_frame;
    rest = frame_size;

    while (b != NULL && rest > 0) {
        pos = b->mem.pos;
        used_size = b->mem.free - pos;
        copy_size = nxt_min(used_size, rest);

        while (copy_size > 0) {
            if (buf == NULL || buf_free_size == 0) {
                buf_free_size = nxt_min(rest, PORT_MMAP_DATA_SIZE);

                buf = nxt_port_mmap_get_buf(task, &req_rpc_data->app->outgoing,
                                            buf_free_size);
                if (nxt_slow_path(buf == NULL)) {
                    /*
                     * A temporary shortage is not the cause here: the router
                     * grows its own outgoing segments on demand, and this
                     * returns null when that fails.  The alert written while
                     * the buffer was allocated names the reason; this line
                     * names the consequence, the bytes that were wanted, and
                     * the frame they belong to (one buffer holds at most
                     * PORT_MMAP_DATA_SIZE bytes of it).
                     */
                    nxt_alert(task, "websocket: no shared memory buffer for "
                              "%uz bytes of a %uz byte frame, closing the "
                              "connection", buf_free_size, frame_size);

                    while (out != NULL) {
                        buf = out->next;
                        out->next = NULL;
                        out->completion_handler(task, out, out->parent);
                        out = buf;
                    }

                    nxt_http_websocket_error_handler(task, r, r->proto.any);

                    return;
                }

                *out_tail = buf;
                out_tail = &buf->next;
            }

            chunk_copy_size = nxt_min(buf_free_size, copy_size);

            buf->mem.free = nxt_cpymem(buf->mem.free, pos, chunk_copy_size);

            copy_size -= chunk_copy_size;
            pos += chunk_copy_size;
            buf_free_size -= chunk_copy_size;
            rest -= chunk_copy_size;
        }

        b = b->next;
    }

    res = nxt_port_socket_write(task, req_rpc_data->app_port,
                                NXT_PORT_MSG_WEBSOCKET, -1,
                                req_rpc_data->stream,
                                task->thread->engine->port->id, out);
    if (nxt_slow_path(res != NXT_OK)) {
        /*
         * The port layer did not take the chain, so it is still ours to
         * return -- and nothing else will: these are chunks of the
         * application's outgoing shared memory, which no pool teardown
         * reclaims.  A frame lost this way costs the segment capacity
         * permanently, and the application starves once enough have gone.
         *
         * Queued rather than run here, as nxt_port_msg_drop() queues its
         * own: nxt_port_mmap_buf_completion() can post a SHM_ACK, and
         * re-entering the port layer from inside this call is worth
         * avoiding even on the way out.
         */

        while (out != NULL) {
            next = out->next;
            out->next = NULL;

            nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                               out->completion_handler, task, out,
                               out->parent);

            out = next;
        }

        if (res == NXT_AGAIN) {
            /*
             * The application port queue is full: the application reads
             * slower than the client sends.  This is back pressure, not a
             * failure.  Read nothing more from the client, and send the
             * same frame again after a short wait.
             */

            nxt_http_websocket_wait(task, r);

            return;
        }

        /*
         * And fail the connection, the way the allocation failure above
         * does.  A non-OK answer here means the application port is gone or
         * its shared queue is full, so this frame is not reaching the
         * application at all; carrying on would leave the peer's stream
         * short a frame it is never told about, which for a fragmented
         * message is a protocol error the client cannot detect.  This is
         * what the "// TODO: handle" that used to stand here was asking
         * for.
         */

        nxt_http_websocket_error_handler(task, r, r->proto.any);

        return;
    }

    /* The application has the frame: consume it from r->ws_frame. */

    r->ws_paused = 0;

    b = r->ws_frame;

    while (b != NULL && frame_size > 0) {
        used_size = nxt_buf_mem_used_size(&b->mem);
        copy_size = nxt_min(used_size, frame_size);

        b->mem.pos += copy_size;
        frame_size -= copy_size;

        next = b->next;
        b->next = NULL;

        if (nxt_buf_mem_used_size(&b->mem) == 0) {
            nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                               b->completion_handler, task, b, b->parent);

            r->ws_frame = next;
        }

        b = next;
    }

    b = r->ws_frame;

    if (b != NULL) {
        used_size = nxt_buf_mem_used_size(&b->mem);

        if (used_size > 0) {
            nxt_memmove(b->mem.start, b->mem.pos, used_size);

            b->mem.pos = b->mem.start;
            b->mem.free = b->mem.start + used_size;
        }
    }

    nxt_http_request_ws_frame_start(task, r, r->ws_frame);
}


static nxt_bool_t
nxt_http_websocket_queue_full(nxt_port_t *port)
{
    nxt_port_queue_t volatile  *q;

    q = port->queue;

    return q != NULL && nxt_atomic_load(&q->nitems) >= NXT_PORT_QUEUE_SIZE;
}


/*
 * Retry a frame on the request timer.  The upgrade disabled the
 * application deadline, so the timer is free; a release of the request
 * takes it over.  An application that reads nothing for the websocket
 * "read_timeout" fails the connection, as a full queue did before.
 */

static void
nxt_http_websocket_wait(nxt_task_t *task, nxt_http_request_t *r)
{
    nxt_msec_t  now, timeout;

    now = task->thread->engine->timers.now;
    timeout = r->conf->socket_conf->websocket_conf.read_timeout;

    if (!r->ws_paused) {
        r->ws_paused = 1;
        r->ws_paused_at = now;

        nxt_log(task, NXT_LOG_INFO, "websocket: the application "
                "queue is full, reading from the client paused");

    } else if (timeout != 0
               && nxt_msec_diff(now, r->ws_paused_at)
                  >= (nxt_msec_int_t) timeout)
    {
        nxt_log(task, NXT_LOG_INFO, "websocket: the application "
                "read nothing for %M ms, closing", timeout);

        nxt_http_websocket_error_handler(task, r, r->proto.any);

        return;
    }

    /*
     * Bias 0: with the default bias, a timer expires on the next pass of a
     * busy engine, and the retry would poll.
     */
    r->timer.handler = nxt_http_websocket_client_retry;
    r->timer.bias = 0;
    nxt_timer_add(task->thread->engine, &r->timer, NXT_HTTP_WEBSOCKET_RETRY);
}


static void
nxt_http_websocket_client_retry(nxt_task_t *task, void *obj, void *data)
{
    nxt_timer_t         *timer;
    nxt_http_request_t  *r;

    timer = obj;
    r = nxt_timer_data(timer, nxt_http_request_t, timer);

    if (r->proto.any == NULL) {
        nxt_debug(task, "websocket frame retry for closed request");

        return;
    }

    nxt_http_websocket_client(task, r, NULL);
}


static void
nxt_http_websocket_error_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_http_request_t  *r;

    nxt_debug(task, "http websocket error handler");

    r = obj;

    nxt_http_websocket_last(task, r);

    nxt_http_request_close_handler(task, obj, data);
}


/*
 * Tell the application that the connection is over.  A full application
 * queue answers NXT_AGAIN.  Dropped, the message never reaches the
 * application: its request stays open, and the worker cannot quit.  So it
 * is sent again until the queue takes it, the port is gone, or the request
 * is released.  The request outlives the close handler: the router keeps
 * its own reference until nxt_router_http_request_release(), which takes
 * r->timer over.
 *
 * An application that never reads again keeps its queue full.  So the wait
 * before the next try is the time since the queue first refused, from 5 ms
 * up to 1 s: the waits double, and a wedged worker costs one try per second
 * for each closed connection, not 200.
 */

static void
nxt_http_websocket_last(nxt_task_t *task, nxt_http_request_t *r)
{
    nxt_int_t               res;
    nxt_msec_t              now, delay;
    nxt_request_rpc_data_t  *req_rpc_data;

    req_rpc_data = r->req_rpc_data;

    if (req_rpc_data == NULL) {
        nxt_debug(task, "  req_rpc_data is NULL");
        return;
    }

    if (req_rpc_data->app_port == NULL) {
        nxt_debug(task, "  app_port is NULL");
        return;
    }

    res = nxt_port_socket_write(task, req_rpc_data->app_port,
                                NXT_PORT_MSG_WEBSOCKET_LAST,
                                -1, req_rpc_data->stream,
                                task->thread->engine->port->id, NULL);

    if (res == NXT_AGAIN) {
        now = task->thread->engine->timers.now;

        if (!r->ws_paused) {
            r->ws_paused = 1;
            r->ws_paused_at = now;
        }

        delay = nxt_msec_diff(now, r->ws_paused_at);
        delay = nxt_max(delay, NXT_HTTP_WEBSOCKET_RETRY);
        delay = nxt_min(delay, NXT_HTTP_WEBSOCKET_LAST_RETRY_MAX);

        nxt_debug(task, "websocket last: application queue is full, "
                  "next try in %M ms", delay);

        r->timer.handler = nxt_http_websocket_last_retry;
        r->timer.bias = 0;
        nxt_timer_add(task->thread->engine, &r->timer, delay);
    }
}


static void
nxt_http_websocket_last_retry(nxt_task_t *task, void *obj, void *data)
{
    nxt_timer_t         *timer;
    nxt_http_request_t  *r;

    timer = obj;
    r = nxt_timer_data(timer, nxt_http_request_t, timer);

    nxt_http_websocket_last(task, r);
}
