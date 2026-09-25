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


/*
 * Span attribute keys, as ids rather than strings.
 *
 * The key text lives on the Rust side in ATTR_KEYS, as a const table of
 * Key::from_static_str -- so a key costs no allocation and no strlen per
 * request.  The order of this enum and of that table is the contract between
 * the two sides: the id indexes the table directly.  Add to the end, and add
 * to ATTR_KEYS in src/otel/src/lib.rs in the same commit.
 */
typedef enum {
    NXT_OTEL_ATTR_METHOD = 0,
    NXT_OTEL_ATTR_PATH,
    NXT_OTEL_ATTR_SCHEME,
    NXT_OTEL_ATTR_FLAVOR,
    NXT_OTEL_ATTR_USER_AGENT,
    NXT_OTEL_ATTR_SERVER_ADDR,
    NXT_OTEL_ATTR_CLIENT_ADDR,
    NXT_OTEL_ATTR_APP_NAME,
    NXT_OTEL_ATTR_APP_TYPE,
    NXT_OTEL_ATTR_STATUS_CODE,
    NXT_OTEL_ATTR_BODY_SIZE,
    NXT_OTEL_ATTR_MAX
} nxt_otel_attr_id_t;


/* Which member of nxt_otel_attr_t carries the value. */
#define NXT_OTEL_ATTR_TYPE_STR  0
#define NXT_OTEL_ATTR_TYPE_I64  1


/*
 * One span attribute in a batch.  Mirrored by nxt_otel_attr_t in
 * src/otel/src/lib.rs; the layouts must stay identical.
 *
 * The Rust struct is #[repr(C)], so both sides lay their fields out under
 * the target's C ABI: a change mirrored on both sides, reordering included,
 * stays in agreement on every target.  What breaks the FFI is editing one
 * side only, or pairing fields whose types are not the same size and
 * alignment everywhere we ship -- C long is 4 bytes on ILP32 where Rust i64
 * is always 8, and C enum sizes are implementation-defined, which is why
 * key_id and type are uint32_t here rather than nxt_otel_attr_id_t.
 *
 * The offsets below are the same on x86-64, armv7 and i386 (the total size
 * is not: 32 on 64-bit, 24 on 32-bit, where int64_t needs only 4-byte
 * alignment).  They are asserted rather than described, so that a field
 * added, reordered or appended here fails the build and prompts the matching
 * edit on the Rust side, which these assertions cannot see.
 *
 * The size assertion is what covers a field appended after sval: the offset
 * checks alone would all still hold, while the struct grew.  That matters
 * more than it looks, because these are passed as an array -- a C-side-only
 * trailing field changes the stride C writes with and not the one Rust reads
 * with, so every attribute after the first would be misread.
 */
typedef struct {
    uint32_t   key_id;
    uint32_t   type;
    int64_t    ival;
    nxt_str_t  sval;
} nxt_otel_attr_t;

nxt_static_assert(offsetof(nxt_otel_attr_t, key_id) == 0,
                  "nxt_otel_attr_t layout drifted from src/otel/src/lib.rs");
nxt_static_assert(offsetof(nxt_otel_attr_t, type) == 4,
                  "nxt_otel_attr_t layout drifted from src/otel/src/lib.rs");
nxt_static_assert(offsetof(nxt_otel_attr_t, ival) == 8,
                  "nxt_otel_attr_t layout drifted from src/otel/src/lib.rs");
nxt_static_assert(offsetof(nxt_otel_attr_t, sval) == 2 * sizeof(uint32_t)
                                                     + sizeof(int64_t),
                  "nxt_otel_attr_t layout drifted from src/otel/src/lib.rs");
nxt_static_assert(sizeof(nxt_otel_attr_t) == offsetof(nxt_otel_attr_t, sval)
                                             + sizeof(nxt_str_t),
                  "nxt_otel_attr_t grew a field Rust does not know about");


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
extern void nxt_otel_rs_add_attrs(void *trace,
    const nxt_otel_attr_t *attrs, size_t n);
extern void nxt_otel_rs_set_error(void *trace);
extern uint8_t nxt_otel_rs_is_recording(void *trace);
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
    /* The sampler's verdict on this span, cached at creation. */
    uint8_t            recording;
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
