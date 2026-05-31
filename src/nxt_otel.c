
/*
 * Copyright (C) F5, Inc.
 */

#include <math.h>

#include <nxt_router.h>
#include <nxt_router_request.h>
#include <nxt_application.h>
#include <nxt_http.h>
#include <nxt_otel.h>
#include <nxt_mp.h>
#include <nxt_work_queue.h>
#include <nxt_main.h>
#include <nxt_conf.h>
#include <nxt_types.h>
#include <nxt_string.h>
#include <nxt_sockaddr.h>
#include <nxt_clang.h>


#define NXT_OTEL_TRACEPARENT_LEN    55

/*
 * Span attribute keys. These follow the stable OpenTelemetry HTTP semantic
 * conventions (https://opentelemetry.io/docs/specs/semconv/http/http-spans/);
 * "http.flavor"/"http.user_agent" from older drafts are superseded by
 * "network.protocol.version"/"user_agent.original". "unit.application.*" are
 * FreeUnit-specific: a reverse proxy can't know the served app, but Unit can.
 */
#define NXT_OTEL_BODY_SIZE_TAG      "http.request.body.size"
#define NXT_OTEL_METHOD_TAG         "http.request.method"
#define NXT_OTEL_PATH_TAG           "url.path"
#define NXT_OTEL_SCHEME_TAG         "url.scheme"
#define NXT_OTEL_FLAVOR_TAG         "network.protocol.version"
#define NXT_OTEL_USER_AGENT_TAG     "user_agent.original"
#define NXT_OTEL_SERVER_ADDR_TAG    "server.address"
#define NXT_OTEL_CLIENT_ADDR_TAG    "client.address"
#define NXT_OTEL_APP_NAME_TAG       "unit.application.name"
#define NXT_OTEL_APP_TYPE_TAG       "unit.application.type"
#define NXT_OTEL_STATUS_CODE_TAG    "http.response.status_code"


static void
nxt_otel_state_transition(nxt_otel_state_t *state, nxt_otel_status_t status)
{
    if (status == NXT_OTEL_ERROR_STATE
        || state->status != NXT_OTEL_ERROR_STATE)
    {
        state->status = status;
    }
}


/*
 * Set a semantic-convention span attribute from a string-literal key and an
 * nxt_str_t value. Empty or absent values are skipped so we don't emit blank
 * attributes for headers the request didn't carry.
 */
static void
nxt_otel_add_attr(nxt_http_request_t *r, const char *key, nxt_str_t *val)
{
    nxt_str_t  k;

    if (val == NULL || val->start == NULL || val->length == 0) {
        return;
    }

    if (r->otel == NULL || r->otel->trace == NULL) {
        return;
    }

    k.start = (u_char *) key;
    k.length = nxt_strlen(key);

    nxt_otel_rs_add_attr(r->otel->trace, &k, val);
}


static const char *
nxt_otel_app_type_name(nxt_app_type_t type)
{
    switch (type) {
    case NXT_APP_PYTHON:
        return "python";
    case NXT_APP_PHP:
        return "php";
    case NXT_APP_PERL:
        return "perl";
    case NXT_APP_RUBY:
        return "ruby";
    case NXT_APP_JAVA:
        return "java";
    case NXT_APP_WASM:
    case NXT_APP_WASM_WC:
        return "wasm";
    case NXT_APP_EXTERNAL:
        return "external";
    default:
        return "unknown";
    }
}


static void
nxt_otel_propagate_header(nxt_task_t *task, nxt_http_request_t *r)
{
    u_char            *traceval;
    nxt_str_t         traceparent_name, traceparent;
    nxt_http_field_t  *f;

    traceval = nxt_mp_zalloc(r->mem_pool, NXT_OTEL_TRACEPARENT_LEN + 1);
    if (nxt_slow_path(traceval == NULL)) {
        /*
         * let it go blank here.
         * span still gets populated and sent
         * but data is not propagated to peer or app.
         */
        nxt_log(task, NXT_LOG_ERR,
                "couldn't allocate traceparent header. "
                "span will not propagate");
        return;
    }

    if (r->otel->trace_id != NULL) {
        // copy in the pre-existing traceparent for the response
        sprintf((char *) traceval, "%s-%s-%s-%s",
                (char *) r->otel->version,
                (char *) r->otel->trace_id,
                (char *) r->otel->parent_id,
                (char *) r->otel->trace_flags);

    /*
     * if we didn't inherit a trace id then we need to add the
     * traceparent header to the request
     */
    } else {

        nxt_otel_rs_copy_traceparent(traceval, r->otel->trace);

        f = nxt_list_add(r->fields);
        if (nxt_slow_path(f == NULL)) {
            return;
        }

        nxt_http_field_name_set(f, "traceparent");
        f->value = traceval;
        f->value_length = nxt_strlen(traceval);

        traceparent_name = (nxt_str_t) {
            .start  = f->name,
            .length = f->name_length,
        };

        traceparent = (nxt_str_t) {
            .start  = f->value,
            .length = f->value_length,
        };

        nxt_otel_rs_add_event_to_trace(r->otel->trace,
                                       &traceparent_name, &traceparent);
    }

    f = nxt_list_add(r->resp.fields);
    if (nxt_slow_path(f == NULL)) {
        nxt_log(task, NXT_LOG_ERR,
                "couldn't allocate traceparent header in response");
        return;
    }

    nxt_http_field_name_set(f, "traceparent");
    f->value = traceval;
    f->value_length = nxt_strlen(traceval);
}


static void
nxt_otel_span_add_headers(nxt_task_t *task, nxt_http_request_t *r)
{
    nxt_str_t  val;

    nxt_log(task, NXT_LOG_DEBUG, "adding attributes to trace");

    if (r->otel == NULL || r->otel->trace == NULL) {
        nxt_log(task, NXT_LOG_ERR, "no trace to add attributes to!");
        nxt_otel_state_transition(r->otel, NXT_OTEL_ERROR_STATE);
        return;
    }

    /*
     * Record only well-defined semconv attributes. We deliberately do NOT
     * iterate every request header: that would leak sensitive values
     * (Authorization, Cookie, ...) into the telemetry backend.
     */
    nxt_otel_add_attr(r, NXT_OTEL_METHOD_TAG, r->method);
    nxt_otel_add_attr(r, NXT_OTEL_PATH_TAG, r->path);

    nxt_str_set(&val, "http");
    if (r->tls) {
        nxt_str_set(&val, "https");
    }
    nxt_otel_add_attr(r, NXT_OTEL_SCHEME_TAG, &val);

    /* "HTTP/1.1" -> "1.1" for network.protocol.version */
    val = r->version;
    if (val.length > nxt_length("HTTP/")
        && memcmp(val.start, "HTTP/", nxt_length("HTTP/")) == 0)
    {
        val.start += nxt_length("HTTP/");
        val.length -= nxt_length("HTTP/");
    }
    nxt_otel_add_attr(r, NXT_OTEL_FLAVOR_TAG, &val);

    if (r->user_agent != NULL) {
        val.start = r->user_agent->value;
        val.length = r->user_agent->value_length;
        nxt_otel_add_attr(r, NXT_OTEL_USER_AGENT_TAG, &val);
    }

    nxt_otel_add_attr(r, NXT_OTEL_SERVER_ADDR_TAG, &r->host);

    if (r->remote != NULL) {
        val.start = nxt_sockaddr_address(r->remote);
        val.length = r->remote->address_length;
        nxt_otel_add_attr(r, NXT_OTEL_CLIENT_ADDR_TAG, &val);
    }

    nxt_otel_propagate_header(task, r);

    nxt_otel_state_transition(r->otel, NXT_OTEL_BODY_STATE);
}


static void
nxt_otel_span_add_body(nxt_http_request_t *r)
{
    size_t     body_size = 0;
    size_t     buf_size;
    u_char     *body_buf, *body_size_buf;
    nxt_int_t  cur;
    nxt_str_t  body_val;

    if (r->body != NULL) {
        body_size = nxt_buf_used_size(r->body);
    }

    buf_size = 1; // first digit
    if (body_size != 0) {
        buf_size += log10(body_size); // subsequent digits
    }
    buf_size += 1; // \0
    buf_size += nxt_strlen(NXT_OTEL_BODY_SIZE_TAG);
    buf_size += 1; // \0

    body_buf = nxt_mp_alloc(r->mem_pool, buf_size);
    if (nxt_slow_path(body_buf == NULL)) {
        return;
    }

    cur = sprintf((char *) body_buf, "%lu", body_size);
    if (cur < 0) {
        return;
    }

    cur += 1;
    body_size_buf = body_buf + cur;
    nxt_cpystr(body_buf + cur, (const u_char *) NXT_OTEL_BODY_SIZE_TAG);

    body_val = (nxt_str_t) {
        .start  = body_buf,
        .length = nxt_strlen(body_buf),
    };

    nxt_otel_add_attr(r, (const char *) body_size_buf, &body_val);
    nxt_otel_state_transition(r->otel, NXT_OTEL_COLLECT_STATE);
}


static void
nxt_otel_span_add_status(nxt_task_t *task, nxt_http_request_t *r)
{
    int                     n;
    u_char                  status_buf[8];
    const char              *type_name;
    nxt_str_t               val;
    nxt_app_t               *app;
    nxt_request_rpc_data_t  *rpc;

    if (r->otel == NULL || r->otel->trace == NULL) {
        return;
    }

    /*
     * Application identity is resolved during routing, so it is only known by
     * the time the span is collected (after the response). A reverse proxy
     * can't emit this; Unit can.
     */
    rpc = r->req_rpc_data;
    if (rpc != NULL && rpc->app != NULL) {
        app = rpc->app;
        nxt_otel_add_attr(r, NXT_OTEL_APP_NAME_TAG, &app->name);

        type_name = nxt_otel_app_type_name(app->type);
        val.start = (u_char *) type_name;
        val.length = nxt_strlen(type_name);
        nxt_otel_add_attr(r, NXT_OTEL_APP_TYPE_TAG, &val);
    }

    // dont bother logging an unset status
    if (r->status == 0) {
        return;
    }

    n = sprintf((char *) status_buf, "%d", (int) r->status);
    if (n > 0) {
        val.start = status_buf;
        val.length = (size_t) n;
        nxt_otel_add_attr(r, NXT_OTEL_STATUS_CODE_TAG, &val);
    }

    /* Flag server errors so the span shows Status::Error in the collector. */
    if (r->status >= NXT_HTTP_INTERNAL_SERVER_ERROR) {
        nxt_otel_rs_set_error(r->otel->trace);
    }
}


static void
nxt_otel_span_collect(nxt_task_t *task, nxt_http_request_t *r)
{
    if (r->otel->trace == NULL) {
        nxt_log(task, NXT_LOG_ERR, "otel error: no trace to send!");
        nxt_otel_state_transition(r->otel, NXT_OTEL_ERROR_STATE);
        return;
    }

    nxt_otel_span_add_status(task, r);
    nxt_otel_state_transition(r->otel, NXT_OTEL_UNINIT_STATE);
    nxt_otel_rs_send_trace(r->otel->trace);

    r->otel->trace = NULL;
}


static void
nxt_otel_error(nxt_task_t *task, nxt_http_request_t *r)
{
    // purposefully not using state transition helper
    r->otel->status = NXT_OTEL_UNINIT_STATE;
    nxt_log(task, NXT_LOG_ERR, "otel error condition");

    /*
     * assumable at time of writing that there is no
     * r->otel->trace to leak. This state is only set
     * in cases where trace fails to generate or is missing
     */
}


static void
nxt_otel_trace_and_span_init(nxt_task_t *task, nxt_http_request_t *r)
{
    r->otel->trace =
        nxt_otel_rs_get_or_create_trace(r->otel->trace_id,
                                        r->otel->parent_id,
                                        r->otel->trace_flags,
                                        &r->otel->trace_state);
    if (r->otel->trace == NULL) {
        nxt_log(task, NXT_LOG_ERR, "error generating otel span");
        nxt_otel_state_transition(r->otel, NXT_OTEL_ERROR_STATE);
        return;
    }

    nxt_otel_state_transition(r->otel, NXT_OTEL_HEADER_STATE);
}


void
nxt_otel_test_and_call_state(nxt_task_t *task, nxt_http_request_t *r)
{
    if (r == NULL || r->otel == NULL) {
        return;
    }

    switch (r->otel->status) {
    case NXT_OTEL_UNINIT_STATE:
        return;
    case NXT_OTEL_INIT_STATE:
        nxt_otel_trace_and_span_init(task, r);
        break;
    case NXT_OTEL_HEADER_STATE:
        nxt_otel_span_add_headers(task, r);
        break;
    case NXT_OTEL_BODY_STATE:
        nxt_otel_span_add_body(r);
        break;
    case NXT_OTEL_COLLECT_STATE:
        nxt_otel_span_collect(task, r);
        break;
    case NXT_OTEL_ERROR_STATE:
        nxt_otel_error(task, r);
        break;
    }
}


// called in nxt_http_request_error
void
nxt_otel_request_error_path(nxt_task_t *task, nxt_http_request_t *r)
{
    if (r->otel == NULL || r->otel->trace == NULL) {
        return;
    }

    // response headers have been cleared
    nxt_otel_propagate_header(task, r);
    nxt_otel_state_transition(r->otel, NXT_OTEL_COLLECT_STATE);
    nxt_otel_test_and_call_state(task, r);
}


nxt_int_t
nxt_otel_parse_traceparent(void *ctx, nxt_http_field_t *field, uintptr_t data)
{
    char                *copy;
    nxt_http_request_t  *r;

    /*
     * For information on parsing the traceparent header:
     * https://www.w3.org/TR/trace-context/#traceparent-header
     * A summary of the traceparent header value format follows:
     * Traceparent: "$a-$b-$c-$d"
     *   a. version (2 hex digits) (ff is forbidden)
     *   b. trace_id (32 hex digits) (all zeroes forbidden)
     *   c. parent_id (16 hex digits) (all zeroes forbidden)
     *   d. flags (2 hex digits)
     */

    r = ctx;
    if (r->otel == NULL) {
        return NXT_OK;
    }

    if (field->value_length != NXT_OTEL_TRACEPARENT_LEN) {
        goto error_state;
    }

    /*
     * strsep is destructive so we make a copy of the field
     */
    copy = nxt_mp_zalloc(r->mem_pool, field->value_length + 1);
    if (nxt_slow_path(copy == NULL)) {
        goto error_state;
    }
    memcpy(copy, field->value, field->value_length);

    r->otel->version = (u_char *) strsep(&copy, "-");
    r->otel->trace_id = (u_char *) strsep(&copy, "-");
    r->otel->parent_id = (u_char *) strsep(&copy, "-");
    r->otel->trace_flags = (u_char *) strsep(&copy, "-");

    if (r->otel->version        == NULL
        || r->otel->trace_id    == NULL
        || r->otel->parent_id   == NULL
        || r->otel->trace_flags == NULL)
    {
        goto error_state;
    }

    return NXT_OK;

error_state:
    nxt_otel_state_transition(r->otel, NXT_OTEL_ERROR_STATE);

    return NXT_ERROR;
}


nxt_int_t
nxt_otel_parse_tracestate(void *ctx, nxt_http_field_t *field, uintptr_t data)
{
    nxt_str_t           s;
    nxt_http_field_t    *f;
    nxt_http_request_t  *r;

    s.length = field->value_length;
    s.start = field->value;

    r = ctx;
    if (r->otel == NULL) {
        return NXT_OK;
    }

    r->otel->trace_state = s;

    /*
     * trace_state is forwarded into the Rust SDK at span creation
     * (nxt_otel_trace_and_span_init -> nxt_otel_rs_get_or_create_trace), so
     * vendor context is preserved on the continued trace. We also echo it back
     * to the peer in the response below.
     */

    f = nxt_list_add(r->resp.fields);
    if (nxt_fast_path(f != NULL)) {
        *f = *field;
    }

    return NXT_OK;
}


void
nxt_otel_log_callback(nxt_uint_t log_level, const char *arg)
{
    nxt_thread_t  *thr = nxt_thread();

    nxt_log(thr->task, log_level, "otel: %s", arg);
}
