
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
 * The request is built by hand with one field, and in some cases a second
 * one marked skip, which the router must leave out of the message and its
 * size; the engine is a stub that only provides the memory pool the buffer
 * is allocated from.  The cases
 * run in a child process: the accepted cases' buffers hold shared memory
 * that only a real engine can release, and the child's exit frees it.
 * nxt_prepare_msg_test_duplicates() below checks how the lines of one
 * field name are joined.
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
    size_t             skipped_length; /* 0: no skipped field */
    nxt_http_status_t  status;         /* 0: accepted */
} nxt_prepare_msg_test_case_t;


#define NXT_TEST_431  NXT_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE
#define NXT_TEST_501  NXT_HTTP_NOT_IMPLEMENTED


static const nxt_prepare_msg_test_case_t  nxt_prepare_msg_test_cases[] = {
    { "short name, php",          NXT_APP_PHP,        3,   6,   0, 0 },
    { "250-byte name + prefix",   NXT_APP_PHP,        3, 250,   0, 0 },
    { "251-byte name + prefix",   NXT_APP_PHP,        3, 251,   0,
      NXT_TEST_431 },
    { "251-byte name, perl",      NXT_APP_PERL,       3, 251,   0,
      NXT_TEST_431 },
    { "251-byte name, ruby",      NXT_APP_RUBY,       3, 251,   0,
      NXT_TEST_431 },
    { "253-byte name + prefix",   NXT_APP_PHP,        3, 253,   0,
      NXT_TEST_431 },
    { "255-byte name + prefix",   NXT_APP_PHP,        3, 255,   0,
      NXT_TEST_431 },
    { "255-byte name, external",  NXT_APP_EXTERNAL,   3, 255,   0, 0 },
    { "255-byte name, python",    NXT_APP_PYTHON,     3, 255,   0, 0 },
    { "255-byte name, java",      NXT_APP_JAVA,       3, 255,   0, 0 },
    { "255-byte name, wasm",      NXT_APP_WASM,       3, 255,   0, 0 },
    { "255-byte name, wasm-wc",   NXT_APP_WASM_WC,    3, 255,   0, 0 },
    { "255-byte method",          NXT_APP_PYTHON,   255,   6,   0, 0 },
    { "256-byte method",          NXT_APP_PYTHON,   256,   6,   0,
      NXT_TEST_501 },
    { "280-byte method",          NXT_APP_PYTHON,   280,   6,   0,
      NXT_TEST_501 },
    { "251-byte skipped name",    NXT_APP_PHP,        3,   6, 251, 0 },
    { "255-byte skipped name",    NXT_APP_PHP,        3,   6, 255, 0 },
};


/* The app types whose header names carry "HTTP_"; the rest get none. */

static nxt_bool_t
nxt_prepare_msg_test_prefixed(nxt_app_type_t type)
{
    return (type == NXT_APP_PHP || type == NXT_APP_PERL
            || type == NXT_APP_RUBY);
}


/* On success *used is the size of the built message, 0 when refused. */

static nxt_int_t
nxt_prepare_msg_test_case(nxt_task_t *task, nxt_mp_t *mp, nxt_app_t *app,
    nxt_sockaddr_t *sa, const nxt_prepare_msg_test_case_t *tc, size_t *used)
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

    if (tc->skipped_length != 0) {
        field = &r->inline_fields[1];
        r->num_inline_fields = 2;

        field->hash = 0;
        field->skip = 1;
        field->name = nxt_prepare_msg_test_name;
        field->name_length = tc->skipped_length;
        field->value = (u_char *) "v";
        field->value_length = 1;
    }

    app->type = tc->type;
    *used = 0;
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

    *used = nxt_buf_mem_used_size(&b->mem);

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


/*
 * For PHP, Perl and Ruby the lines of one field name are joined into the
 * first line of that name.  For the other app types they are passed as
 * they are.  A case gives the request lines in order and the lines that
 * r->content_length, r->content_type, r->cookie and r->authorization point
 * at.  The h1 field handlers leave each one at the last line of its name
 * (nxt_http_request_field() in src/nxt_http_request.c).  The expected
 * message lists the fields in order and the four indices, with -1 for
 * NXT_UNIT_NONE_FIELD.  As in the parser, the first 16 lines go to
 * r->inline_fields and the others to the r->fields list.  The call must
 * leave the request lines as they were: the skip flags as before, and no
 * merged mark.
 */

#define NXT_PMT_IN   24
#define NXT_PMT_OUT  20


typedef struct {
    const char      *name;
    const char      *value;
    uint8_t         skip;
} nxt_pmt_line_t;


typedef struct {
    const char      *name;
    nxt_bool_t      prefixed;  /* PHP, Perl and Ruby; else Python */
    nxt_pmt_line_t  in[NXT_PMT_IN];
    int             ptr[4];
    nxt_pmt_line_t  out[NXT_PMT_OUT];
    int             idx[4];
} nxt_pmt_case_t;


static const nxt_pmt_case_t  nxt_pmt_cases[] = {
    {
        "special lines last", 1,
        { { "Cookie", "a=1", 0 },
          { "Content-Type", "text/plain", 0 },
          { "Authorization", "Bearer x", 0 },
          { "Cookie", "b=2", 0 },
          { "Content-Type", "charset=utf-8", 0 },
          { "Authorization", "Bearer y", 0 },
          { "Cookie", "c=3", 0 } },
        { -1, 4, 6, 5 },
        { { "HTTP_COOKIE", "a=1; b=2; c=3", 0 },
          { "HTTP_CONTENT_TYPE", "text/plain, charset=utf-8", 0 },
          { "HTTP_AUTHORIZATION", "Bearer x, Bearer y", 0 } },
        { -1, 1, 0, 2 },
    },
    {
        "special lines once", 1,
        { { "Host", "localhost", 0 },
          { "Content-Length", "3", 0 },
          { "Content-Type", "text/plain", 0 },
          { "Cookie", "a=1", 0 },
          { "Authorization", "Basic dTpw", 0 } },
        { 1, 2, 3, 4 },
        { { "HTTP_HOST", "localhost", 0 },
          { "HTTP_CONTENT_LENGTH", "3", 0 },
          { "HTTP_CONTENT_TYPE", "text/plain", 0 },
          { "HTTP_COOKIE", "a=1", 0 },
          { "HTTP_AUTHORIZATION", "Basic dTpw", 0 } },
        { 1, 2, 3, 4 },
    },
    {
        "skipped lines between", 1,
        { { "X-A", "1", 0 },
          { "Transfer-Encoding", "chunked", 1 },
          { "Cookie", "a=1", 0 },
          { "X-A", "2", 0 },
          { "Traceparent",
            "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01", 1 },
          { "Cookie", "b=2", 0 },
          { "X-A", "3", 0 } },
        { -1, -1, 5, -1 },
        { { "HTTP_X_A", "1, 2, 3", 0 },
          { "HTTP_COOKIE", "a=1; b=2", 0 } },
        { -1, -1, 1, -1 },
    },
    {
        "case and similar names", 1,
        { { "cookie", "a=1", 0 },
          { "Set-Cookie", "s=1", 0 },
          { "COOKIE", "b=2", 0 },
          { "Set-Cookie", "s=2", 0 },
          { "Cookie2", "c=1", 0 },
          { "Cookie2", "c=2", 0 } },
        { -1, -1, 2, -1 },
        { { "HTTP_COOKIE", "a=1; b=2", 0 },
          { "HTTP_SET_COOKIE", "s=1, s=2", 0 },
          { "HTTP_COOKIE2", "c=1, c=2", 0 } },
        { -1, -1, 0, -1 },
    },
    {
        "inline and list lines", 1,
        { { "Cookie", "a=1", 0 },
          { "X-1", "v", 0 }, { "X-2", "v", 0 }, { "X-3", "v", 0 },
          { "X-4", "v", 0 }, { "X-5", "v", 0 }, { "X-6", "v", 0 },
          { "X-7", "v", 0 }, { "X-8", "v", 0 }, { "X-9", "v", 0 },
          { "X-10", "v", 0 }, { "X-11", "v", 0 }, { "X-12", "v", 0 },
          { "X-13", "v", 0 }, { "X-14", "v", 0 },
          { "Content-Type", "text/plain", 0 },
          { "X-3", "w", 0 },
          { "Cookie", "b=2", 0 },
          { "Content-Type", "text/html", 0 },
          { "Authorization", "Bearer z", 0 } },
        { -1, 18, 17, 19 },
        { { "HTTP_COOKIE", "a=1; b=2", 0 },
          { "HTTP_X_1", "v", 0 }, { "HTTP_X_2", "v", 0 },
          { "HTTP_X_3", "v, w", 0 }, { "HTTP_X_4", "v", 0 },
          { "HTTP_X_5", "v", 0 }, { "HTTP_X_6", "v", 0 },
          { "HTTP_X_7", "v", 0 }, { "HTTP_X_8", "v", 0 },
          { "HTTP_X_9", "v", 0 }, { "HTTP_X_10", "v", 0 },
          { "HTTP_X_11", "v", 0 }, { "HTTP_X_12", "v", 0 },
          { "HTTP_X_13", "v", 0 }, { "HTTP_X_14", "v", 0 },
          { "HTTP_CONTENT_TYPE", "text/plain, text/html", 0 },
          { "HTTP_AUTHORIZATION", "Bearer z", 0 } },
        { -1, 15, 0, 16 },
    },
    {
        "no prefix, not joined", 0,
        { { "Cookie", "a=1", 0 },
          { "Content-Type", "text/plain", 0 },
          { "Cookie", "b=2", 0 },
          { "Content-Type", "text/html", 0 } },
        { -1, 3, 2, -1 },
        { { "Cookie", "a=1", 0 },
          { "Content-Type", "text/plain", 0 },
          { "Cookie", "b=2", 0 },
          { "Content-Type", "text/html", 0 } },
        { -1, 3, 2, -1 },
    },
};


static const nxt_app_type_t  nxt_pmt_prefixed_types[] = {
    NXT_APP_PHP, NXT_APP_PERL, NXT_APP_RUBY,
};


static const char  *nxt_pmt_idx_names[] = {
    "content_length_field", "content_type_field", "cookie_field",
    "authorization_field",
};


static uint16_t
nxt_pmt_hash(const char *name)
{
    u_char    c;
    uint32_t  hash;

    hash = NXT_HTTP_FIELD_HASH_INIT;

    while (*name != '\0') {
        c = *name++;
        hash = nxt_http_field_hash_char(hash, nxt_lowcase(c));
    }

    return nxt_http_field_hash_end(hash) & 0xFFFF;
}


/*
 * "marks" sets the merged bit of every line before the call, as the memory
 * that nxt_list_add() returns can hold it.
 */

static nxt_int_t
nxt_pmt_run(nxt_task_t *task, nxt_mp_t *mp, nxt_app_t *app,
    nxt_sockaddr_t *sa, const char *name, nxt_app_type_t type,
    nxt_bool_t marks, const nxt_pmt_line_t *in, nxt_uint_t n_in,
    const int *ptr, const nxt_pmt_line_t *out, nxt_uint_t n_out,
    const int *idx)
{
    u_char              *s, *v, label[128];
    uint32_t            got[4], want;
    nxt_buf_t           *b;
    nxt_str_t           *method, *args;
    nxt_uint_t          i, status;
    nxt_http_field_t    *f, **fields, **refs[4];
    nxt_unit_field_t    *uf;
    nxt_unit_request_t  *req;
    nxt_http_request_t  *r;

    (void) nxt_sprintf(label, label + sizeof(label), "\"%s\" (type %d%s)%Z",
                       name, (int) type, marks ? ", marks set" : "");

    r = nxt_mp_zalloc(mp, sizeof(nxt_http_request_t));
    method = nxt_mp_zalloc(mp, sizeof(nxt_str_t));
    args = nxt_mp_zalloc(mp, sizeof(nxt_str_t));
    fields = nxt_mp_zalloc(mp, n_in * sizeof(nxt_http_field_t *));

    if (r == NULL || method == NULL || args == NULL || fields == NULL) {
        return NXT_ERROR;
    }

    r->mem_pool = mp;
    nxt_str_set(method, "GET");
    r->method = method;
    nxt_str_set(&r->version, "HTTP/1.1");
    nxt_str_set(&r->server_name, "localhost");
    nxt_str_set(&r->target, "/");
    r->path = &r->target;
    r->args = args;
    r->remote = sa;
    r->local = sa;
    r->content_length_n = -1;

    for (i = 0; i < n_in; i++) {
        f = nxt_http_req_field_zero_add(r);
        if (f == NULL) {
            return NXT_ERROR;
        }

        f->hash = nxt_pmt_hash(in[i].name);
        f->skip = in[i].skip;
        f->merged = marks;
        f->name = (u_char *) in[i].name;
        f->name_length = nxt_strlen(in[i].name);
        f->value = (u_char *) in[i].value;
        f->value_length = nxt_strlen(in[i].value);

        fields[i] = f;
    }

    refs[0] = &r->content_length;
    refs[1] = &r->content_type;
    refs[2] = &r->cookie;
    refs[3] = &r->authorization;

    for (i = 0; i < 4; i++) {
        *refs[i] = (ptr[i] >= 0) ? fields[ptr[i]] : NULL;
    }

    app->type = type;
    status = 0;

    b = nxt_router_test_prepare_msg(task, r, app, &status);
    if (b == NULL) {
        nxt_log_alert(task->log, "prepare msg dup test %s: refused with %d",
                      label, (int) status);
        return NXT_ERROR;
    }

    req = (nxt_unit_request_t *) b->mem.pos;

    if (req->fields_count != n_out) {
        nxt_log_alert(task->log, "prepare msg dup test %s: %uD fields, "
                      "expected %ui", label, req->fields_count, n_out);
        return NXT_ERROR;
    }

    got[0] = req->content_length_field;
    got[1] = req->content_type_field;
    got[2] = req->cookie_field;
    got[3] = req->authorization_field;

    for (i = 0; i < 4; i++) {
        want = (idx[i] < 0) ? NXT_UNIT_NONE_FIELD : (uint32_t) idx[i];

        if (got[i] != want) {
            nxt_log_alert(task->log, "prepare msg dup test %s: %s %uD, "
                          "expected %uD", label, nxt_pmt_idx_names[i],
                          got[i], want);
            return NXT_ERROR;
        }
    }

    for (i = 0; i < n_out; i++) {
        uf = &req->fields[i];
        s = nxt_unit_sptr_get(&uf->name);
        v = nxt_unit_sptr_get(&uf->value);

        if (uf->name_length != nxt_strlen(out[i].name)
            || memcmp(s, out[i].name, uf->name_length) != 0
            || s[uf->name_length] != '\0'
            || uf->value_length != nxt_strlen(out[i].value)
            || memcmp(v, out[i].value, uf->value_length) != 0
            || v[uf->value_length] != '\0')
        {
            nxt_log_alert(task->log, "prepare msg dup test %s: field %ui is "
                          "\"%*s: %*s\", expected \"%s: %s\"", label, i,
                          (size_t) uf->name_length, s,
                          (size_t) uf->value_length, v, out[i].name,
                          out[i].value);
            return NXT_ERROR;
        }
    }

    /* The request lines are as they were, with no mark left. */

    for (i = 0; i < n_in; i++) {
        if (fields[i]->skip != in[i].skip
            || (!in[i].skip && fields[i]->merged))
        {
            nxt_log_alert(task->log, "prepare msg dup test %s: line %ui "
                          "\"%s\" has skip %d and merged %d after the call, "
                          "skip %d before", label, i, in[i].name,
                          (int) fields[i]->skip, (int) fields[i]->merged,
                          (int) in[i].skip);
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


static nxt_int_t
nxt_prepare_msg_test_duplicates(nxt_task_t *task, nxt_mp_t *mp, nxt_app_t *app,
    nxt_sockaddr_t *sa)
{
    nxt_int_t             ret;
    nxt_uint_t            i, t, n, n_in, n_out;
    nxt_app_type_t        type;
    const nxt_pmt_case_t  *tc;

    ret = NXT_OK;

    for (i = 0; i < nxt_nitems(nxt_pmt_cases); i++) {
        tc = &nxt_pmt_cases[i];

        for (n_in = 0; n_in < NXT_PMT_IN && tc->in[n_in].name != NULL; n_in++)
        {
            /* void */
        }

        for (n_out = 0;
             n_out < NXT_PMT_OUT && tc->out[n_out].name != NULL;
             n_out++)
        {
            /* void */
        }

        /* Each type with and without the marks set before the call. */
        n = tc->prefixed ? nxt_nitems(nxt_pmt_prefixed_types) : 1;

        for (t = 0; t < 2 * n; t++) {
            type = tc->prefixed ? nxt_pmt_prefixed_types[t / 2]
                                : NXT_APP_PYTHON;

            if (nxt_pmt_run(task, mp, app, sa, tc->name, type, t % 2, tc->in,
                            n_in, tc->ptr, tc->out, n_out, tc->idx)
                != NXT_OK)
            {
                ret = NXT_ERROR;
            }
        }
    }

    return ret;
}


static nxt_int_t
nxt_prepare_msg_test_run(nxt_thread_t *thr)
{
    size_t                             used, live_used;
    nxt_mp_t                           *mp;
    nxt_str_t                          addr;
    nxt_int_t                          ret;
    nxt_app_t                          *app;
    nxt_uint_t                         i;
    nxt_task_t                         *task;
    nxt_sockaddr_t                     *sa;
    nxt_event_engine_t                 engine, *saved_engine;
    nxt_prepare_msg_test_case_t        live;
    const nxt_prepare_msg_test_case_t  *tc;

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
        tc = &nxt_prepare_msg_test_cases[i];

        if (nxt_prepare_msg_test_case(task, mp, app, sa, tc, &used)
            != NXT_OK)
        {
            ret = NXT_ERROR;
            continue;
        }

        if (tc->skipped_length == 0 || tc->status != 0) {
            continue;
        }

        /* The skipped field must add nothing to the message. */

        live = *tc;
        live.skipped_length = 0;

        if (nxt_prepare_msg_test_case(task, mp, app, sa, &live, &live_used)
            != NXT_OK)
        {
            ret = NXT_ERROR;
            continue;
        }

        if (used != live_used) {
            nxt_log_alert(task->log, "prepare msg test \"%s\": the message "
                          "is %uz bytes, %uz without the skipped field",
                          tc->name, used, live_used);
            ret = NXT_ERROR;
        }
    }

    if (nxt_prepare_msg_test_duplicates(task, mp, app, sa) != NXT_OK) {
        ret = NXT_ERROR;
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
