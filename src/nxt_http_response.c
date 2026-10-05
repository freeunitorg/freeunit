
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_router.h>
#include <nxt_http.h>


static nxt_int_t nxt_http_response_status(void *ctx, nxt_http_field_t *field,
    uintptr_t data);
static nxt_int_t nxt_http_response_skip(void *ctx, nxt_http_field_t *field,
    uintptr_t data);
static nxt_int_t nxt_http_response_field(void *ctx, nxt_http_field_t *field,
    uintptr_t offset);
static nxt_int_t nxt_http_response_content_length(void *ctx,
    nxt_http_field_t *field, uintptr_t offset);


nxt_lvlhsh_t  nxt_response_fields_hash;

static nxt_http_field_proc_t   nxt_response_fields[] = {
    { nxt_string("Status"),         &nxt_http_response_status, 0 },
    { nxt_string("Server"),         &nxt_http_response_skip, 0 },
    { nxt_string("Date"),           &nxt_http_response_field,
        offsetof(nxt_http_request_t, resp.date) },
    { nxt_string("Connection"),     &nxt_http_response_skip, 0 },
    { nxt_string("Content-Type"),   &nxt_http_response_field,
        offsetof(nxt_http_request_t, resp.content_type) },
    { nxt_string("Content-Length"), &nxt_http_response_content_length,
        offsetof(nxt_http_request_t, resp.content_length) },
    { nxt_string("Upgrade"),        &nxt_http_response_skip, 0 },
    { nxt_string("Sec-WebSocket-Accept"), &nxt_http_response_skip, 0 },
};


nxt_int_t
nxt_http_response_hash_init(nxt_task_t *task)
{
    return nxt_http_fields_hash(&nxt_response_fields_hash,
                    nxt_response_fields, nxt_nitems(nxt_response_fields));
}


nxt_int_t
nxt_http_response_status(void *ctx, nxt_http_field_t *field,
    uintptr_t data)
{
    nxt_int_t           status;
    nxt_http_request_t  *r;

    r = ctx;

    field->skip = 1;

    if (field->value_length >= 3) {
        status = nxt_int_parse(field->value, 3);

        if (status >= 100 && status <= 999) {
            r->status = status;
            return NXT_OK;
        }
    }

    return NXT_ERROR;
}


nxt_int_t
nxt_http_response_skip(void *ctx, nxt_http_field_t *field, uintptr_t data)
{
    field->skip = 1;

    return NXT_OK;
}


nxt_int_t
nxt_http_response_field(void *ctx, nxt_http_field_t *field, uintptr_t offset)
{
    nxt_http_request_t  *r;

    r = ctx;

    nxt_value_at(nxt_http_field_t *, r, offset) = field;

    return NXT_OK;
}


/*
 * An application response that disagrees with itself about its own body length
 * is a response-smuggling primitive.  nxt_h1p_request_header_send() emits every
 * unskipped response field verbatim and, because r->resp.content_length is set
 * and unskipped, frames the body by the advertised length instead of chunking
 * it.  So two disagreeing Content-Length headers both reach the client, and a
 * downstream proxy or cache that honours the other one desyncs from the
 * connection -- response splitting / cache poisoning against whoever shares it
 * next.  RFC 7230 section 3.3.3 requires such a message to be treated as
 * invalid.  "Content-Length: 10, 200" is the list form of the same
 * disagreement and reaches a downstream parser identically, so the value is
 * validated here too; this is the only place on the application path that
 * parses it (the proxy sibling needs the number itself, this does not).
 *
 * Trust no disputed value: skip every Content-Length involved so none is
 * forwarded, and reset content_length_n.  The framing check in
 * nxt_h1p_request_header_send() then falls through to chunked encoding and the
 * body is framed by the byte count the application actually wrote.
 *
 * Two details are load-bearing:
 *
 *   - *slot is cleared, not left pointing at the rejected field.  A field
 *     skipped inside the response-field loop is popped from the store by
 *     nxt_router_response_ready_handler(), and nxt_http_resp_field_add() hands
 *     the same inline_fields[] slot to the next field, which overwrites it.  A
 *     retained pointer would end up referring to an unrelated header with
 *     skip == 0, and the response would go out with neither Content-Length nor
 *     chunked framing -- unframed, and worse than the bug this closes.
 *
 *   - r->inconsistent is set, which drops keepalive at request close (the body
 *     still gets its terminal chunk; see nxt_h1proto.c).  It doubles as the
 *     "a Content-Length was already rejected" marker that *slot can no longer
 *     carry, so a later one cannot be mistaken for the first and reinstated.
 *     Closing the connection also means that even if framing were wrong, there
 *     is no following response on it to desync into.
 */

nxt_int_t
nxt_http_response_content_length(void *ctx, nxt_http_field_t *field,
    uintptr_t offset)
{
    u_char              *p;
    size_t              length;
    nxt_http_field_t    **slot;
    nxt_http_request_t  *r;

    r = ctx;

    /* nxt_value_at() has no outer parentheses, so bind it once here. */
    slot = &nxt_value_at(nxt_http_field_t *, r, offset);

    if (nxt_slow_path(*slot != NULL)) {
        nxt_log(&r->task, NXT_LOG_WARN,
                "application sent duplicate Content-Length");

        (*slot)->skip = 1;
        *slot = NULL;

        goto reject;
    }

    if (nxt_slow_path(r->inconsistent)) {
        /* An earlier Content-Length was already rejected; do not reinstate. */
        goto reject;
    }

    /*
     * Surrounding optional whitespace is part of the field's serialisation,
     * not of its value (RFC 7230 section 3.2.4), and an application can emit
     * it -- PHP's header() preserves a trailing space.  Trim it before parsing
     * so a legitimate "10 " is not rejected; the field is still forwarded
     * verbatim when it parses, since a recipient strips the OWS itself.
     */
    p = field->value;
    length = field->value_length;

    while (length > 0 && (*p == ' ' || *p == '\t')) {
        p++;
        length--;
    }

    while (length > 0 && (p[length - 1] == ' ' || p[length - 1] == '\t')) {
        length--;
    }

    if (nxt_slow_path(nxt_off_t_parse(p, length) < 0)) {
        nxt_log(&r->task, NXT_LOG_WARN,
                "application sent invalid Content-Length \"%*s\"",
                (size_t) field->value_length, field->value);

        goto reject;
    }

    *slot = field;

    return NXT_OK;

reject:

    field->skip = 1;
    r->resp.content_length_n = -1;
    r->inconsistent = 1;

    return NXT_OK;
}
