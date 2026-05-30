#![allow(non_camel_case_types)]

//! OpenTelemetry trace export for FreeUnit.
//!
//! HTTP/OTLP only, by design. The exporter uses the blocking reqwest client
//! driven by the stable dedicated-thread `BatchSpanProcessor`. There is no
//! tokio runtime and no async executor here — that keeps the FFI surface
//! simple and the runtime behaviour predictable for an LTS fork.
//!
//! A finished span is handed to C as a raw `*mut BoxedSpan`. Ending the span
//! (on drop in `nxt_otel_rs_send_trace`) enqueues it into the batch processor,
//! which exports it from its own background thread.

use opentelemetry::global;
use opentelemetry::global::BoxedSpan;
use opentelemetry::trace::{
    Span, SpanContext, SpanId, SpanKind, TraceContextExt, TraceFlags, TraceId,
    TraceState, Tracer, TracerProvider,
};
use opentelemetry::{Context, KeyValue};
use opentelemetry_otlp::{Protocol, SpanExporter, WithExportConfig};
use opentelemetry_sdk::trace::{
    BatchConfigBuilder, BatchSpanProcessor, Sampler, SdkTracerProvider,
};
use opentelemetry_sdk::Resource;
use std::ffi::{c_char, CStr, CString};
use std::sync::Mutex;
use std::time::Duration;
use std::{ptr, slice};

const TRACEPARENT_HEADER_LEN: u8 = 55;
const EXPORT_TIMEOUT: Duration = Duration::from_secs(10);
const MAX_QUEUE_SIZE: usize = 4096;
const SERVICE_NAME: &str = "FreeUnit";
const TRACER_NAME: &str = "FreeUnit";
const SPAN_NAME: &str = "request";

const NXT_LOG_ERR: nxt_uint_t = 1;

#[repr(C)]
pub struct nxt_str_t {
    pub length: usize,
    pub start: *const u8,
}

#[cfg(target_arch = "x86_64")]
pub type nxt_uint_t = ::std::os::raw::c_uint;

#[cfg(not(target_arch = "x86_64"))]
pub type nxt_uint_t = usize;

type nxt_otel_log_cb = unsafe extern "C" fn(log_level: nxt_uint_t, msg: *const c_char);

/// The live tracer provider. Held so we can flush and shut it down cleanly on
/// reconfigure or teardown. `None` means OTel is not currently configured.
fn provider_slot() -> &'static Mutex<Option<SdkTracerProvider>> {
    static PROVIDER: Mutex<Option<SdkTracerProvider>> = Mutex::new(None);
    &PROVIDER
}

/// Copy a `nxt_str_t` into an owned `String`. The caller guarantees `s.start`
/// points at `s.length` valid bytes for the duration of the call; we copy
/// because batch-exported spans outlive the request memory these reference.
unsafe fn nxt_str_to_string(s: &nxt_str_t) -> String {
    // `slice::from_raw_parts` requires a non-null, aligned pointer even when
    // the length is zero. C may hand us a `nxt_str_t` with a NULL `start` for
    // an empty or uninitialised value, so guard against it to avoid UB.
    if s.start.is_null() || s.length == 0 {
        return String::new();
    }
    // Header values are arbitrary bytes, not guaranteed UTF-8; `from_utf8_lossy`
    // replaces any invalid sequence with U+FFFD instead of constructing an
    // invalid `String` (which `from_utf8_unchecked` would — that is itself UB).
    String::from_utf8_lossy(slice::from_raw_parts(s.start, s.length)).into_owned()
}

/// Log a message through the C callback. `msg` must be a valid C string body.
unsafe fn log_err(cb: nxt_otel_log_cb, msg: String) {
    if let Ok(cmsg) = CString::new(msg) {
        cb(NXT_LOG_ERR, cmsg.as_ptr());
    }
}

#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_is_init() -> u8 {
    provider_slot()
        .lock()
        .map(|g| g.is_some() as u8)
        .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_uninit() {
    nxt_otel_rs_shutdown_tracer();
}

/// Initialise the global tracer provider for OTLP/HTTP export.
///
/// `protocol` must be `"http"`; anything else is rejected via `log_callback`.
/// Re-invoking this flushes and replaces any previously configured provider.
#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_init(
    log_callback: nxt_otel_log_cb,
    endpoint: *const nxt_str_t,
    protocol: *const nxt_str_t,
    sample_fraction: f64,
    batch_size: f64,
) {
    if endpoint.is_null() || protocol.is_null() {
        return;
    }

    let endpoint = nxt_str_to_string(&*endpoint);
    let proto = nxt_str_to_string(&*protocol).to_lowercase();

    if proto != "http" {
        log_err(
            log_callback,
            format!("unsupported otel protocol {proto:?}: only \"http\" is supported"),
        );
        return;
    }

    // Start from a clean slate: flush and drop any prior provider.
    nxt_otel_rs_shutdown_tracer();

    let exporter = match SpanExporter::builder()
        .with_http()
        .with_endpoint(endpoint)
        .with_protocol(Protocol::HttpBinary)
        .with_timeout(EXPORT_TIMEOUT)
        .build()
    {
        Ok(e) => e,
        Err(e) => {
            log_err(log_callback, format!("couldn't build otel exporter: {e}"));
            return;
        }
    };

    let processor = BatchSpanProcessor::builder(exporter)
        .with_batch_config(
            BatchConfigBuilder::default()
                .with_max_export_batch_size(batch_size as usize)
                .with_max_queue_size(MAX_QUEUE_SIZE)
                .build(),
        )
        .build();

    let provider = SdkTracerProvider::builder()
        .with_span_processor(processor)
        .with_resource(
            Resource::builder().with_service_name(SERVICE_NAME).build(),
        )
        // ParentBased honours an upstream sampling decision carried in
        // traceparent; falls back to ratio sampling for new roots.
        .with_sampler(Sampler::ParentBased(Box::new(
            Sampler::TraceIdRatioBased(sample_fraction),
        )))
        .build();

    global::set_tracer_provider(provider.clone());

    if let Ok(mut slot) = provider_slot().lock() {
        *slot = Some(provider);
    }
}

// it's on the caller to pass in a buf of proper length
#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_copy_traceparent(buf: *mut c_char, span: *const BoxedSpan) {
    if buf.is_null() || span.is_null() {
        return;
    }

    let ctx = (*span).span_context();
    let traceparent = format!(
        "00-{:032x}-{:016x}-{:02x}",
        ctx.trace_id(),    // 16 bytes, 32 hex
        ctx.span_id(),     // 8 bytes, 16 hex
        ctx.trace_flags()  // 1 byte, 2 hex
    );

    debug_assert_eq!(traceparent.len(), TRACEPARENT_HEADER_LEN as usize);

    ptr::copy_nonoverlapping(
        traceparent.as_bytes().as_ptr() as *const c_char,
        buf,
        TRACEPARENT_HEADER_LEN as usize,
    );
    // null terminator
    *buf.add(TRACEPARENT_HEADER_LEN as usize) = 0;
}

#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_add_event_to_trace(
    trace: *mut BoxedSpan,
    key: *const nxt_str_t,
    val: *const nxt_str_t,
) {
    if trace.is_null() || key.is_null() || val.is_null() {
        return;
    }

    let key = nxt_str_to_string(&*key);
    let val = nxt_str_to_string(&*val);

    (*trace).add_event("Unit Attribute".to_string(), vec![KeyValue::new(key, val)]);
}

/// Build a parent context from an inherited traceparent, if all parts parse.
///
/// In OTel 0.32 the trace id can no longer be forced onto a `SpanBuilder`; a
/// continued trace must be expressed as a remote parent `SpanContext`. The new
/// span then inherits the trace id and links to `parent_id`, and `ParentBased`
/// sampling honours the inherited `trace_flags`.
unsafe fn nxt_otel_parent_context(
    trace_id: *const c_char,
    parent_id: *const c_char,
    trace_flags: *const c_char,
) -> Option<Context> {
    if trace_id.is_null() || parent_id.is_null() {
        return None;
    }

    let tid = TraceId::from_hex(&CStr::from_ptr(trace_id).to_string_lossy()).ok()?;
    let sid = SpanId::from_hex(&CStr::from_ptr(parent_id).to_string_lossy()).ok()?;

    let flags = if trace_flags.is_null() {
        TraceFlags::SAMPLED
    } else {
        u8::from_str_radix(CStr::from_ptr(trace_flags).to_string_lossy().trim(), 16)
            .map(TraceFlags::new)
            .unwrap_or(TraceFlags::SAMPLED)
    };

    let sc = SpanContext::new(tid, sid, flags, true, TraceState::default());
    Some(Context::new().with_remote_span_context(sc))
}

#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_get_or_create_trace(
    trace_id: *const c_char,
    parent_id: *const c_char,
    trace_flags: *const c_char,
) -> *mut BoxedSpan {
    let tracer = global::tracer_provider().tracer(TRACER_NAME);
    let builder = tracer.span_builder(SPAN_NAME).with_kind(SpanKind::Server);

    let parent = nxt_otel_parent_context(trace_id, parent_id, trace_flags)
        .unwrap_or_else(Context::new);
    let span = tracer.build_with_context(builder, &parent);

    Box::into_raw(Box::new(span))
}

#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_send_trace(trace: *mut BoxedSpan) {
    if trace.is_null() {
        return;
    }

    // Reclaim ownership of the span allocated in nxt_otel_rs_get_or_create_trace
    // and end it. Ending enqueues the span into the batch processor, which
    // exports it from its own background thread; the Box is then dropped here.
    let mut span = Box::from_raw(trace);
    span.end();
}

/// Flush and tear down the live tracer provider, if any.
#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_shutdown_tracer() {
    let provider = provider_slot().lock().ok().and_then(|mut g| g.take());
    if let Some(provider) = provider {
        let _ = provider.shutdown();
    }
}
