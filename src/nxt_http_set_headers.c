
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


/* The keys of r->action resolved for this request, one entry per key. */

struct nxt_http_set_headers_ctx_s {
    nxt_http_action_t       *action;
    nxt_str_t               *value;
    uint8_t                 *state;
};


typedef enum {
    NXT_HTTP_SET_HEADERS_UNRESOLVED = 0,
    NXT_HTTP_SET_HEADERS_RESOLVED,
    NXT_HTTP_SET_HEADERS_REJECTED,
} nxt_http_set_headers_state_t;


static nxt_http_set_headers_ctx_t *nxt_http_set_headers_ctx(
    nxt_http_request_t *r);
static nxt_int_t nxt_http_set_headers_value(nxt_http_request_t *r,
    nxt_http_set_headers_ctx_t *ctx, nxt_uint_t i);


/*
 * Whether the matched action replaces or removes one of the validators the
 * static handler generates.
 *
 * A conditional request has to be judged against the validator the client was
 * actually given.  When "response_headers" sets ETag or Last-Modified, the
 * value Unit derives from the file is not what went out, so comparing against
 * it answers the wrong question -- it refuses an If-Match carrying the tag the
 * server itself advertised.  The static handler asks this and declines to
 * evaluate preconditions at all in that case, which loses the 304 but is never
 * wrong.
 *
 * Only the name matters here, so no template value is resolved.
 */

nxt_bool_t
nxt_http_set_headers_override_validators(nxt_http_request_t *r)
{
    nxt_uint_t             i, n;
    nxt_http_action_t      *action;
    nxt_http_header_val_t  *header;

    action = r->action;

    if (action == NULL || action->set_headers == NULL) {
        return 0;
    }

    header = action->set_headers->elts;
    n = action->set_headers->nelts;

    for (i = 0; i < n; i++) {
        if (header[i].name.length == nxt_length("ETag")
            && nxt_strncasecmp(header[i].name.start, (u_char *) "ETag",
                               nxt_length("ETag")) == 0)
        {
            return 1;
        }

        if (header[i].name.length == nxt_length("Last-Modified")
            && nxt_strncasecmp(header[i].name.start,
                               (u_char *) "Last-Modified",
                               nxt_length("Last-Modified")) == 0)
        {
            return 1;
        }
    }

    return 0;
}


/*
 * What nxt_http_set_headers() will later do to the response field "name".
 *
 * Code that runs before the header is sent can ask this.  Compression needs
 * it: a Content-Encoding from "response_headers" replaces the one that a
 * compressor adds.  So the compressor must not run.  Otherwise the body is
 * coded twice and the field names one coding only.
 *
 * The value is resolved here with nxt_http_set_headers_value().  It stores
 * the result on the request, and nxt_http_set_headers() uses that result.
 * So each key is resolved once, and both see the same value, also for a
 * JavaScript template.  A template value can resolve to bytes that are not
 * safe in a field.  nxt_http_set_headers() then skips that key, so it
 * counts as absent here too.  A null value removes the field.  An empty
 * string is a value: the field is sent empty.  A variable that is not set
 * gives an empty string.  A failed query gives NXT_HTTP_SET_HEADER_ERROR.
 *
 * Resolving a template this early is safe.  The query is synchronous.
 * nxt_tstr_query_init() reuses r->tstr_query.  A cacheable variable keeps
 * its value in r->tstr_cache for the rest of the request.
 *
 * The status test is the same as in nxt_http_set_headers().  Keys are
 * applied in order, so the last key that matches and is not skipped gives
 * the result.  The search goes back from the end and stops at that key.
 */

nxt_http_set_header_op_t
nxt_http_set_headers_field_op(nxt_http_request_t *r, const char *name,
    size_t length)
{
    nxt_int_t                   ret;
    nxt_uint_t                  i;
    nxt_http_action_t           *action;
    nxt_http_header_val_t       *header;
    nxt_http_set_headers_ctx_t  *ctx;

    action = r->action;

    if (action == NULL || action->set_headers == NULL) {
        return NXT_HTTP_SET_HEADER_NONE;
    }

    if (r->status < NXT_HTTP_OK || r->status >= NXT_HTTP_BAD_REQUEST) {
        return NXT_HTTP_SET_HEADER_NONE;
    }

    /* The context is made only when a key matches. */

    ctx = NULL;

    header = action->set_headers->elts;
    i = action->set_headers->nelts;

    while (i > 0) {
        i--;

        if (header[i].name.length != length
            || nxt_memcasecmp(header[i].name.start, name, length) != 0)
        {
            continue;
        }

        if (ctx == NULL) {
            ctx = nxt_http_set_headers_ctx(r);
            if (nxt_slow_path(ctx == NULL)) {
                return NXT_HTTP_SET_HEADER_ERROR;
            }
        }

        ret = nxt_http_set_headers_value(r, ctx, i);

        if (ret == NXT_DECLINED) {
            continue;
        }

        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_HTTP_SET_HEADER_ERROR;
        }

        return (ctx->value[i].start != NULL) ? NXT_HTTP_SET_HEADER_REPLACE
                                             : NXT_HTTP_SET_HEADER_REMOVE;
    }

    return NXT_HTTP_SET_HEADER_NONE;
}


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


/*
 * The resolved keys of r->action for this request.  The context is made on
 * first use and kept in r->set_headers.  A request is allocated zeroed for
 * each request, also on a keep-alive connection, so the pointer never comes
 * from an earlier request.  The context is made again if r->action is not
 * the action that it was made for.
 */

static nxt_http_set_headers_ctx_t *
nxt_http_set_headers_ctx(nxt_http_request_t *r)
{
    nxt_uint_t                  n;
    nxt_http_set_headers_ctx_t  *ctx;

    ctx = r->set_headers;

    /*
     * Today the action does not change between the compression check and
     * the header: the static fallback runs only before the check.  This
     * test is a guard for a later path that changes r->action.
     */

    if (ctx != NULL && ctx->action == r->action) {
        return ctx;
    }

    n = r->action->set_headers->nelts;

    ctx = nxt_mp_zget(r->mem_pool, sizeof(nxt_http_set_headers_ctx_t));
    if (nxt_slow_path(ctx == NULL)) {
        return NULL;
    }

    ctx->value = nxt_mp_zalloc(r->mem_pool, sizeof(nxt_str_t) * n);
    if (nxt_slow_path(ctx->value == NULL)) {
        return NULL;
    }

    ctx->state = nxt_mp_zalloc(r->mem_pool, n);
    if (nxt_slow_path(ctx->state == NULL)) {
        return NULL;
    }

    ctx->action = r->action;
    r->set_headers = ctx;

    return ctx;
}


/*
 * Resolve key "i" of "response_headers" once for this request, and store
 * the result in ctx.  A later call returns the stored result.  So a
 * template, a JavaScript one too, is run only once.
 *
 * A null value gives a null string, which removes the field.  A value that
 * is not safe in a field gives NXT_DECLINED, and the key is skipped.
 * nxt_http_set_headers() and nxt_http_set_headers_field_op() both use this,
 * so they agree on what each key does.
 */

static nxt_int_t
nxt_http_set_headers_value(nxt_http_request_t *r,
    nxt_http_set_headers_ctx_t *ctx, nxt_uint_t i)
{
    nxt_int_t              ret;
    nxt_str_t              *value;
    nxt_router_conf_t      *rtcf;
    nxt_http_header_val_t  *hv;

    switch (ctx->state[i]) {

    case NXT_HTTP_SET_HEADERS_RESOLVED:
        return NXT_OK;

    case NXT_HTTP_SET_HEADERS_REJECTED:
        return NXT_DECLINED;

    default:
        break;
    }

    hv = ctx->action->set_headers->elts;
    hv = &hv[i];
    value = &ctx->value[i];

    if (hv->value == NULL) {
        nxt_str_null(value);

    } else if (nxt_tstr_is_const(hv->value)) {
        nxt_tstr_str(hv->value, value);

    } else {
        rtcf = r->conf->socket_conf->router_conf;

        ret = nxt_tstr_query_init(&r->tstr_query, rtcf->tstr_state,
                                  &r->tstr_cache, r, r->mem_pool);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }

        ret = nxt_tstr_query(&r->task, r->tstr_query, hv->value, value);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }
    }

    if (value->start != NULL
        && nxt_slow_path(!nxt_http_header_value_is_safe(value)))
    {
        ctx->state[i] = NXT_HTTP_SET_HEADERS_REJECTED;
        return NXT_DECLINED;
    }

    ctx->state[i] = NXT_HTTP_SET_HEADERS_RESOLVED;

    return NXT_OK;
}


static nxt_http_field_t *
nxt_http_resp_header_find(nxt_http_request_t *r, u_char *name, size_t length)
{
    nxt_http_field_t  *f;

    nxt_http_fields_each(f, r->resp.inline_fields, r->resp.num_inline_fields,
                         r->resp.fields)
    {

        if (f->skip) {
            continue;
        }

        if (length == f->name_length
            && nxt_memcasecmp(name, f->name, f->name_length) == 0)
        {
            return f;
        }

    } nxt_http_fields_loop;

    return NULL;
}


nxt_int_t
nxt_http_set_headers(nxt_http_request_t *r)
{
    nxt_int_t                   ret;
    nxt_uint_t                  i, n;
    nxt_str_t                   *value;
    nxt_http_field_t            *f;
    nxt_http_action_t           *action;
    nxt_http_header_val_t       *hv, *header;
    nxt_http_set_headers_ctx_t  *ctx;

    action = r->action;

    if (action == NULL || action->set_headers == NULL) {
        return NXT_OK;
    }

    if ((r->status < NXT_HTTP_OK || r->status >= NXT_HTTP_BAD_REQUEST)) {
        return NXT_OK;
    }

    header = action->set_headers->elts;
    n = action->set_headers->nelts;

    /*
     * nxt_http_set_headers_field_op() can have resolved some keys already.
     * Their stored results are used here, and they are not resolved again.
     */

    ctx = nxt_http_set_headers_ctx(r);
    if (nxt_slow_path(ctx == NULL)) {
        return NXT_ERROR;
    }

    value = ctx->value;

    for (i = 0; i < n; i++) {
        hv = &header[i];

        ret = nxt_http_set_headers_value(r, ctx, i);

        if (nxt_slow_path(ret == NXT_DECLINED)) {
            nxt_log(&r->task, NXT_LOG_INFO,
                    "set_headers \"%V\": dropping value containing control "
                    "bytes (HTTP response-splitting protection)",
                    &hv->name);

            /*
             * The entry is marked as rejected instead of clearing the value:
             * a NULL value means "delete this header", and letting an
             * attacker-triggered rejection remove an existing response
             * header (e.g. an app-emitted X-Frame-Options) would fail
             * open.  Rejected entries are skipped entirely below, so a
             * pre-existing same-named header survives untouched.
             */
            continue;
        }

        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }
    }

    for (i = 0; i < n; i++) {
        if (ctx->state[i] == NXT_HTTP_SET_HEADERS_REJECTED) {
            continue;
        }

        hv = &header[i];

        f = nxt_http_resp_header_find(r, hv->name.start, hv->name.length);

        if (value[i].start != NULL) {

            if (f == NULL) {
                f = nxt_http_resp_field_zero_add(&r->resp, r->mem_pool);
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
