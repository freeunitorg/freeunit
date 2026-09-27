/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_router_response_header_parse() (src/nxt_router.c) decodes a response
 * buffer the application wrote.  Every count, length and sptr in it must be
 * checked against the buffer, not followed.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_unit_response.h>
#include "nxt_tests.h"


#define NXT_RRP_NAME   "X-Test"
#define NXT_RRP_VALUE  "value"
#define NXT_RRP_BODY   "body"
#define NXT_RRP_SIZE                                                          \
    (sizeof(nxt_unit_response_t) + sizeof(nxt_unit_field_t)                   \
     + nxt_length(NXT_RRP_NAME) + nxt_length(NXT_RRP_VALUE)                   \
     + nxt_length(NXT_RRP_BODY))


/* A well-formed response with one field and a piggybacked body. */
static void
nxt_router_response_parse_test_fill(nxt_unit_response_t *resp)
{
    u_char            *p;
    nxt_unit_field_t  *f;

    nxt_memzero(resp, NXT_RRP_SIZE);

    resp->status = 200;
    resp->fields_count = 1;
    resp->piggyback_content_length = nxt_length(NXT_RRP_BODY);

    f = &resp->fields[0];
    f->name_length = nxt_length(NXT_RRP_NAME);
    f->value_length = nxt_length(NXT_RRP_VALUE);

    p = (u_char *) &resp->fields[1];

    nxt_unit_sptr_set(&f->name, p);
    p = nxt_cpymem(p, NXT_RRP_NAME, nxt_length(NXT_RRP_NAME));

    nxt_unit_sptr_set(&f->value, p);
    p = nxt_cpymem(p, NXT_RRP_VALUE, nxt_length(NXT_RRP_VALUE));

    nxt_unit_sptr_set(&resp->piggyback_content, p);
    nxt_memcpy(p, NXT_RRP_BODY, nxt_length(NXT_RRP_BODY));
}


static void
nxt_rrp_edit_name(nxt_unit_response_t *resp)
{
    resp->fields[0].name.offset = 4096;
}


static void
nxt_rrp_edit_value(nxt_unit_response_t *resp)
{
    resp->fields[0].value.offset = 4096;
}


static void
nxt_rrp_edit_value_length(nxt_unit_response_t *resp)
{
    resp->fields[0].value_length = 4096;
}


static void
nxt_rrp_edit_count(nxt_unit_response_t *resp)
{
    resp->fields_count = 2;
}


static void
nxt_rrp_edit_piggyback(nxt_unit_response_t *resp)
{
    resp->piggyback_content.offset = 4096;
}


static void
nxt_rrp_edit_piggyback_length(nxt_unit_response_t *resp)
{
    resp->piggyback_content_length = 4096;
}


nxt_int_t
nxt_router_response_parse_test(nxt_thread_t *thr)
{
    nxt_mp_t            *mp;
    nxt_int_t           ret;
    nxt_uint_t          i;
    nxt_buf_t           b;
    nxt_task_t          *task;
    nxt_http_request_t  *r;

    /* "resp" is in a page of its own, so a wrong offset stays mapped. */
    static union {
        nxt_unit_response_t  resp;
        u_char               page[8192];
    } u;

    static const struct {
        const char  *name;
        void        (*edit)(nxt_unit_response_t *resp);
        size_t      size;
        nxt_int_t   expect;
    } tests[] = {
        { "in-bounds response", NULL, NXT_RRP_SIZE, NXT_OK },
        { "short buffer", NULL, sizeof(nxt_unit_response_t) - 1, NXT_ERROR },
        { "fields_count past the buffer", nxt_rrp_edit_count, NXT_RRP_SIZE,
          NXT_ERROR },
        { "name sptr out of the buffer", nxt_rrp_edit_name, NXT_RRP_SIZE,
          NXT_ERROR },
        { "value sptr out of the buffer", nxt_rrp_edit_value, NXT_RRP_SIZE,
          NXT_ERROR },
        { "value length past the buffer", nxt_rrp_edit_value_length,
          NXT_RRP_SIZE, NXT_ERROR },
        { "piggyback sptr out of the buffer", nxt_rrp_edit_piggyback,
          NXT_RRP_SIZE, NXT_ERROR },
        { "piggyback length past the buffer", nxt_rrp_edit_piggyback_length,
          NXT_RRP_SIZE, NXT_ERROR },
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
        if (r == NULL) {
            ret = NXT_ERROR;
            break;
        }

        r->mem_pool = mp;
        r->task = *task;

        nxt_router_response_parse_test_fill(&u.resp);

        if (tests[i].edit != NULL) {
            tests[i].edit(&u.resp);
        }

        nxt_memzero(&b, sizeof(nxt_buf_t));
        b.mem.start = u.page;
        b.mem.pos = u.page;
        b.mem.free = u.page + tests[i].size;
        b.mem.end = b.mem.free;

        if (nxt_router_test_response_header_parse(task, r, &b)
            != tests[i].expect)
        {
            nxt_log_alert(task->log, "response parse test \"%s\" failed",
                          tests[i].name);
            ret = NXT_ERROR;
            break;
        }

        if (tests[i].expect == NXT_OK
            && (r->status != 200
                || nxt_buf_mem_used_size(&b.mem) != nxt_length(NXT_RRP_BODY)
                || memcmp(b.mem.pos, NXT_RRP_BODY, nxt_length(NXT_RRP_BODY))
                   != 0))
        {
            nxt_log_alert(task->log, "response parse test \"%s\": "
                          "wrong status or body", tests[i].name);
            ret = NXT_ERROR;
        }
    }

    nxt_mp_destroy(mp);

    if (ret == NXT_OK) {
        nxt_log(task, NXT_LOG_NOTICE, "router response parse test passed");
    }

    return ret;
}
