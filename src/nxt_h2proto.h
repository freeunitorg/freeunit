
/*
 * Copyright (C) FreeUnit contributors
 */

#ifndef _NXT_H2PROTO_H_INCLUDED_
#define _NXT_H2PROTO_H_INCLUDED_


#include <nxt_main.h>
#include <nxt_http.h>
#include <nxt_router.h>

#include <nghttp2/nghttp2.h>


/*
 * HTTP/2 over TLS with nghttp2 (ADR 0005, phase 1).
 *
 * Two objects with two lifetimes.  nxt_h2proto_t is one per connection and
 * lives in c->mem_pool as c->socket.data.  nxt_h2p_stream_t is one per
 * HTTP/2 stream, allocated from c->mem_pool too, never from the request
 * pool: nghttp2 keeps it as stream user data until on_stream_close, which
 * may run after the request has finished and its pool has been released.
 * The request's close hook sets stream->r = NULL; whichever of the two
 * ("request closed", "stream closed") comes last frees the stream.
 *
 * The limits below are constants in this release (ADR 0005, "one knob":
 * the listener option is only "tls": {"http2": true}).
 */

#define NXT_H2P_MAX_CONCURRENT_STREAMS  128
#define NXT_H2P_STREAM_WINDOW           (256 * 1024)
#define NXT_H2P_CONN_WINDOW             (1024 * 1024)
#define NXT_H2P_HEADER_TABLE_SIZE       4096
#define NXT_H2P_MAX_REQUESTS            1000
#define NXT_H2P_RST_BURST               1000
#define NXT_H2P_RST_RATE                33
#define NXT_H2P_MAX_CONTINUATIONS       8
#define NXT_H2P_MAX_SETTINGS            32

/* nxt_http_field_t.name_length is a uint8_t. */
#define NXT_H2P_MAX_FIELD_NAME          255

/* RFC 9113, 6.5.2: SETTINGS_MAX_HEADER_LIST_SIZE counts 32 bytes per field. */
#define NXT_H2P_FIELD_OVERHEAD          32

#define NXT_H2P_READ_BUFFER_SIZE        16384

/* One frame (SETTINGS_MAX_FRAME_SIZE 16384 plus the 9-byte header) fits. */
#define NXT_H2P_WRITE_BUFFER_SIZE       (16384 + 9)

/* Frames are not pulled from nghttp2 while this many bytes wait in c->write. */
#define NXT_H2P_WRITE_LIMIT             (64 * 1024)


typedef struct nxt_h2proto_s  nxt_h2proto_t;


struct nxt_h2p_stream_s {
    nxt_h2proto_t               *h2c;
    nxt_http_request_t          *r;
    nxt_queue_link_t            link;      /* nxt_h2proto_t.streams */

    /* Response buffers waiting for the nghttp2 data provider. */
    nxt_buf_t                   *out;
    nxt_buf_t                   **out_tail;

    nxt_off_t                   body_bytes_sent;
    size_t                      header_list_size;

    nxt_str_t                   authority;
    nxt_str_t                   method;
    nxt_str_t                   path;
    nxt_str_t                   args;

    int32_t                     id;

    /* The error answered at END_HEADERS while the block is still consumed. */
    nxt_http_status_t           status:16;

    uint8_t                     deferred;      /* 1 bit */
    uint8_t                     submitted;     /* 1 bit */
    uint8_t                     eof;           /* 1 bit */
    uint8_t                     closed;        /* 1 bit */
    uint8_t                     failed;        /* 1 bit */
    uint8_t                     end_stream;    /* 1 bit */
    uint8_t                     body_wanted;   /* 1 bit */
    uint8_t                     body_error;    /* 1 bit */
    uint8_t                     no_provider;   /* 1 bit */
};


struct nxt_h2proto_s {
    nghttp2_session             *session;
    nxt_conn_t                  *conn;

    nxt_queue_t                 streams;   /* of nxt_h2p_stream_t */

    /* Bytes of nghttp2 output waiting in c->write. */
    size_t                      write_size;

    uint32_t                    stream_count;
    uint32_t                    requests_total;

    uint8_t                     busy;           /* 1 bit */
    uint8_t                     flush_pending;  /* 1 bit */
    uint8_t                     goaway_sent;    /* 1 bit */
    uint8_t                     closing;        /* 1 bit */
    uint8_t                     failed;         /* 1 bit */
};


void nxt_h2p_conn_init(nxt_task_t *task, nxt_conn_t *c);

void nxt_h2p_request_body_read(nxt_task_t *task, nxt_http_request_t *r);
void nxt_h2p_request_local_addr(nxt_task_t *task, nxt_http_request_t *r);
void nxt_h2p_request_header_send(nxt_task_t *task, nxt_http_request_t *r,
    nxt_work_handler_t body_handler, void *data);
void nxt_h2p_request_send(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *out);
nxt_off_t nxt_h2p_request_body_bytes_sent(nxt_task_t *task,
    nxt_http_proto_t proto);
void nxt_h2p_request_discard(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *last);
void nxt_h2p_request_close(nxt_task_t *task, nxt_http_proto_t proto,
    nxt_socket_conf_joint_t *joint);


#endif  /* _NXT_H2PROTO_H_INCLUDED_ */
