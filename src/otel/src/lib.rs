#![allow(non_camel_case_types)]

//! OpenTelemetry trace export for FreeUnit.
//!
//! Two OTLP transports, selected at runtime by `settings/telemetry/protocol`:
//! `"http"` (default) uses the blocking reqwest client and needs no async
//! executor; `"grpc"` uses tonic over a small multi-thread tokio runtime that
//! this crate owns (built lazily, dropped on shutdown). Both are driven by the
//! stable dedicated-thread `BatchSpanProcessor`. v1 is plaintext only — no TLS
//! to the collector on either transport.
//!
//! A finished span is handed to C as a raw `*mut BoxedSpan`. Ending the span
//! (on drop in `nxt_otel_rs_send_trace`) enqueues it into the batch processor,
//! which exports it from its own background thread.

use opentelemetry::global;
use opentelemetry::global::BoxedSpan;
use opentelemetry::trace::{
    Span, SpanContext, SpanId, SpanKind, Status, TraceContextExt, TraceFlags,
    TraceId, TraceState, Tracer, TracerProvider,
};
use opentelemetry::{Context, KeyValue};
use opentelemetry_otlp::{Protocol, SpanExporter, WithExportConfig};
use opentelemetry_sdk::trace::{
    BatchConfigBuilder, BatchSpanProcessor, Sampler, SdkTracerProvider,
};
use opentelemetry_sdk::Resource;
use std::ffi::{c_char, CStr, CString};
use std::str::FromStr;
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

/// The tokio runtime owning the gRPC exporter's tonic channel. The blocking
/// batch processor drives export RPCs onto it from its own thread, so it must
/// outlive the provider; it is dropped in `nxt_otel_rs_shutdown_tracer`.
fn runtime_slot() -> &'static Mutex<Option<tokio::runtime::Runtime>> {
    static RT: Mutex<Option<tokio::runtime::Runtime>> = Mutex::new(None);
    &RT
}

/// Build the OTLP/HTTP exporter: the blocking reqwest client, no async runtime.
fn build_http_exporter(endpoint: String) -> Result<SpanExporter, String> {
    SpanExporter::builder()
        .with_http()
        .with_endpoint(endpoint)
        .with_protocol(Protocol::HttpBinary)
        .with_timeout(EXPORT_TIMEOUT)
        .build()
        .map_err(|e| format!("couldn't build otel http exporter: {e}"))
}

/// Build the OTLP/gRPC exporter (tonic). A small multi-thread tokio runtime is
/// created and stashed so its reactor stays alive: the tonic channel is built
/// inside the runtime context, and the batch processor's blocking export later
/// dispatches RPCs onto it. v1 is plaintext h2c — no TLS to the collector.
fn build_grpc_exporter(endpoint: String) -> Result<SpanExporter, String> {
    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(1)
        .enable_all()
        .build()
        .map_err(|e| format!("couldn't build tokio runtime for otel grpc: {e}"))?;

    let exporter = {
        let _guard = rt.enter();
        SpanExporter::builder()
            .with_tonic()
            .with_endpoint(endpoint)
            .with_timeout(EXPORT_TIMEOUT)
            .build()
            .map_err(|e| format!("couldn't build otel grpc exporter: {e}"))?
    };

    if let Ok(mut slot) = runtime_slot().lock() {
        *slot = Some(rt);
    }
    Ok(exporter)
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

/// Initialise the global tracer provider for OTLP export.
///
/// `protocol` selects the transport: `"http"` or `"grpc"`; anything else is
/// rejected via `log_callback`.
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

    if proto != "http" && proto != "grpc" {
        log_err(
            log_callback,
            format!("unsupported otel protocol {proto:?}: expected \"http\" or \"grpc\""),
        );
        return;
    }

    // Start from a clean slate: flush and drop any prior provider (and, if the
    // prior config used grpc, its tokio runtime).
    nxt_otel_rs_shutdown_tracer();

    let exporter = match if proto == "grpc" {
        build_grpc_exporter(endpoint)
    } else {
        build_http_exporter(endpoint)
    } {
        Ok(e) => e,
        Err(msg) => {
            log_err(log_callback, msg);
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

/// Set a semantic-convention span attribute (e.g. `http.request.method`).
/// Unlike `add_event_to_trace`, this records structured span attributes the
/// collector can index and query, not timestamped events.
#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_add_attr(
    trace: *mut BoxedSpan,
    key: *const nxt_str_t,
    val: *const nxt_str_t,
) {
    if trace.is_null() || key.is_null() || val.is_null() {
        return;
    }

    let key = nxt_str_to_string(&*key);
    let val = nxt_str_to_string(&*val);

    (*trace).set_attribute(KeyValue::new(key, val));
}

/// Mark the span as errored. Called by C for 5xx responses so the trace is
/// flagged `Status::Error` in the collector, matching nginx-otel/Caddy.
#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_set_error(trace: *mut BoxedSpan) {
    if trace.is_null() {
        return;
    }

    (*trace).set_status(Status::error(""));
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
    trace_state: *const nxt_str_t,
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

    // Forward the inherited W3C `tracestate` so vendor context is preserved on
    // the continued trace; an unparseable or absent value falls back to empty.
    let state = if trace_state.is_null() {
        TraceState::default()
    } else {
        TraceState::from_str(&nxt_str_to_string(&*trace_state)).unwrap_or_default()
    };

    let sc = SpanContext::new(tid, sid, flags, true, state);
    Some(Context::new().with_remote_span_context(sc))
}

#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_get_or_create_trace(
    trace_id: *const c_char,
    parent_id: *const c_char,
    trace_flags: *const c_char,
    trace_state: *const nxt_str_t,
) -> *mut BoxedSpan {
    let tracer = global::tracer_provider().tracer(TRACER_NAME);
    let builder = tracer.span_builder(SPAN_NAME).with_kind(SpanKind::Server);

    let parent = nxt_otel_parent_context(trace_id, parent_id, trace_flags, trace_state)
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
        // Flushes pending spans; on a grpc build this still needs the runtime,
        // so the provider is shut down before the runtime is dropped below.
        let _ = provider.shutdown();
    }

    // Drop the gRPC runtime (if the live config used grpc) after the provider
    // shutdown above has flushed through it.
    let rt = runtime_slot().lock().ok().and_then(|mut g| g.take());
    if let Some(rt) = rt {
        rt.shutdown_background();
    }
}
