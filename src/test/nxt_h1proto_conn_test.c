/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * The slot that carries a h1proto is shared: the peer side of a proxied
 * request keeps nxt_http_peer_t in the connection's socket.data, so that the
 * generic connection handlers hand the peer to the peer handlers, and
 * nxt_h1p_closing() empties the slot.  A h1proto keeps the connection that
 * owns it in conn, so the object is a h1proto for this connection only when
 * it points back at it.
 *
 * Reading the slot without that check is what crashed the router in
 * production.  nxt_h1p_complete_buffers() takes the connection to complete
 * the buffers on from the object it is given,
 *
 *     c = h1p->conn;
 *     in = c->read;
 *
 * and it reached that read with a valid object pointer and a zero conn --
 * a read of 0x50, offsetof(nxt_conn_t, read).  Before the fix this test
 * dies with SIGSEGV on that read.  The assertion is that the completion
 * call returns for an object that is not a h1proto of a live connection,
 * and that the slot is read as a h1proto only for the connection that owns
 * it.
 */

#include <nxt_main.h>
#include <nxt_h1proto.h>
#include "nxt_tests.h"


nxt_int_t
nxt_h1proto_conn_test(nxt_thread_t *thr)
{
    nxt_conn_t         c;
    nxt_h1p_slot_t     h1p;
    nxt_h1p_slot_t     foreign;

    nxt_memzero(&h1p, sizeof(nxt_h1p_slot_t));
    nxt_memzero(&foreign, sizeof(nxt_h1p_slot_t));
    nxt_memzero(&c, sizeof(nxt_conn_t));

    /*
     * The call that crashed.  The object has no connection, so there are no
     * buffers to complete and nothing to read.
     */
    nxt_h1p_complete_buffers(thr->task, &h1p.h1p, 1);
    nxt_h1p_complete_buffers(thr->task, &h1p.h1p, 0);

    /* An empty slot holds no h1proto. */

    if (nxt_h1p_from_conn(&c) != NULL) {
        nxt_log_alert(thr->log,
                      "nxt_h1proto conn test: empty slot is not NULL");
        return NXT_ERROR;
    }

    /* A foreign object in the slot is no h1proto for this connection. */

    foreign.owner = (nxt_conn_t *) 1;
    c.socket.data = &foreign;

    if (nxt_h1p_from_conn(&c) != NULL) {
        nxt_log_alert(thr->log,
                      "nxt_h1proto conn test: foreign object is accepted");
        return NXT_ERROR;
    }

    /* The h1proto that points back at this connection is returned. */

    h1p.owner = &c;
    h1p.h1p.conn = &c;
    c.socket.data = &h1p;

    if (nxt_h1p_from_conn(&c) != &h1p.h1p) {
        nxt_log_alert(thr->log,
                      "nxt_h1proto conn test: h1proto is not returned");
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_h1proto conn test passed");

    return NXT_OK;
}
