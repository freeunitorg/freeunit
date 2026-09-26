/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_router_response_header_parse() (src/nxt_router.c) resolves each
 * field's name and value sptr as base + offset, and the application chooses
 * the offset.  One pointing outside the response buffer must be refused, not
 * dereferenced (found by fuzzing/nxt_router_app_response_fuzz.c).
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_unit_response.h>
#include "nxt_tests.h"


nxt_int_t nxt_router_test_response_header_parse(nxt_task_t *task,
    nxt_http_request_t *r, nxt_buf_t *b);


static nxt_buf_t *
nxt_router_response_parse_test_buf(nxt_mp_t *mp, const char *name,
    const char *value, nxt_bool_t name_oob, nxt_bool_t value_oob)
{
    size_t                data_size, buf_size;
    u_char                *data, *far;
    nxt_buf_t             *b;
    nxt_unit_field_t      *f;
    nxt_unit_response_t   *resp;

    data_size = nxt_strlen(name) + nxt_strlen(value);
    buf_size = sizeof(nxt_unit_response_t) + sizeof(nxt_unit_field_t)
               + data_size;

    resp = nxt_mp_zalloc(mp, buf_size);
    if (resp == NULL) {
        return NULL;
    }

    resp->fields_count = 1;
    resp->status = 200;

    f = &resp->fields[0];
    f->hash = 0;
    f->name_length = nxt_strlen(name);
    f->value_length = nxt_strlen(value);

    data = (u_char *) resp + sizeof(nxt_unit_response_t)
           + sizeof(nxt_unit_field_t);
    nxt_memcpy(data, name, f->name_length);
    nxt_memcpy(data + f->name_length, value, f->value_length);

    /* Outside "resp" but mapped: the check's verdict is tested, no crash. */
    far = nxt_mp_zalloc(mp, 4096);
    if (far == NULL) {
        return NULL;
    }

    nxt_unit_sptr_set(&f->name, name_oob ? far : data);
    nxt_unit_sptr_set(&f->value,
                       value_oob ? far : data + f->name_length);

    b = nxt_mp_zalloc(mp, sizeof(nxt_buf_t));
    if (b == NULL) {
        return NULL;
    }

    b->mem.start = (u_char *) resp;
    b->mem.pos = (u_char *) resp;
    b->mem.free = (u_char *) resp + buf_size;
    b->mem.end = (u_char *) resp + buf_size;

    return b;
}


nxt_int_t
nxt_router_response_parse_test(nxt_thread_t *thr)
{
    nxt_mp_t            *mp;
    nxt_int_t           ret;
    nxt_uint_t          i;
    nxt_buf_t           *b;
    nxt_task_t          *task;
    nxt_http_request_t  *r;

    static const struct {
        const char  *name;
        nxt_bool_t  name_oob, value_oob;
        nxt_int_t   expect;
    } tests[] = {
        { "in-bounds field", 0, 0, NXT_OK },
        { "out-of-bounds name sptr", 1, 0, NXT_ERROR },
        { "out-of-bounds value sptr", 0, 1, NXT_ERROR },
    };

    task = thr->task;
    task->thread = thr;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    ret = NXT_OK;

    for (i = 0; ret == NXT_OK && i < nxt_nitems(tests); i++) {
        r = nxt_mp_zalloc(mp, sizeof(nxt_http_request_t));
        b = nxt_router_response_parse_test_buf(mp, "X-Test", "value",
                                                tests[i].name_oob,
                                                tests[i].value_oob);
        if (r == NULL || b == NULL) {
            ret = NXT_ERROR;
            break;
        }

        r->mem_pool = mp;
        r->task = *task;

        if (nxt_router_test_response_header_parse(task, r, b)
            != tests[i].expect)
        {
            nxt_log_alert(task->log, "response parse test \"%s\" failed",
                          tests[i].name);
            ret = NXT_ERROR;
        }
    }

    nxt_mp_destroy(mp);

    if (ret == NXT_OK) {
        nxt_log(task, NXT_LOG_NOTICE, "router response parse test passed");
    }

    return ret;
}
