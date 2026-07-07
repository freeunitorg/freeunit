
/*
 * Copyright (C) Zhidao HONG
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_router.h>
#include <nxt_http.h>


typedef struct {
    nxt_str_t               name;
    nxt_tstr_t              *value;
} nxt_http_header_val_t;


nxt_int_t
nxt_http_set_headers_init(nxt_router_conf_t *rtcf, nxt_http_action_t *action,
     nxt_http_action_conf_t *acf)
 {
    uint32_t               next;
    nxt_str_t              str, name;
    nxt_array_t            *headers;
    nxt_conf_value_t       *value;
    nxt_http_header_val_t  *hv;

    headers = nxt_array_create(rtcf->mem_pool, 4,
                               sizeof(nxt_http_header_val_t));
    if (nxt_slow_path(headers == NULL)) {
        return NXT_ERROR;
    }

    action->set_headers = headers;

    next = 0;

    for ( ;; ) {
        value = nxt_conf_next_object_member(acf->set_headers, &name, &next);
        if (value == NULL) {
            break;
        }

        hv = nxt_array_zero_add(headers);
        if (nxt_slow_path(hv == NULL)) {
            return NXT_ERROR;
        }

        hv->name.length = name.length;

        hv->name.start = nxt_mp_nget(rtcf->mem_pool, name.length);
        if (nxt_slow_path(hv->name.start == NULL)) {
            return NXT_ERROR;
        }

        nxt_memcpy(hv->name.start, name.start, name.length);

        if (nxt_conf_type(value) == NXT_CONF_STRING) {
            nxt_conf_get_string(value, &str);

            hv->value = nxt_tstr_compile(rtcf->tstr_state, &str, 0);
            if (nxt_slow_path(hv->value == NULL)) {
                return NXT_ERROR;
            }
        }
    }

    return NXT_OK;
}


/*
 * Reject values that would inject a header boundary into the response.
 * Templated values (e.g. $uri, $arg_*) can carry CR/LF/NUL bytes if the
 * client encodes them in the request, and writing those bytes verbatim
 * into the wire serialiser yields HTTP response splitting.  Static
 * config values are operator-controlled and trusted, but the check is
 * cheap enough to apply to both paths.
 *
 * Per the RFC 9110 field-value grammar, all control bytes other than
 * HTAB are rejected, including DEL (0x7F); lenient downstream proxies
 * may otherwise reinterpret them.  HTAB and high (0x80+) bytes are
 * left alone.
 */
static nxt_bool_t
nxt_http_header_value_is_safe(const nxt_str_t *v)
{
    u_char  c;
    size_t  i;

    for (i = 0; i < v->length; i++) {
        c = v->start[i];

        if ((c < 0x20 && c != '\t') || c == 0x7F) {
            return 0;
        }
    }

    return 1;
}


static nxt_http_field_t *
nxt_http_resp_header_find(nxt_http_request_t *r, u_char *name, size_t length)
{
    nxt_http_field_t  *f;

    nxt_list_each(f, r->resp.fields) {

        if (f->skip) {
            continue;
        }

        if (length == f->name_length
            && nxt_memcasecmp(name, f->name, f->name_length) == 0)
        {
            return f;
        }

    } nxt_list_loop;

    return NULL;
}


nxt_int_t
nxt_http_set_headers(nxt_http_request_t *r)
{
    u_char                 *rejected;
    nxt_int_t              ret;
    nxt_uint_t             i, n;
    nxt_str_t              *value;
    nxt_http_field_t       *f;
    nxt_router_conf_t      *rtcf;
    nxt_http_action_t      *action;
    nxt_http_header_val_t  *hv, *header;

    action = r->action;

    if (action == NULL || action->set_headers == NULL) {
        return NXT_OK;
    }

    if ((r->status < NXT_HTTP_OK || r->status >= NXT_HTTP_BAD_REQUEST)) {
        return NXT_OK;
    }

    rtcf = r->conf->socket_conf->router_conf;

    header = action->set_headers->elts;
    n = action->set_headers->nelts;

    value = nxt_mp_zalloc(r->mem_pool, sizeof(nxt_str_t) * n);
    if (nxt_slow_path(value == NULL)) {
        return NXT_ERROR;
    }

    rejected = nxt_mp_zalloc(r->mem_pool, n);
    if (nxt_slow_path(rejected == NULL)) {
        return NXT_ERROR;
    }

    for (i = 0; i < n; i++) {
        hv = &header[i];

        if (hv->value == NULL) {
            continue;
        }

        if (nxt_tstr_is_const(hv->value)) {
            nxt_tstr_str(hv->value, &value[i]);

        } else {
            ret = nxt_tstr_query_init(&r->tstr_query, rtcf->tstr_state,
                                      &r->tstr_cache, r, r->mem_pool);
            if (nxt_slow_path(ret != NXT_OK)) {
                return NXT_ERROR;
            }

            ret = nxt_tstr_query(&r->task, r->tstr_query, hv->value, &value[i]);
            if (nxt_slow_path(ret != NXT_OK)) {
                return NXT_ERROR;
            }
        }

        if (value[i].start != NULL
            && nxt_slow_path(!nxt_http_header_value_is_safe(&value[i])))
        {
            nxt_log(&r->task, NXT_LOG_INFO,
                    "set_headers \"%V\": dropping value containing control "
                    "bytes (HTTP response-splitting protection)",
                    &hv->name);

            /*
             * Mark the entry as rejected instead of clearing the value:
             * a NULL value means "delete this header", and letting an
             * attacker-triggered rejection remove an existing response
             * header (e.g. an app-emitted X-Frame-Options) would fail
             * open.  Rejected entries are skipped entirely below, so a
             * pre-existing same-named header survives untouched.
             */
            rejected[i] = 1;
        }
    }

    for (i = 0; i < n; i++) {
        if (rejected[i]) {
            continue;
        }

        hv = &header[i];

        f = nxt_http_resp_header_find(r, hv->name.start, hv->name.length);

        if (value[i].start != NULL) {

            if (f == NULL) {
                f = nxt_list_zero_add(r->resp.fields);
                if (nxt_slow_path(f == NULL)) {
                    return NXT_ERROR;
                }

                f->name = hv->name.start;
                f->name_length = hv->name.length;
            }

            f->value = value[i].start;
            f->value_length = value[i].length;

        } else if (f != NULL) {
            f->skip = 1;
        }
    }

    return NXT_OK;
}
