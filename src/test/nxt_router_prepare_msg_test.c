
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_router_prepare_msg() (src/nxt_router.c) stores the method and header
 * name lengths as uint8_t.  A 251..255-byte name plus the "HTTP_" prefix,
 * or a method over 255 bytes, used to wrap and be stored truncated; now they
 * are refused with 431 and 501.  The limits themselves are pinned.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_event_engine.h>
#include <nxt_port_memory_int.h>
#include <nxt_unit_request.h>
#include <nxt_checked.h>
#include "nxt_tests.h"


nxt_buf_t *nxt_router_test_prepare_msg(nxt_task_t *task, nxt_http_request_t *r,
    nxt_app_t *app, nxt_bool_t use_http_prefix, nxt_http_status_t *status);


static u_char  nxt_prepare_msg_test_name[300];
static u_char  nxt_prepare_msg_test_method[300];

static nxt_buf_t   *nxt_prepare_msg_test_bufs[8];
static nxt_uint_t  nxt_prepare_msg_test_nbufs;


typedef struct {
    const char         *name;
    nxt_bool_t         prefix;
    size_t             method_length;
    size_t             field_length;
    nxt_http_status_t  status;      /* 0: accepted */
} nxt_prepare_msg_test_case_t;


static const nxt_prepare_msg_test_case_t  nxt_prepare_msg_test_cases[] = {
    { "short name, prefix",              1,   3,   6, 0 },
    { "250-byte name + prefix = 255",    1,   3, 250, 0 },
    { "251-byte name + prefix",          1,   3, 251,
      NXT_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE },
    { "253-byte name + prefix",          1,   3, 253,
      NXT_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE },
    { "255-byte name + prefix",          1,   3, 255,
      NXT_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE },
    { "255-byte name, no prefix",        0,   3, 255, 0 },
    { "255-byte method",                 0, 255,   6, 0 },
    { "256-byte method",                 0, 256,   6,
      NXT_HTTP_NOT_IMPLEMENTED },
    { "280-byte method",                 0, 280,   6,
      NXT_HTTP_NOT_IMPLEMENTED },
};


static nxt_int_t
nxt_prepare_msg_test_case(nxt_task_t *task, nxt_mp_t *mp, nxt_app_t *app,
    nxt_sockaddr_t *sa, const nxt_prepare_msg_test_case_t *tc)
{
    size_t              expect;
    nxt_buf_t           *b;
    nxt_str_t           *method, *args;
    nxt_http_field_t    *field;
    nxt_unit_field_t    *f;
    nxt_http_status_t   status;
    nxt_unit_request_t  *req;
    nxt_http_request_t  *r;

    r = nxt_mp_zalloc(mp, sizeof(nxt_http_request_t));
    method = nxt_mp_zalloc(mp, sizeof(nxt_str_t));
    args = nxt_mp_zalloc(mp, sizeof(nxt_str_t));

    if (r == NULL || method == NULL || args == NULL) {
        return NXT_ERROR;
    }

    method->start = nxt_prepare_msg_test_method;
    method->length = tc->method_length;

    r->method = method;
    nxt_str_set(&r->version, "HTTP/1.1");
    nxt_str_set(&r->server_name, "localhost");
    nxt_str_set(&r->target, "/");
    r->path = &r->target;
    r->args = args;
    r->remote = sa;
    r->local = sa;
    r->content_length_n = -1;

    field = &r->inline_fields[0];
    r->num_inline_fields = 1;

    field->hash = 0;
    field->name = nxt_prepare_msg_test_name;
    field->name_length = tc->field_length;
    field->value = (u_char *) "v";
    field->value_length = 1;

    status = 0;

    b = nxt_router_test_prepare_msg(task, r, app, tc->prefix, &status);

    if (b != NULL
        && nxt_prepare_msg_test_nbufs < nxt_nitems(nxt_prepare_msg_test_bufs))
    {
        nxt_prepare_msg_test_bufs[nxt_prepare_msg_test_nbufs++] = b;
    }

    if (tc->status != 0) {
        NXT_TEST_CHECK(task->log, b == NULL && status == tc->status,
                       "prepare msg test \"%s\": %s, status %d", tc->name,
                       b != NULL ? "accepted" : "refused", (int) status);
        return NXT_OK;
    }

    NXT_TEST_CHECK(task->log, b != NULL, "prepare msg test \"%s\": refused "
                   "with %d", tc->name, (int) status);

    req = (nxt_unit_request_t *) b->mem.pos;
    f = &req->fields[0];

    if (nxt_size_add(tc->field_length, tc->prefix ? nxt_length("HTTP_") : 0,
                     &expect) != 0)
    {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(task->log, req->method_length == tc->method_length
                   && req->fields_count == 1 && f->name_length == expect
                   && nxt_strlen(nxt_unit_sptr_get(&f->name)) == expect,
                   "prepare msg test \"%s\": method_length %d, name_length "
                   "%d, expected %d and %uz", tc->name,
                   (int) req->method_length, (int) f->name_length,
                   (int) tc->method_length, expect);
    return NXT_OK;
}


nxt_int_t
nxt_router_prepare_msg_test(nxt_thread_t *thr)
{
    nxt_mp_t                 *mp;
    nxt_str_t                addr;
    nxt_int_t                ret;
    nxt_app_t                *app;
    nxt_uint_t               i;
    nxt_task_t               *task;
    nxt_sockaddr_t           *sa;
    nxt_event_engine_t       engine, *saved_engine;
    nxt_port_mmap_handler_t  *mmap_handler;

    nxt_thread_time_update(thr);

    task = thr->task;
    task->thread = thr;

    nxt_memset(nxt_prepare_msg_test_name, 'a',
               sizeof(nxt_prepare_msg_test_name));
    nxt_memset(nxt_prepare_msg_test_method, 'A',
               sizeof(nxt_prepare_msg_test_method));

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    app = nxt_mp_zalloc(mp, sizeof(nxt_app_t));
    nxt_str_set(&addr, "127.0.0.1:8080");
    sa = nxt_sockaddr_parse(mp, &addr);

    if (nxt_slow_path(app == NULL || sa == NULL
                      || nxt_thread_mutex_create(&app->outgoing.mutex)
                         != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    nxt_memzero(&engine, sizeof(engine));
    engine.mem_pool = mp;
    engine.task.thread = thr;
    engine.task.log = thr->log;

    saved_engine = thr->engine;
    thr->engine = &engine;

    ret = NXT_OK;
    nxt_prepare_msg_test_nbufs = 0;

    for (i = 0; i < nxt_nitems(nxt_prepare_msg_test_cases); i++) {
        if (nxt_prepare_msg_test_case(task, mp, app, sa,
                                      &nxt_prepare_msg_test_cases[i])
            != NXT_OK)
        {
            ret = NXT_ERROR;
        }
    }

    /*
     * Each accepted case's buffer holds a reference on its app->outgoing
     * segment handler, and the array holds one more.  The buffers are never
     * completed here, so their references are dropped by hand
     * (nxt_port_mmap_handler_use() is private to nxt_port_memory.c); the
     * array's reference, dropped last by nxt_port_mmaps_destroy(), then
     * unmaps and frees each handler.  The buffers themselves come from mp.
     */
    for (i = 0; i < nxt_prepare_msg_test_nbufs; i++) {
        mmap_handler = nxt_prepare_msg_test_bufs[i]->parent;
        (void) nxt_atomic_fetch_add(&mmap_handler->use_count, -1);
    }

    nxt_port_mmaps_destroy(&app->outgoing, 1);

    thr->engine = saved_engine;

    nxt_thread_mutex_destroy(&app->outgoing.mutex);
    nxt_mp_destroy(mp);

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "router prepare msg test "
                      "passed");
    }

    return ret;
}
