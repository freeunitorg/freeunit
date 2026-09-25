
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_upstream.h>


struct nxt_upstream_proxy_s {
    nxt_sockaddr_t  *sockaddr;
    uint8_t         protocol;
};


static void nxt_http_proxy_server_get(nxt_task_t *task,
    nxt_upstream_server_t *us);
static void nxt_http_proxy_upstream_ready(nxt_task_t *task,
    nxt_upstream_server_t *us);
static void nxt_http_proxy_upstream_error(nxt_task_t *task,
    nxt_upstream_server_t *us);
static nxt_http_action_t *nxt_http_proxy(nxt_task_t *task,
    nxt_http_request_t *r, nxt_http_action_t *action);
static void nxt_http_proxy_header_send(nxt_task_t *task, void *obj, void *data);
static void nxt_http_proxy_header_sent(nxt_task_t *task, void *obj, void *data);
static void nxt_http_proxy_header_read(nxt_task_t *task, void *obj, void *data);
static void nxt_http_proxy_send_body(nxt_task_t *task, void *obj, void *data);
static void nxt_http_proxy_buf_mem_completion(nxt_task_t *task, void *obj,
    void *data);
static void nxt_http_proxy_error(nxt_task_t *task, void *obj, void *data);


static const nxt_http_request_state_t  nxt_http_proxy_header_send_state;
static const nxt_http_request_state_t  nxt_http_proxy_header_sent_state;
static const nxt_http_request_state_t  nxt_http_proxy_header_read_state;
static const nxt_http_request_state_t  nxt_http_proxy_read_state;


static const nxt_upstream_server_proto_t  nxt_upstream_simple_proto = {
    .get = nxt_http_proxy_server_get,
};


static const nxt_upstream_peer_state_t  nxt_upstream_proxy_state = {
    .ready = nxt_http_proxy_upstream_ready,
    .error = nxt_http_proxy_upstream_error,
};


nxt_int_t
nxt_http_proxy_init(nxt_mp_t *mp, nxt_http_action_t *action,
    nxt_http_action_conf_t *acf)
{
    nxt_str_t             name;
    nxt_sockaddr_t        *sa;
    nxt_upstream_t        *up;
    nxt_upstream_proxy_t  *proxy;

    sa = NULL;
    nxt_conf_get_string(acf->proxy, &name);

    if (nxt_str_start(&name, "http://", 7)) {
        name.length -= 7;
        name.start += 7;

        sa = nxt_sockaddr_parse(mp, &name);
        if (nxt_slow_path(sa == NULL)) {
            return NXT_ERROR;
        }

        sa->type = SOCK_STREAM;
    }

    if (sa != NULL) {
        up = nxt_mp_alloc(mp, sizeof(nxt_upstream_t));
        if (nxt_slow_path(up == NULL)) {
            return NXT_ERROR;
        }

        up->name.length = sa->length;
        up->name.start = nxt_sockaddr_start(sa);
        up->proto = &nxt_upstream_simple_proto;

        proxy = nxt_mp_alloc(mp, sizeof(nxt_upstream_proxy_t));
        if (nxt_slow_path(proxy == NULL)) {
            return NXT_ERROR;
        }

        proxy->sockaddr = sa;
        proxy->protocol = NXT_HTTP_PROTO_H1;
        up->type.proxy = proxy;

        action->u.upstream = up;
        action->handler = nxt_http_proxy;
    }

    return NXT_OK;
}


static nxt_http_action_t *
nxt_http_proxy(nxt_task_t *task, nxt_http_request_t *r,
    nxt_http_action_t *action)
{
    nxt_upstream_t  *u;

    u = action->u.upstream;

    nxt_debug(task, "http proxy: \"%V\"", &u->name);

    return nxt_upstream_proxy_handler(task, r, u);
}


nxt_http_action_t *
nxt_upstream_proxy_handler(nxt_task_t *task, nxt_http_request_t *r,
    nxt_upstream_t *upstream)
{
    nxt_http_peer_t        *peer;
    nxt_upstream_server_t  *us;

    us = nxt_mp_zalloc(r->mem_pool, sizeof(nxt_upstream_server_t));
    if (nxt_slow_path(us == NULL)) {
        nxt_http_request_error(task, r, NXT_HTTP_INTERNAL_SERVER_ERROR);
        return NULL;
    }

    peer = nxt_mp_zalloc(r->mem_pool, sizeof(nxt_http_peer_t));
    if (nxt_slow_path(peer == NULL)) {
        nxt_http_request_error(task, r, NXT_HTTP_INTERNAL_SERVER_ERROR);
        return NULL;
    }

    peer->request = r;
    r->peer = peer;

    nxt_mp_retain(r->mem_pool);

    us->state = &nxt_upstream_proxy_state;
    us->peer.http = peer;
    peer->server = us;

    us->upstream = upstream;
    upstream->proto->get(task, us);

    return NULL;
}


static void
nxt_http_proxy_server_get(nxt_task_t *task, nxt_upstream_server_t *us)
{
    nxt_upstream_proxy_t  *proxy;

    proxy = us->upstream->type.proxy;

    us->sockaddr = proxy->sockaddr;
    us->protocol = proxy->protocol;

    us->state->ready(task, us);
}


static void
nxt_http_proxy_upstream_ready(nxt_task_t *task, nxt_upstream_server_t *us)
{
    nxt_http_peer_t  *peer;

    peer = us->peer.http;

    peer->protocol = us->protocol;

    peer->request->state = &nxt_http_proxy_header_send_state;

    nxt_http_proto[peer->protocol].peer_connect(task, peer);
}


static void
nxt_http_proxy_upstream_error(nxt_task_t *task, nxt_upstream_server_t *us)
{
    nxt_http_request_t  *r;

    r = us->peer.http->request;

    nxt_mp_release(r->mem_pool);

    nxt_http_request_error(task, r, NXT_HTTP_BAD_GATEWAY);
}


static const nxt_http_request_state_t  nxt_http_proxy_header_send_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_http_proxy_header_send,
    .error_handler = nxt_http_proxy_error,
};


static void
nxt_http_proxy_header_send(nxt_task_t *task, void *obj, void *data)
{
    nxt_http_peer_t     *peer;
    nxt_http_request_t  *r;

    r = obj;
    peer = data;
    r->state = &nxt_http_proxy_header_sent_state;

    nxt_http_proto[peer->protocol].peer_header_send(task, peer);
}


static const nxt_http_request_state_t  nxt_http_proxy_header_sent_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_http_proxy_header_sent,
    .error_handler = nxt_http_proxy_error,
};


static void
nxt_http_proxy_header_sent(nxt_task_t *task, void *obj, void *data)
{
    nxt_http_peer_t     *peer;
    nxt_http_request_t  *r;

    r = obj;
    peer = data;
    r->state = &nxt_http_proxy_header_read_state;

    nxt_http_proto[peer->protocol].peer_header_read(task, peer);
}


static const nxt_http_request_state_t  nxt_http_proxy_header_read_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_http_proxy_header_read,
    .error_handler = nxt_http_proxy_error,
};


static void
nxt_http_proxy_header_read(nxt_task_t *task, void *obj, void *data)
{
    nxt_http_peer_t     *peer;
    nxt_http_field_t    *f, *field;
    nxt_http_request_t  *r;

    r = obj;
    peer = data;

    r->status = peer->status;

    nxt_debug(task, "http proxy status: %d", peer->status);

    nxt_http_fields_each(field, peer->inline_fields, peer->num_inline_fields,
                         peer->fields)
    {

        nxt_debug(task, "http proxy header: \"%*s: %*s\"",
                  (size_t) field->name_length, field->name,
                  (size_t) field->value_length, field->value);

        if (!field->skip) {
            f = nxt_http_resp_field_add(&r->resp, r->mem_pool);
            if (nxt_slow_path(f == NULL)) {
                nxt_http_proxy_error(task, r, peer);
                return;
            }

            *f = *field;
        }

    } nxt_http_fields_loop;

    r->state = &nxt_http_proxy_read_state;

    nxt_http_request_header_send(task, r, nxt_http_proxy_send_body, peer);
}


static const nxt_http_request_state_t  nxt_http_proxy_read_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_http_proxy_send_body,
    .error_handler = nxt_http_proxy_error,
};


static void
nxt_http_proxy_send_body(nxt_task_t *task, void *obj, void *data)
{
    nxt_buf_t           *out;
    nxt_http_peer_t     *peer;
    nxt_http_request_t  *r;

    r = obj;
    peer = data;
    out = peer->body;

    if (out != NULL) {
        peer->body = NULL;
        nxt_http_request_send(task, r, out);
    }

    if (!peer->closed) {
        nxt_http_proto[peer->protocol].peer_read(task, peer);

    } else {
        nxt_http_proto[peer->protocol].peer_close(task, peer);

        nxt_mp_release(r->mem_pool);
    }
}


nxt_buf_t *
nxt_http_proxy_buf_mem_alloc(nxt_task_t *task, nxt_http_request_t *r,
    size_t size)
{
    nxt_buf_t  *b;

    b = nxt_event_engine_buf_mem_alloc(task->thread->engine, size);
    if (nxt_fast_path(b != NULL)) {
        b->completion_handler = nxt_http_proxy_buf_mem_completion;
        b->parent = r;
        nxt_mp_retain(r->mem_pool);

    } else {
        nxt_http_request_error(task, r, NXT_HTTP_INTERNAL_SERVER_ERROR);
    }

    return b;
}


static void
nxt_http_proxy_buf_mem_completion(nxt_task_t *task, void *obj, void *data)
{
    nxt_buf_t           *b, *next;
    nxt_http_peer_t     *peer;
    nxt_http_request_t  *r;

    b = obj;
    r = data;

    peer = r->peer;

    do {
        next = b->next;

        nxt_http_proxy_buf_mem_free(task, r, b);

        b = next;
    } while (b != NULL);

    if (!peer->closed) {
        nxt_http_proto[peer->protocol].peer_read(task, peer);
    }
}


void
nxt_http_proxy_buf_mem_free(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *b)
{
    nxt_event_engine_buf_mem_free(task->thread->engine, b);

    nxt_mp_release(r->mem_pool);
}


static void
nxt_http_proxy_buf_mem_cleanup(nxt_task_t *task, void *obj, void *data)
{
    nxt_buf_t           *b;
    nxt_event_engine_t  *engine;

    b = obj;
    engine = data;

    /*
     * The engine is carried in "data": nxt_mp_destroy() has none to hand.
     * "task" is passed only so the pointer nxt_mp_cleanup() stores stays
     * inside the pool being destroyed -- do not dereference it here.
     */

    nxt_event_engine_buf_mem_free(engine, b);
}


/*
 * Keep an upstream read buffer alive for the rest of the request without
 * holding the request pool hostage.
 *
 * A buffer that is relayed downstream needs none of this: its completion
 * handler frees it and drops the retain.  One that is *not* relayed does --
 * an upstream header block with no body bytes behind it, and the header of a
 * response that RFC 9112 Sect. 6.3 gives no body at all.  Two things then have
 * to hold at once:
 *
 *   - It must outlive the response.  peer->fields point their name/value into
 *     this buffer (nxt_http_parse_fields) and nxt_http_proxy_header_read()
 *     shallow-copies those field structs into r->resp, where
 *     $response_header_* and "match" conditions read them until the request is
 *     logged and closed.  Returning it to the engine cache when the response
 *     completes would leave them pointing at reusable memory.
 *
 *   - It must not keep the retain nxt_http_proxy_buf_mem_alloc() took.  That
 *     retain is dropped by a buffer completion, and a buffer that is never
 *     relayed has no completion to run -- the pool would never reach zero and
 *     the entire request pool would be stranded.
 *
 * A pool cleanup satisfies both: nxt_mp_destroy() runs it before freeing the
 * pool's own blocks, so the field bytes stay valid for exactly as long as
 * anything can read them, and not one request longer.
 */

nxt_int_t
nxt_http_proxy_buf_mem_hold(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *b)
{
    nxt_int_t  ret;

    /*
     * &r->task, not "task": nxt_mp_cleanup() stores the task pointer in the
     * work item and hands it back at destroy time, and the peer connection's
     * task is freed with that connection well before the request pool.
     * &r->task lives in the pool being destroyed, which nxt_mp_destroy() runs
     * its cleanups before freeing.
     */

    ret = nxt_mp_cleanup(r->mem_pool, nxt_http_proxy_buf_mem_cleanup, &r->task,
                         b, task->thread->engine);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

    /*
     * The request itself holds a retain until nxt_http_request_close_handler(),
     * so this cannot destroy the pool here -- which it must not, the cleanup
     * just registered lives in it.
     */

    nxt_mp_release(r->mem_pool);

    return NXT_OK;
}


static void
nxt_http_proxy_error(nxt_task_t *task, void *obj, void *data)
{
    nxt_http_peer_t     *peer;
    nxt_http_request_t  *r;

    r = obj;
    peer = r->peer;

    if (!peer->closed) {
        nxt_http_proto[peer->protocol].peer_close(task, peer);
        nxt_mp_release(r->mem_pool);
    }

    nxt_http_request_error(&r->task, r, peer->status);
}


nxt_int_t
nxt_http_proxy_date(void *ctx, nxt_http_field_t *field, uintptr_t data)
{
    nxt_http_request_t  *r;

    r = ctx;

    r->resp.date = field;

    return NXT_OK;
}


nxt_int_t
nxt_http_proxy_content_length(void *ctx, nxt_http_field_t *field,
    uintptr_t data)
{
    nxt_off_t           n;
    nxt_http_request_t  *r;

    r = ctx;

    /*
     * A second Content-Length header from the upstream is a classic
     * response-smuggling primitive: the two values disagree on where the
     * body ends, and silently overwriting the first with the second lets
     * a malicious/compromised upstream desync the proxy from the client.
     *
     * Mark the response inconsistent (disables keepalive and closes the
     * connection after this response, same as the parse-error path below)
     * and do not trust either value: skip both Content-Length fields so
     * neither is forwarded to the client, and reset content_length_n to -1
     * so the body is framed by read-to-EOF rather than by an ambiguous
     * advertised length.  Forwarding both headers would let a downstream
     * parser that honours the other value re-frame the body.
     */
    if (r->resp.content_length != NULL) {
        nxt_log(&r->task, NXT_LOG_WARN,
                "upstream sent duplicate Content-Length");

        r->inconsistent = 1;
        r->resp.content_length->skip = 1;
        field->skip = 1;
        r->resp.content_length_n = -1;

        return NXT_OK;
    }

    r->resp.content_length = field;

    n = nxt_off_t_parse(field->value, field->value_length);

    if (nxt_fast_path(n >= 0)) {
        r->resp.content_length_n = n;

    } else {
        /*
         * n == -2 means the upstream Content-Length value overflows
         * nxt_off_t; n == -1 is a generic parse error.  Both are
         * inconsistent with a usable response body length, so log and
         * mark the response inconsistent rather than silently leaving
         * content_length_n at -1.
         */
        nxt_log(&r->task, NXT_LOG_WARN,
                "upstream Content-Length \"%*s\" is invalid",
                (size_t) field->value_length, field->value);

        r->inconsistent = 1;
    }

    return NXT_OK;
}


nxt_int_t
nxt_http_proxy_skip(void *ctx, nxt_http_field_t *field, uintptr_t data)
{
    field->skip = 1;

    return NXT_OK;
}
