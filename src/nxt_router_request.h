
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_ROUTER_REQUEST_H_INCLUDED_
#define _NXT_ROUTER_REQUEST_H_INCLUDED_


#include <nxt_atomic.h>


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
    /*
     * The close of a request that NXT_APR_UPGRADE turned into a websocket.
     * It is the counterpart of that action: it drops the reference the way
     * NXT_APR_CLOSE does and also takes the session off the worker's
     * active_websockets count.
     */
    NXT_APR_WEBSOCKET_CLOSE,
} nxt_apr_action_t;


typedef struct {
    uint32_t                stream;
    nxt_app_t               *app;

    nxt_port_t              *app_port;
    nxt_apr_action_t        apr_action;

    nxt_http_request_t      *request;
    nxt_msg_info_t          msg_info;

    /*
     * The worker's main port and its application, when the router gave up on
     * a request that worker was running.  Each is held by a reference of its
     * own until the worker answers, because neither the request nor its
     * accounting outlives the decision to stop waiting: the port stays out of
     * the idle economy until then and the application has to stay alive for
     * that state to be settled at all.
     */
    nxt_port_t              *abandoned_port;
    nxt_app_t               *abandoned_app;

    nxt_bool_t              rpc_cancel;

    /*
     * Nonzero while ->request is linked in app->ack_waiting_req.  The
     * request's own router thread writes it: set by the park, cleared by the
     * acknowledgement as it takes the request out of that queue
     * (nxt_router_req_headers_ack_handler()) and by
     * nxt_request_rpc_data_unlink() as it decides whether to.  The
     * application's main thread can drain the same queue -- a failed start --
     * but leaves the flag set, so a zero read means the queue no longer
     * points at the request and the unlink can skip the application lock.
     *
     * The unlink clears before it re-checks the queue under the lock, so a
     * zero is visible for that moment while the request is still linked.
     * That window is safe: no other live thread unlinks the same rpc data,
     * and the engine-teardown unlink, the one write from another thread,
     * runs after the request's thread has been joined.
     */
    nxt_atomic_t            app_linked;

    /*
     * Set when the deadline expired once for a request that a worker
     * claimed from the shared queue and did not acknowledge.  See
     * nxt_router_app_timeout().
     */
    uint8_t                 claimed_expired;
} nxt_request_rpc_data_t;


#endif /* _NXT_ROUTER_REQUEST_H_INCLUDED_ */
