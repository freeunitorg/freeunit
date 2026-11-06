
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_H1PROTO_H_INCLUDED_
#define _NXT_H1PROTO_H_INCLUDED_


#include <nxt_main.h>
#include <nxt_http_parse.h>
#include <nxt_http.h>
#include <nxt_router.h>


typedef struct nxt_h1p_websocket_timer_s nxt_h1p_websocket_timer_t;


/*
 * The trailing bytes of a UTF-8 sequence the end of a frame cut in half.
 * A sequence is at most four bytes, so three of them can be incomplete;
 * they are completed by the frame that continues the message.
 */
typedef struct {
    uint8_t   len;
    u_char    tail[3];
} nxt_h1p_ws_utf8_t;


struct nxt_h1proto_s {
    nxt_http_request_parse_t  parser;
    nxt_http_chunk_parse_t    chunked_parse;
    nxt_off_t                 remainder;

    size_t                    nbuffers;
    uint8_t                   header_buffer_slot;
    uint8_t                   large_buffer_slot;
    uint8_t                   keepalive;            /* 1 bit  */
    uint8_t                   chunked;              /* 1 bit  */
    uint8_t                   chunk_sent;           /* 1 bit  */
    uint8_t                   websocket;            /* 1 bit  */
    uint8_t                   connection_upgrade;   /* 1 bit  */
    uint8_t                   upgrade_websocket;    /* 1 bit  */
    uint8_t                   websocket_version_ok; /* 1 bit  */
    nxt_http_te_t             transfer_encoding:8;  /* 2 bits */

    uint8_t                   websocket_cont_expected;  /* 1 bit */
    uint8_t                   websocket_closed;         /* 1 bit */
    uint8_t                   websocket_text;           /* 1 bit */
    uint8_t                   websocket_pong_queued;    /* 1 bit */
    uint8_t                   continue_pending;         /* 1 bit */

    nxt_h1p_ws_utf8_t         websocket_utf8;

    /*
     * Bytes written before the body: a 100 (Continue) and the response
     * header.  $body_bytes_sent does not count them.
     */
    uint32_t                  sent_before_body;

    /*
     * The minimum transfer rate state of the current request.  The
     * keep-alive code zeroes these fields before the next request.
     */
    uint8_t                   body_rate_on;         /* 1 bit  */
    uint8_t                   send_rate_on;         /* 1 bit  */
    nxt_msec_t                body_rate_start;
    nxt_msec_t                send_rate_start;
    uint64_t                  body_rate_bytes;
    uint64_t                  send_rate_bytes;
    uint64_t                  send_rate_time;
    nxt_off_t                 send_rate_sent;

    nxt_http_field_t          *websocket_key;
    nxt_h1p_websocket_timer_t *websocket_timer;
    nxt_buf_t                 *websocket_pong_next;

    nxt_http_request_t        *request;
    nxt_buf_t                 *buffers;

    nxt_buf_t                 **conn_write_tail;
    /*
     * All fields before the conn field will
     * be zeroed in a keep-alive connection.
     */
    nxt_conn_t                *conn;
};

/*
 * The h1proto object is kept in the connection's socket.data, but that slot
 * is not the h1proto's alone: the peer side of a proxied request keeps
 * nxt_http_peer_t there, so that the generic connection handlers hand the
 * peer to the peer handlers (nxt_h1p_peer_connect()), and nxt_h1p_closing()
 * empties the slot.  Every h1proto points back at the connection that owns
 * it -- the accept path and the peer path bind them together, and the
 * keep-alive path zeroes the h1proto up to, never including, conn -- so the
 * object is a h1proto for this connection only when it does.  Reading the
 * slot without this check hands a peer object to code that reads it as a
 * h1proto: the conn field then comes from beyond that object, a zero that
 * the caller dereferences.
 */
typedef struct {
    nxt_conn_t     *owner;   /* offset 0 */
    nxt_h1proto_t  h1p;
} nxt_h1p_slot_t;

nxt_inline nxt_h1proto_t *
nxt_h1p_from_conn(nxt_conn_t *c)
{
    nxt_h1p_slot_t  *slot;

    slot = c->socket.data;

    if (nxt_slow_path(slot == NULL || slot->owner != c)) {
        return NULL;
    }

    return &slot->h1p;
}
void nxt_h1p_shutdown(nxt_task_t *task, nxt_conn_t *c);



#define nxt_h1p_is_http11(h1p)                                              \
    ((h1p)->parser.version.s.minor != '0')


extern nxt_lvlhsh_t  nxt_h1p_fields_hash;

#endif  /* _NXT_H1PROTO_H_INCLUDED_ */
