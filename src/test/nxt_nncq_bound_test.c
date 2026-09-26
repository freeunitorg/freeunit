/*
 * Copyright (C) F5, Inc.
 */

/*
 * The shared-memory queues (src/nxt_nncq.h, src/nxt_app_nncq.h) live in
 * memory the application maps writable.  A peer that writes a foreign cycle
 * under head or tail used to keep the router retrying forever.  Each case
 * poisons one queue and runs the router-side operation in a child under
 * alarm(): it must return, reporting the queue empty or the send failed.
 */

#include <nxt_main.h>
#include <nxt_port_queue.h>
#include <nxt_app_queue.h>
#include "nxt_tests.h"

#include <sys/mman.h>


static const uint8_t  nxt_nncq_bound_test_msg[1] = { 0x5A };


static int
nxt_nncq_bound_test_port_recv(void *mem)
{
    uint8_t           buf[NXT_PORT_QUEUE_MSG_SIZE];
    nxt_port_queue_t  *q;

    q = mem;
    q->queue.entries[0] = 7 * NXT_NNCQ_SIZE;

    return nxt_port_queue_recv(q, buf) == -1;
}


static int
nxt_nncq_bound_test_port_send(nxt_port_queue_t *q, nxt_nncq_t *poison)
{
    int  notify;

    poison->entries[0] = 7 * NXT_NNCQ_SIZE;

    return nxt_port_queue_send(q, nxt_nncq_bound_test_msg, 1, &notify);
}


static int
nxt_nncq_bound_test_port_send_full(void *mem)
{
    nxt_port_queue_t  *q = mem;

    return nxt_nncq_bound_test_port_send(q, &q->free_items) == NXT_AGAIN;
}


/* The refused message must give its slot back: all of them stay free. */

static int
nxt_nncq_bound_test_port_send_slot(void *mem)
{
    nxt_uint_t        n;
    nxt_port_queue_t  *q = mem;

    if (nxt_nncq_bound_test_port_send(q, &q->queue) != NXT_ERROR) {
        return 0;
    }

    for (n = 0; nxt_nncq_dequeue(&q->free_items)
                != nxt_nncq_empty(&q->free_items); n++);

    return n == NXT_PORT_QUEUE_SIZE;
}


static int
nxt_nncq_bound_test_app_send(nxt_app_queue_t *q, nxt_app_nncq_t *poison)
{
    int       notify;
    uint32_t  cookie;

    poison->entries[0] = 7 * NXT_APP_NNCQ_SIZE;

    return nxt_app_queue_send(q, nxt_nncq_bound_test_msg, 1, 1, &notify,
                              &cookie);
}


static int
nxt_nncq_bound_test_app_send_full(void *mem)
{
    nxt_app_queue_t  *q = mem;

    return nxt_nncq_bound_test_app_send(q, &q->free_items) == NXT_AGAIN;
}


static int
nxt_nncq_bound_test_app_send_slot(void *mem)
{
    nxt_uint_t       n;
    nxt_app_queue_t  *q = mem;

    if (nxt_nncq_bound_test_app_send(q, &q->queue) != NXT_ERROR) {
        return 0;
    }

    for (n = 0; nxt_app_nncq_dequeue(&q->free_items)
                != nxt_app_nncq_empty(&q->free_items); n++);

    return n == NXT_APP_QUEUE_SIZE;
}


typedef struct {
    const char  *name;
    nxt_bool_t  app;
    int         (*op)(void *q);
} nxt_nncq_bound_test_case_t;


static const nxt_nncq_bound_test_case_t  nxt_nncq_bound_test_cases[] = {
    { "port recv", 0, nxt_nncq_bound_test_port_recv },
    { "port send, poisoned free list", 0, nxt_nncq_bound_test_port_send_full },
    { "port send, poisoned slot", 0, nxt_nncq_bound_test_port_send_slot },
    { "app send, poisoned free list", 1, nxt_nncq_bound_test_app_send_full },
    { "app send, poisoned slot", 1, nxt_nncq_bound_test_app_send_slot },
};


static void  *nxt_nncq_bound_test_mem;


static int
nxt_nncq_bound_test_child(void *data)
{
    const nxt_nncq_bound_test_case_t  *tc = data;

    alarm(5);

    return !tc->op(nxt_nncq_bound_test_mem);
}


nxt_int_t
nxt_nncq_bound_test(nxt_thread_t *thr)
{
    int         rc;
    size_t      size;
    nxt_uint_t  i;

    const nxt_nncq_bound_test_case_t  *tc;

    nxt_thread_time_update(thr);

    for (i = 0; i < nxt_nitems(nxt_nncq_bound_test_cases); i++) {
        tc = &nxt_nncq_bound_test_cases[i];
        size = tc->app ? sizeof(nxt_app_queue_t) : sizeof(nxt_port_queue_t);

        nxt_nncq_bound_test_mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANON, -1, 0);
        if (nxt_nncq_bound_test_mem == MAP_FAILED) {
            nxt_log_alert(thr->log, "nncq bound test mmap failed %E",
                          nxt_errno);
            return NXT_ERROR;
        }

        if (tc->app) {
            nxt_app_queue_init(nxt_nncq_bound_test_mem);

        } else {
            nxt_port_queue_init(nxt_nncq_bound_test_mem);
        }

        rc = nxt_test_in_child(thr, tc->name, nxt_nncq_bound_test_child,
                               (void *) tc);

        munmap(nxt_nncq_bound_test_mem, size);

        NXT_TEST_CHECK(thr->log, rc == 0, "nncq bound test: %s did not "
                       "return the failure", tc->name);
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nncq bound test passed");

    return NXT_OK;
}
