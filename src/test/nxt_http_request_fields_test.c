
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_h1proto.h>
#include "nxt_tests.h"


/*
 * nxt_h1p_init() builds two hashes over one table: nxt_h1p_fields_hash
 * with every entry, for the HTTP/1 parser, and nxt_http_request_fields_hash
 * with the entries after the HTTP/1 framing ones, for a frontend that has
 * no such framing.  The first part of the test walks the table by name:
 * a framing field resolves in the h1 hash only, and every other field
 * resolves in both hashes to the same entry.  The second part processes
 * fields through the neutral hash the way another frontend would, with
 * lower-case names hashed by the nxt_http_field_hash_* macros, and checks
 * that the handlers act on the request: Host is validated and stored, and
 * Content-Length is parsed and bounded by max_body_size.
 */

typedef struct {
    nxt_str_t   name;
    nxt_bool_t  h1_only;
} nxt_http_request_fields_test_case_t;


static const nxt_http_request_fields_test_case_t
    nxt_http_request_fields_test_cases[] =
{
    { nxt_string("Connection"),            1 },
    { nxt_string("Upgrade"),               1 },
    { nxt_string("Sec-WebSocket-Key"),     1 },
    { nxt_string("Sec-WebSocket-Version"), 1 },
    { nxt_string("Transfer-Encoding"),     1 },

    { nxt_string("Host"),                  0 },
    { nxt_string("Cookie"),                0 },
    { nxt_string("Referer"),               0 },
    { nxt_string("User-Agent"),            0 },
    { nxt_string("Content-Type"),          0 },
    { nxt_string("Content-Length"),        0 },
    { nxt_string("Authorization"),         0 },
#if (NXT_HAVE_OTEL)
    { nxt_string("Traceparent"),           0 },
    { nxt_string("Tracestate"),            0 },
#endif
};


static uint16_t
nxt_http_request_fields_test_hash(const nxt_str_t *name)
{
    size_t    i;
    uint32_t  hash;

    hash = NXT_HTTP_FIELD_HASH_INIT;

    for (i = 0; i < name->length; i++) {
        hash = nxt_http_field_hash_char(hash, nxt_lowcase(name->start[i]));
    }

    return nxt_http_field_hash_end(hash) & 0xFFFF;
}


static void *
nxt_http_request_fields_test_find(nxt_lvlhsh_t *hash, const nxt_str_t *name)
{
    nxt_lvlhsh_query_t  lhq;

    lhq.proto = &nxt_http_fields_hash_proto;
    lhq.key_hash = nxt_http_request_fields_test_hash(name);
    lhq.key = *name;

    if (nxt_lvlhsh_find(hash, &lhq) != NXT_OK) {
        return NULL;
    }

    return lhq.value;
}


static nxt_int_t
nxt_http_request_fields_test_process(nxt_http_request_t *r, const char *name,
    const char *value)
{
    nxt_str_t         str;
    nxt_http_field_t  field;

    nxt_memzero(&field, sizeof(nxt_http_field_t));

    str.length = nxt_strlen(name);
    str.start = (u_char *) name;

    field.hash = nxt_http_request_fields_test_hash(&str);
    field.name_length = str.length;
    field.name = str.start;
    field.value_length = nxt_strlen(value);
    field.value = (u_char *) value;

    return nxt_http_field_process(&field, &nxt_http_request_fields_hash, r);
}


nxt_int_t
nxt_http_request_fields_test(nxt_thread_t *thr)
{
    void                     *h1, *neutral;
    nxt_mp_t                 *mp;
    nxt_int_t                ret;
    nxt_uint_t               i;
    nxt_socket_conf_t        skcf;
    nxt_http_request_t       r;
    nxt_socket_conf_joint_t  joint;

    const nxt_http_request_fields_test_case_t  *tc;

    static const nxt_str_t  host = nxt_string("example.com");

    if (nxt_lvlhsh_is_empty(&nxt_h1p_fields_hash)) {
        if (nxt_h1p_init(thr->task) != NXT_OK) {
            nxt_log_alert(thr->log, "http request fields test failed: "
                          "nxt_h1p_init()");
            return NXT_ERROR;
        }
    }

    for (i = 0; i < nxt_nitems(nxt_http_request_fields_test_cases); i++) {
        tc = &nxt_http_request_fields_test_cases[i];

        h1 = nxt_http_request_fields_test_find(&nxt_h1p_fields_hash,
                                               &tc->name);
        neutral = nxt_http_request_fields_test_find(
                                               &nxt_http_request_fields_hash,
                                               &tc->name);

        if (h1 == NULL) {
            nxt_log_alert(thr->log, "http request fields test failed: "
                          "\"%V\" is not in the h1 hash", &tc->name);
            return NXT_ERROR;
        }

        if (tc->h1_only) {
            if (neutral != NULL) {
                nxt_log_alert(thr->log, "http request fields test failed: "
                              "\"%V\" is in the neutral hash", &tc->name);
                return NXT_ERROR;
            }

            continue;
        }

        if (neutral != h1) {
            nxt_log_alert(thr->log, "http request fields test failed: "
                          "\"%V\" resolves to %p in the neutral hash "
                          "and to %p in the h1 hash",
                          &tc->name, neutral, h1);
            return NXT_ERROR;
        }
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    nxt_memzero(&skcf, sizeof(nxt_socket_conf_t));
    nxt_memzero(&joint, sizeof(nxt_socket_conf_joint_t));
    nxt_memzero(&r, sizeof(nxt_http_request_t));

    skcf.max_body_size = 100;
    joint.socket_conf = &skcf;
    r.conf = &joint;
    r.mem_pool = mp;
    r.content_length_n = -1;

    ret = nxt_http_request_fields_test_process(&r, "host", "Example.COM:443");

    if (ret != NXT_OK || !nxt_strstr_eq(&r.host, &host)) {
        nxt_log_alert(thr->log, "http request fields test failed: "
                      "host: %i \"%V\"", ret, &r.host);
        goto fail;
    }

    ret = nxt_http_request_fields_test_process(&r, "content-length", "10");

    if (ret != NXT_OK || r.content_length_n != 10) {
        nxt_log_alert(thr->log, "http request fields test failed: "
                      "content-length: %i %O", ret, r.content_length_n);
        goto fail;
    }

    r.content_length = NULL;

    ret = nxt_http_request_fields_test_process(&r, "content-length", "101");

    if (ret != NXT_HTTP_PAYLOAD_TOO_LARGE) {
        nxt_log_alert(thr->log, "http request fields test failed: "
                      "content-length above max_body_size: %i", ret);
        goto fail;
    }

    /* A field the neutral hash does not know is passed through. */

    ret = nxt_http_request_fields_test_process(&r, "connection", "close");

    if (ret != NXT_OK) {
        nxt_log_alert(thr->log, "http request fields test failed: "
                      "connection: %i", ret);
        goto fail;
    }

    nxt_mp_destroy(mp);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "http request fields test passed");

    return NXT_OK;

fail:

    nxt_mp_destroy(mp);

    return NXT_ERROR;
}
