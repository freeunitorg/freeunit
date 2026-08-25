/*
 * Copyright (C) F5, Inc.
 */

#ifndef _NXT_OTEL_H_INCLUDED_
#define _NXT_OTEL_H_INCLUDED_

#include <nxt_router.h>
#include <nxt_string.h>


#if (NXT_HAVE_OTEL)
#define NXT_OTEL_TRACE()  nxt_otel_test_and_call_state(task, r)
#else
#define NXT_OTEL_TRACE()
#endif


#if (NXT_HAVE_OTEL)
extern void nxt_otel_rs_send_trace(void *trace);
extern void * nxt_otel_rs_get_or_create_trace(const u_char *trace_id,
    const u_char *parent_id, const u_char *trace_flags,
    const nxt_str_t *trace_state);
extern void nxt_otel_rs_init(
    void (*log_callback)(nxt_uint_t log_level, const char *log_string),
    const nxt_str_t *endpoint, const nxt_str_t *protocol,
    double sample_fraction, double batch_size);
extern void nxt_otel_rs_copy_traceparent(u_char *buffer, void *span);
extern void nxt_otel_rs_add_event_to_trace(void *trace, nxt_str_t *key,
    nxt_str_t *val);
extern void nxt_otel_rs_add_attr(void *trace, nxt_str_t *key, nxt_str_t *val);
extern void nxt_otel_rs_set_error(void *trace);
extern uint8_t nxt_otel_rs_is_init(void);
extern void nxt_otel_rs_uninit(void);
extern uint8_t nxt_otel_rs_shutdown_bounded(uint64_t timeout_ms);
/*
 * Span export health for the /status API: writes the number of spans the
 * exporter accepted and the number it rejected since the live provider was
 * installed, and returns non-zero when a provider is installed at all.  When
 * it returns 0 the two counters are meaningless and must not be reported.
 */
extern uint8_t nxt_otel_rs_export_stats(uint64_t *exported, uint64_t *failed);

/*
 * Return values of nxt_otel_rs_shutdown_bounded().  Defined on the Rust side
 * as NXT_OTEL_SHUTDOWN_* in src/otel/src/lib.rs; the two lists must be kept
 * in step.
 *
 * TIMEOUT  the flush did not answer within the caller's budget.
 * FLUSHED  the flush completed and no export has failed since the provider
 *          was installed.  Not a promise that no spans were lost: see the
 *          producer race documented on the Rust side and in issue #219.
 * FAILED   an export failed -- either the one this call forced, or an earlier
 *          batch recorded by the sticky failure flag.
 */
#define NXT_OTEL_SHUTDOWN_TIMEOUT  0
#define NXT_OTEL_SHUTDOWN_FLUSHED  1
#define NXT_OTEL_SHUTDOWN_FAILED   2
#endif


typedef enum nxt_otel_status_e   nxt_otel_status_t;
typedef struct nxt_otel_state_s  nxt_otel_state_t;


/*
 * nxt_otel_status_t
 * more efficient than a single handler state struct
 */
enum nxt_otel_status_e {
    NXT_OTEL_UNINIT_STATE = 0,
    NXT_OTEL_INIT_STATE,
    NXT_OTEL_HEADER_STATE,
    NXT_OTEL_BODY_STATE,
    NXT_OTEL_COLLECT_STATE,
    NXT_OTEL_ERROR_STATE,
};


/*
 * nxt_otel_state_t
 * cache of trace data needed per request and
 * includes indicator as to current flow state
 */
struct nxt_otel_state_s {
    u_char             *trace_id;
    u_char             *version;
    u_char             *parent_id;
    u_char             *trace_flags;
    void               *trace;
    nxt_otel_status_t  status;
    nxt_str_t          trace_state;
};


nxt_int_t nxt_otel_parse_traceparent(void *ctx, nxt_http_field_t *field,
    uintptr_t data);
nxt_int_t nxt_otel_parse_tracestate(void *ctx, nxt_http_field_t *field,
    uintptr_t data);
void nxt_otel_log_callback(nxt_uint_t log_level, const char *arg);


void nxt_otel_test_and_call_state(nxt_task_t *task, nxt_http_request_t *r);
void nxt_otel_request_error_path(nxt_task_t *task, nxt_http_request_t *r);
void nxt_otel_shutdown(nxt_task_t *task);


#endif /* _NXT_OTEL_H_INCLUDED_ */
