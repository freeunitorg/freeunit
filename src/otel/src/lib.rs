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
use opentelemetry::global::{BoxedSpan, BoxedTracer};
use opentelemetry::trace::{
    Span, SpanContext, SpanId, SpanKind, Status, TraceContextExt, TraceFlags,
    TraceId, TraceState, Tracer, TracerProvider,
};
use opentelemetry::{Context, KeyValue};
use opentelemetry_otlp::{Protocol, SpanExporter, WithExportConfig};
use opentelemetry_sdk::error::OTelSdkResult;
use opentelemetry_sdk::trace::{
    BatchConfigBuilder, BatchSpanProcessor, Sampler, SdkTracerProvider, SpanData,
    SpanExporter as SdkSpanExporter,
};
use opentelemetry_sdk::Resource;
use std::ffi::{c_char, CStr, CString};
use std::str::FromStr;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::{Mutex, RwLock};
use std::time::Duration;
use std::{ptr, slice};

const TRACEPARENT_HEADER_LEN: u8 = 55;
const EXPORT_TIMEOUT: Duration = Duration::from_secs(10);
const MAX_QUEUE_SIZE: usize = 4096;
const SERVICE_NAME: &str = "FreeUnit";
const TRACER_NAME: &str = "FreeUnit";
const SPAN_NAME: &str = "request";

const NXT_LOG_ERR: nxt_uint_t = 1;

/// Return values of `nxt_otel_rs_shutdown_bounded`.
///
/// These are mirrored as `NXT_OTEL_SHUTDOWN_*` in `src/nxt_otel.h`, which is
/// the only consumer; the two lists must be kept in step.
const NXT_OTEL_SHUTDOWN_TIMEOUT: u8 = 0;
const NXT_OTEL_SHUTDOWN_FLUSHED: u8 = 1;
const NXT_OTEL_SHUTDOWN_FAILED: u8 = 2;

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

/// The tracer built from the live provider.
///
/// `global::tracer_provider().tracer()` takes the global provider's lock,
/// clones the provider and heap-allocates a fresh tracer on every call, but a
/// tracer is meant to be built once. Caching it leaves the request path with
/// one uncontended read lock instead. `None` means no provider is installed;
/// the request path then falls back to the global one, so behaviour is
/// unchanged either way.
fn tracer_slot() -> &'static RwLock<Option<BoxedTracer>> {
    static TRACER: RwLock<Option<BoxedTracer>> = RwLock::new(None);
    &TRACER
}

/// The tokio runtime owning the gRPC exporter's tonic channel. The blocking
/// batch processor drives export RPCs onto it from its own thread, so it must
/// outlive the provider; it is dropped in `nxt_otel_rs_shutdown_tracer`.
fn runtime_slot() -> &'static Mutex<Option<tokio::runtime::Runtime>> {
    static RT: Mutex<Option<tokio::runtime::Runtime>> = Mutex::new(None);
    &RT
}

/// Set whenever an export returns `Err`, and cleared when a new provider is
/// installed. Sticky because the `BatchSpanProcessor` throws away the result
/// of every batch it exports on its own schedule (it only propagates the one
/// forced by `force_flush`), so without this a batch that failed minutes
/// before exit would leave nothing at all for the shutdown path to report.
static EXPORT_FAILED: AtomicBool = AtomicBool::new(false);

/// Spans in batches the exporter accepted, and spans in batches it rejected,
/// since the live provider was installed.  Read out by
/// `nxt_otel_rs_export_stats` for the `/status` API, which is the only way an
/// operator can see export health without waiting for the process to exit.
///
/// Counted in spans rather than batches because the number an operator needs
/// is "how much telemetry did I lose", and a batch is a variable-size unit
/// that answers that only by accident.  Neither counter includes spans the
/// processor dropped before an export was attempted (a full queue): that
/// happens inside the SDK, which reports it nowhere we can observe.
static SPANS_EXPORTED: AtomicU64 = AtomicU64::new(0);
static SPANS_FAILED: AtomicU64 = AtomicU64::new(0);

/// Wraps the configured OTLP exporter so a failed export is remembered in
/// `EXPORT_FAILED` even when the processor discards the result.
///
/// Everything other than `export` is delegated verbatim to the inner
/// exporter: taking the trait's defaults here would silently turn shutdown,
/// force-flush and resource propagation into no-ops.
#[derive(Debug)]
struct FailureTrackingExporter<E> {
    inner: E,
}

impl<E: SdkSpanExporter> SdkSpanExporter for FailureTrackingExporter<E> {
    async fn export(&self, batch: Vec<SpanData>) -> OTelSdkResult {
        // Count before the batch is moved into the inner exporter.
        let spans = batch.len() as u64;

        let res = self.inner.export(batch).await;

        if res.is_err() {
            // The boolean is kept alongside the counter rather than derived
            // from it: it is the fact the shutdown path asks for ("has any
            // export failed"), and it stays true even for the degenerate
            // empty batch that would add nothing to SPANS_FAILED.
            EXPORT_FAILED.store(true, Ordering::Release);
            SPANS_FAILED.fetch_add(spans, Ordering::Relaxed);
        } else {
            SPANS_EXPORTED.fetch_add(spans, Ordering::Relaxed);
        }

        res
    }

    fn shutdown_with_timeout(&self, timeout: Duration) -> OTelSdkResult {
        self.inner.shutdown_with_timeout(timeout)
    }

    fn shutdown(&self) -> OTelSdkResult {
        self.inner.shutdown()
    }

    fn force_flush(&self) -> OTelSdkResult {
        self.inner.force_flush()
    }

    fn set_resource(&mut self, resource: &Resource) {
        self.inner.set_resource(resource);
    }
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

/// Report span export health for the `/status` API.
///
/// Writes the number of spans the exporter accepted and the number it
/// rejected since the live provider was installed, and returns 1 when a
/// provider is installed at all.
///
/// The counters are written whether or not a provider is installed, so on a
/// 0 return they hold whatever the previous provider left behind -- they are
/// only zeroed when a new one is installed.  A 0 return therefore means the
/// values are stale, not absent: the caller must ignore them rather than
/// report them.
///
/// The counters are read with two independent `Relaxed` loads, so a report
/// taken while an export is completing can be off by one batch.  That is
/// deliberate: this is a health gauge, not an accounting ledger, and taking a
/// lock on the export path to make the pair atomic would cost more than the
/// skew is worth.
///
/// # Safety
///
/// `exported` and `failed` must each be either null or a valid, aligned,
/// writable `uint64_t`.
#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_export_stats(
    exported: *mut u64,
    failed: *mut u64,
) -> u8 {
    if !exported.is_null() {
        *exported = SPANS_EXPORTED.load(Ordering::Relaxed);
    }

    if !failed.is_null() {
        *failed = SPANS_FAILED.load(Ordering::Relaxed);
    }

    nxt_otel_rs_is_init()
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

    // A new provider starts with a clean slate, so the shutdown path does not
    // normally report a failure that belonged to the exporter we replaced:
    // nxt_otel_rs_shutdown_tracer() above joins the old worker thread before
    // this runs.  It joins on a 5s budget though, and an export gets 10s, so
    // a worker still blocked on a dead collector can outlive the join and set
    // the flag after this store.  The cost is one spurious FAILED at the next
    // exit, describing a failure that was real but belonged to the old
    // exporter -- not worth a second flag to suppress.
    EXPORT_FAILED.store(false, Ordering::Release);
    SPANS_EXPORTED.store(0, Ordering::Relaxed);
    SPANS_FAILED.store(0, Ordering::Relaxed);

    let processor = BatchSpanProcessor::builder(FailureTrackingExporter { inner: exporter })
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

    if let Ok(mut slot) = tracer_slot().write() {
        *slot = Some(global::tracer_provider().tracer(TRACER_NAME));
    }

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

/// Set a semantic-convention span attribute (e.g. `http.request.method`).
/// These are structured span attributes the collector can index and query.
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

/// Whether the sampler kept this span.
///
/// C asks once, right after the span is built, and skips the attribute work
/// for a span that is not recording. `set_attribute` on such a span is
/// already a no-op, but its arguments are built before the call that throws
/// them away -- so without this gate, lowering `sampling_ratio` buys back the
/// exporter and nothing on the request path.
#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_is_recording(trace: *mut BoxedSpan) -> u8 {
    if trace.is_null() {
        return 0;
    }

    (*trace).is_recording() as u8
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

fn nxt_otel_build_span(tracer: &BoxedTracer, parent: &Context) -> BoxedSpan {
    let builder = tracer.span_builder(SPAN_NAME).with_kind(SpanKind::Server);

    tracer.build_with_context(builder, parent)
}

#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_get_or_create_trace(
    trace_id: *const c_char,
    parent_id: *const c_char,
    trace_flags: *const c_char,
    trace_state: *const nxt_str_t,
) -> *mut BoxedSpan {
    let parent = nxt_otel_parent_context(trace_id, parent_id, trace_flags, trace_state)
        .unwrap_or_else(Context::new);

    // The read guard is held across the build so the cached tracer cannot be
    // dropped by a concurrent reconfigure while a span is being made from it.
    let cached = tracer_slot().read().ok();

    let span = match cached.as_ref().and_then(|slot| slot.as_ref()) {
        Some(tracer) => nxt_otel_build_span(tracer, &parent),
        None => {
            // No provider installed, or the lock is poisoned: fall back to the
            // global provider, which hands out a no-op tracer in that case.
            let tracer = global::tracer_provider().tracer(TRACER_NAME);

            nxt_otel_build_span(&tracer, &parent)
        }
    };

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

/// Flush and tear down the live tracer provider with a caller-supplied bound,
/// for use on the process exit path.
///
/// `nxt_otel_rs_shutdown_tracer` can block for as long as the exporter's own
/// `EXPORT_TIMEOUT` (10s) when the collector is unreachable, which is far too
/// long to sit in front of `exit()`. The provider and runtime are taken out of
/// their slots here and *moved* into a helper thread, so the statics are left
/// empty and unlocked whatever happens; we then wait up to `timeout_ms` for
/// that thread to finish flushing. On timeout we give up and return: the
/// helper thread is left running and dies with the process. Best effort by
/// construction — spans that could not be flushed in the budget are lost,
/// exactly as they are today, but a dead collector can no longer delay exit.
///
/// Returns one of three statuses, mirrored in `src/nxt_otel.h`:
///
/// * `NXT_OTEL_SHUTDOWN_FLUSHED` (1) — the flush completed within the budget
///   and no export has failed since this provider was installed.
/// * `NXT_OTEL_SHUTDOWN_FAILED` (2) — an export failed. Either the flush this
///   call forced failed, or `EXPORT_FAILED` records an earlier batch that did:
///   the processor exports on its own schedule and discards the result, so the
///   only way that failure is visible at all is the sticky flag. The helper
///   thread failing to spawn reports here too; it is a failure, not a timeout.
/// * `NXT_OTEL_SHUTDOWN_TIMEOUT` (0) — the helper thread did not answer inside
///   `timeout_ms`. Kept distinct from the above because a rejected export
///   fails in milliseconds, and calling that a timeout sends an operator
///   looking for latency that is not there.
///
/// One loss remains unreportable from here: the router's worker engines are
/// still live when `nxt_runtime_exit` calls this, so a span they end after the
/// flush reaches a provider that is already shut down and is dropped without a
/// word. Nothing at this layer can see it — the engine threads are never
/// joined (`nxt_thread_join` has no call sites), so producers cannot be
/// quiesced first. That needs the P5 graceful-shutdown work and is tracked in
/// issue #219. A `FLUSHED` return therefore means "this flush succeeded and no
/// export has failed", not "no spans were lost".
#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_shutdown_bounded(timeout_ms: u64) -> u8 {
    // This path takes the provider without going through
    // nxt_otel_rs_shutdown_tracer(), so the cached tracer has to be dropped
    // here too: leaving it would hand out a tracer whose provider has
    // already been shut down.
    if let Ok(mut slot) = tracer_slot().write() {
        *slot = None;
    }

    let provider = provider_slot().lock().ok().and_then(|mut g| g.take());
    let rt = runtime_slot().lock().ok().and_then(|mut g| g.take());

    if provider.is_none() && rt.is_none() {
        return if EXPORT_FAILED.load(Ordering::Acquire) {
            NXT_OTEL_SHUTDOWN_FAILED
        } else {
            NXT_OTEL_SHUTDOWN_FLUSHED
        };
    }

    let (tx, rx) = mpsc::channel::<bool>();

    let spawned = std::thread::Builder::new()
        .name("otel-shutdown".to_string())
        .spawn(move || {
            let mut flushed = true;

            if let Some(provider) = provider {
                // Needs the runtime alive on a grpc build, hence the ordering.
                //
                // force_flush() first, because it is the only one of the two
                // that reports whether the spans actually left: the batch
                // processor hands shutdown() the export result and shutdown()
                // throws it away, reporting only that the worker answered.  A
                // collector that refuses the connection fails in milliseconds,
                // so shutdown() alone would call that a clean flush and exit
                // without a word.  Both run whatever the first one says.
                let exported = provider.force_flush().is_ok();
                let stopped = provider.shutdown().is_ok();

                flushed = exported && stopped;
            }
            if let Some(rt) = rt {
                rt.shutdown_background();
            }
            // Read the sticky flag only after the flush above, so a batch the
            // flush itself pushed out and failed on is counted here too.
            let _ = tx.send(flushed && !EXPORT_FAILED.load(Ordering::Acquire));
        });

    // The caller logs these, so they must not be conflated: a collector that
    // rejects the export fails in milliseconds, and reporting that as a
    // timeout sends an operator looking for latency that is not there.
    match spawned {
        Ok(_) => match rx.recv_timeout(Duration::from_millis(timeout_ms)) {
            Ok(true) => NXT_OTEL_SHUTDOWN_FLUSHED,
            Ok(false) => NXT_OTEL_SHUTDOWN_FAILED,
            Err(_) => NXT_OTEL_SHUTDOWN_TIMEOUT,
        },
        // Nothing flushed at all, and no time was spent trying.
        Err(_) => NXT_OTEL_SHUTDOWN_FAILED,
    }
}

/// Flush and tear down the live tracer provider, if any.
#[no_mangle]
pub unsafe extern "C" fn nxt_otel_rs_shutdown_tracer() {
    // Dropped first: a tracer handed out after this point would belong to a
    // provider that is already being torn down.
    if let Ok(mut slot) = tracer_slot().write() {
        *slot = None;
    }

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
