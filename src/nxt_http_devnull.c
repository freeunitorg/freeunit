
/*
 * Copyright (C) FreeUnit contributors.
 */

/* The NXT_HTTP_PROTO_DEVNULL slot of nxt_http_proto[]; see the header. */

#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_http_devnull.h>
#include <nxt_otel.h>


/* No body: ready at once. */

void
nxt_http_devnull_body_read(nxt_task_t *task, nxt_http_request_t *r)
{
    r->state->ready_handler(task, r, NULL);
}


/* The owner sets r->local before the request starts. */

void
nxt_http_devnull_local_addr(nxt_task_t *task, nxt_http_request_t *r)
{
}


/* As h1 does, the body handler is queued, never called in place. */

void
nxt_http_devnull_header_send(nxt_task_t *task, nxt_http_request_t *r,
    nxt_work_handler_t body_handler, void *data)
{
    nxt_buf_t           *last;
    nxt_http_devnull_t  *dn;

    nxt_debug(task, "devnull request header send: %d", (int) r->status);

    NXT_OTEL_TRACE();

    dn = r->proto.any;
    dn->status = r->status;

    r->header_sent = 1;

    if (body_handler != NULL) {
        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           body_handler, task, r, data);
        return;
    }

    last = nxt_http_buf_last(r);

    if (last != NULL) {
        nxt_sendbuf_drain(task, &task->thread->engine->fast_work_queue, last);
    }
}


/* Completing the sync "last" buffer is what ends the request. */

void
nxt_http_devnull_send(nxt_task_t *task, nxt_http_request_t *r, nxt_buf_t *out)
{
    size_t              size, n;
    nxt_buf_t           *b;
    nxt_http_devnull_t  *dn;

    dn = r->proto.any;

    for (b = out; b != NULL; b = b->next) {
        if (nxt_buf_is_sync(b)) {
            continue;
        }

        size = nxt_buf_used_size(b);
        dn->body_bytes += size;

        if (nxt_buf_is_mem(b) && dn->head_length < NXT_HTTP_DEVNULL_HEAD) {
            n = nxt_min(size, NXT_HTTP_DEVNULL_HEAD - dn->head_length);

            nxt_memcpy(dn->head + dn->head_length, b->mem.pos, n);
            dn->head_length += n;
        }
    }

    nxt_debug(task, "devnull request send: %O body bytes", dn->body_bytes);

    nxt_sendbuf_drain(task, &task->thread->engine->fast_work_queue, out);
}


nxt_off_t
nxt_http_devnull_body_bytes_sent(nxt_task_t *task, nxt_http_proto_t proto)
{
    nxt_http_devnull_t  *dn;

    dn = proto.any;

    return dn->body_bytes;
}



void
nxt_http_devnull_discard(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *last)
{
    nxt_http_devnull_t  *dn;

    nxt_debug(task, "devnull request discard");

    dn = r->proto.any;
    dn->discarded = 1;

    if (last != NULL) {
        nxt_sendbuf_drain(task, &task->thread->engine->fast_work_queue, last);
    }
}


/* The request pool goes as soon as this returns. */

void
nxt_http_devnull_close(nxt_task_t *task, nxt_http_proto_t proto,
    nxt_socket_conf_joint_t *joint)
{
    nxt_http_devnull_t  *dn;

    nxt_debug(task, "devnull request close");

    dn = proto.any;

    if (dn->request != NULL) {
        dn->status = dn->request->status;
        dn->request = NULL;
    }

    dn->close(task, dn, joint);
}
