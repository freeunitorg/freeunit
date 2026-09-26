
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_router_prepare_msg() (src/nxt_router.c) stores the method and header
 * name lengths as uint8_t.  A 251..255-byte name plus the "HTTP_" prefix,
 * or a method over 255 bytes, used to wrap and be stored truncated; now they
 * are refused with 431 and 501.  The limits themselves are pinned, and so
 * is nxt_app_msg_prefix[]: which app types get the prefix and which do not.
 *
 * The request is built by hand with one field; the engine is a stub that
 * only provides the memory pool the buffer is allocated from.  The cases
 * run in a child process: the accepted cases' buffers hold shared memory
 * that only a real engine can release, and the child's exit frees it.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_event_engine.h>
#include <nxt_port_memory_int.h>
#include <nxt_unit_request.h>
#include "nxt_tests.h"


static u_char  nxt_prepare_msg_test_name[300];
static u_char  nxt_prepare_msg_test_method[300];


typedef struct {
    const char         *name;
    nxt_app_type_t     type;
    size_t             method_length;
    size_t             field_length;
    nxt_http_status_t  status;      /* 0: accepted */
} nxt_prepare_msg_test_case_t;


#define NXT_TEST_431  NXT_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE
#define NXT_TEST_501  NXT_HTTP_NOT_IMPLEMENTED


static const nxt_prepare_msg_test_case_t  nxt_prepare_msg_test_cases[] = {
    { "short name, php",             NXT_APP_PHP,        3,   6, 0 },
    { "250-byte name + prefix",      NXT_APP_PHP,        3, 250, 0 },
    { "251-byte name + prefix",      NXT_APP_PHP,        3, 251, NXT_TEST_431 },
    { "251-byte name, perl",         NXT_APP_PERL,       3, 251, NXT_TEST_431 },
    { "251-byte name, ruby",         NXT_APP_RUBY,       3, 251, NXT_TEST_431 },
    { "253-byte name + prefix",      NXT_APP_PHP,        3, 253, NXT_TEST_431 },
    { "255-byte name + prefix",      NXT_APP_PHP,        3, 255, NXT_TEST_431 },
    { "255-byte name, external",     NXT_APP_EXTERNAL,   3, 255, 0 },
    { "255-byte name, python",       NXT_APP_PYTHON,     3, 255, 0 },
    { "255-byte name, java",         NXT_APP_JAVA,       3, 255, 0 },
    { "255-byte name, wasm",         NXT_APP_WASM,       3, 255, 0 },
    { "255-byte name, wasm-wc",      NXT_APP_WASM_WC,    3, 255, 0 },
    { "255-byte method",             NXT_APP_PYTHON,   255,   6, 0 },
    { "256-byte method",             NXT_APP_PYTHON,   256,   6, NXT_TEST_501 },
    { "280-byte method",             NXT_APP_PYTHON,   280,   6, NXT_TEST_501 },
};


/* The app types whose header names carry "HTTP_"; the rest get none. */

static nxt_bool_t
nxt_prepare_msg_test_prefixed(nxt_app_type_t type)
{
    return (type == NXT_APP_PHP || type == NXT_APP_PERL
            || type == NXT_APP_RUBY);
}


static nxt_int_t
nxt_prepare_msg_test_case(nxt_task_t *task, nxt_mp_t *mp, nxt_app_t *app,
    nxt_sockaddr_t *sa, const nxt_prepare_msg_test_case_t *tc)
{
    u_char              *name;
    size_t              expect;
    nxt_buf_t           *b;
    nxt_str_t           *method, *args;
    nxt_uint_t          status;
    nxt_bool_t          prefixed;
    nxt_http_field_t    *field;
    nxt_unit_field_t    *f;
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

    app->type = tc->type;
    status = 0;

    b = nxt_router_test_prepare_msg(task, r, app, &status);

    if (b == NULL) {
        if (status != (nxt_uint_t) tc->status) {
            nxt_log_alert(task->log, "prepare msg test \"%s\": refused "
                          "with %d, expected %d", tc->name, (int) status,
                          (int) tc->status);
            return NXT_ERROR;
        }

        return NXT_OK;
    }

    req = (nxt_unit_request_t *) b->mem.pos;
    f = &req->fields[0];
    name = nxt_unit_sptr_get(&f->name);

    prefixed = nxt_prepare_msg_test_prefixed(tc->type);
    expect = tc->field_length + (prefixed ? nxt_length("HTTP_") : 0);

    if (tc->status != 0
        || req->method_length != tc->method_length
        || req->fields_count != 1
        || f->name_length != expect
        || nxt_strlen(name) != expect
        || (nxt_strncmp(name, "HTTP_", nxt_length("HTTP_")) == 0) != prefixed)
    {
        nxt_log_alert(task->log, "prepare msg test \"%s\": accepted with "
                      "method_length %d, name_length %d; expected %d and "
                      "%uz, or status %d", tc->name, (int) req->method_length,
                      (int) f->name_length, (int) tc->method_length, expect,
                      (int) tc->status);
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_prepare_msg_test_run(nxt_thread_t *thr)
{
    nxt_mp_t            *mp;
    nxt_str_t           addr;
    nxt_int_t           ret;
    nxt_app_t           *app;
    nxt_uint_t          i;
    nxt_task_t          *task;
    nxt_sockaddr_t      *sa;
    nxt_event_engine_t  engine, *saved_engine;

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

    for (i = 0; i < nxt_nitems(nxt_prepare_msg_test_cases); i++) {
        if (nxt_prepare_msg_test_case(task, mp, app, sa,
                                      &nxt_prepare_msg_test_cases[i])
            != NXT_OK)
        {
            ret = NXT_ERROR;
        }
    }

    thr->engine = saved_engine;

    return ret;
}


nxt_int_t
nxt_router_prepare_msg_test(nxt_thread_t *thr)
{
    int    status;
    pid_t  child;

    child = fork();

    if (child == 0) {
        _exit(nxt_prepare_msg_test_run(thr) == NXT_OK ? 0 : 1);
    }

    if (nxt_slow_path(child == -1 || waitpid(child, &status, 0) != child)) {
        nxt_log_alert(thr->log, "router prepare msg test: fork() failed %E",
                      nxt_errno);
        return NXT_ERROR;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        nxt_log_alert(thr->log, "router prepare msg test failed, "
                      "status %d", status);
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router prepare msg test passed");

    return NXT_OK;
}
