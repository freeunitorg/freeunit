
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_ROUTER_REQUEST_H_INCLUDED_
#define _NXT_ROUTER_REQUEST_H_INCLUDED_


typedef enum {
    /* Not offered to nxt_app_queue_cancel() yet. */
    NXT_MSG_QUEUED = 0,
    /* The router took the message back; no worker will ever see it. */
    NXT_MSG_RETRACTED,
    /* A worker claimed the queue slot first; the request is running. */
    NXT_MSG_CLAIMED,
} nxt_msg_cancel_t;


typedef struct {
    nxt_buf_t                 *buf;
    nxt_fd_t                  body_fd;
    uint32_t                  tracking_cookie;

    /*
     * What nxt_app_queue_cancel() answered, kept because the CAS cannot be
     * asked twice: a win and a loss both leave the queue item's tracking
     * word at 0, so a second call would report a claim that never happened.
     */
    nxt_msg_cancel_t          cancel;
} nxt_msg_info_t;


typedef enum {
    NXT_APR_NEW_PORT,
    NXT_APR_REQUEST_FAILED,
    NXT_APR_GOT_RESPONSE,
    NXT_APR_UPGRADE,
    NXT_APR_CLOSE,
} nxt_apr_action_t;


typedef struct {
    uint32_t                stream;
    nxt_app_t               *app;

    nxt_port_t              *app_port;
    nxt_apr_action_t        apr_action;

    nxt_http_request_t      *request;
    nxt_msg_info_t          msg_info;

    nxt_bool_t              rpc_cancel;
} nxt_request_rpc_data_t;


#endif /* _NXT_ROUTER_REQUEST_H_INCLUDED_ */
