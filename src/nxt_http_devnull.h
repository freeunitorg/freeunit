
/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_HTTP_DEVNULL_H_INCLUDED_
#define _NXT_HTTP_DEVNULL_H_INCLUDED_


#include <nxt_http.h>


/*
 * NXT_HTTP_PROTO_DEVNULL: a request with no client connection
 * (docs/adr/0004-schedules.md, section 6.4).  The response is counted and
 * its head kept, and every buffer is completed at once.  r->proto.any
 * points at an nxt_http_devnull_t embedded in the owner's object; ->close
 * replaces the connection close, and must release "joint" on this engine.
 */

#define NXT_HTTP_DEVNULL_HEAD  256


typedef struct nxt_http_devnull_s  nxt_http_devnull_t;

struct nxt_http_devnull_s {
    nxt_http_request_t        *request;

    void                      (*close)(nxt_task_t *task,
                                       nxt_http_devnull_t *devnull,
                                       nxt_socket_conf_joint_t *joint);

    nxt_off_t                 body_bytes;
    nxt_http_status_t         status;
    uint8_t                   discarded;  /* 1 bit */

    size_t                    head_length;
    u_char                    head[NXT_HTTP_DEVNULL_HEAD];
};


void nxt_http_devnull_body_read(nxt_task_t *task, nxt_http_request_t *r);
void nxt_http_devnull_local_addr(nxt_task_t *task, nxt_http_request_t *r);
void nxt_http_devnull_header_send(nxt_task_t *task, nxt_http_request_t *r,
    nxt_work_handler_t body_handler, void *data);
void nxt_http_devnull_send(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *out);
nxt_off_t nxt_http_devnull_body_bytes_sent(nxt_task_t *task,
    nxt_http_proto_t proto);
void nxt_http_devnull_discard(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *last);
void nxt_http_devnull_close(nxt_task_t *task, nxt_http_proto_t proto,
    nxt_socket_conf_joint_t *joint);


#endif /* _NXT_HTTP_DEVNULL_H_INCLUDED_ */
