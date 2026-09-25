use anyhow::{bail, Context, Result};
use bytes::{Bytes, BytesMut};
use http_body_util::combinators::BoxBody;
use http_body_util::{BodyExt, Full};
use hyper::Error;
use std::borrow::Cow;
use std::ffi::{CStr, CString};
use std::mem::MaybeUninit;
use std::os::raw::c_int;
use std::process::exit;
use std::ptr;
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::time::Duration;
use tokio::sync::mpsc;
use wasmtime::component::{Component, Linker, ResourceTable};
use wasmtime::{Config, Engine, Store};
use wasmtime_wasi::p2::add_to_linker_async;
use wasmtime_wasi::{DirPerms, FilePerms, WasiCtx, WasiCtxBuilder,
                    WasiCtxView, WasiView};
use wasmtime_wasi_http::p2::bindings::http::types::{ErrorCode, Scheme};
use wasmtime_wasi_http::p2::bindings::ProxyPre;
use wasmtime_wasi_http::p2::body::HyperOutgoingBody;
use wasmtime_wasi_http::p2::types::{HostFutureIncomingResponse,
                                    OutgoingRequestConfig};
use wasmtime_wasi_http::p2::{HttpResult, WasiHttpCtxView, WasiHttpHooks,
                             WasiHttpView};
use wasmtime_wasi_http::WasiHttpCtx;

#[allow(
    non_camel_case_types,
    non_upper_case_globals,
    non_snake_case,
    dead_code,
    unknown_lints,
    unnecessary_transmutes
)]
mod bindings {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

    pub const fn nxt_string(s: &'static str) -> nxt_str_t {
        nxt_str_t {
            start: s.as_ptr().cast_mut(),
            length: s.len(),
        }
    }

    pub unsafe fn nxt_unit_sptr_get(sptr: &nxt_unit_sptr_t) -> *const u8 {
        sptr.base.as_ptr().offset(sptr.offset as isize)
    }
}

#[no_mangle]
pub static mut nxt_app_module: bindings::nxt_app_module_t = {
    const COMPAT: [u32; 2] = [bindings::NXT_VERNUM, bindings::NXT_DEBUG];
    let version = "0.1\0";
    bindings::nxt_app_module_t {
        compat: COMPAT.as_ptr().cast_mut(),
        compat_length: COMPAT.len() * 4,
        mounts: ptr::null(),
        nmounts: 0,
        type_: bindings::nxt_string("wasm-wasi-component"),
        version: version.as_ptr().cast(),
        setup: Some(setup),
        start: Some(start),
    }
};

static GLOBAL_CONFIG: OnceLock<GlobalConfig> = OnceLock::new();
static GLOBAL_STATE: OnceLock<GlobalState> = OnceLock::new();

unsafe extern "C" fn setup(
    task: *mut bindings::nxt_task_t,
    // TODO: should this get used?
    _process: *mut bindings::nxt_process_t,
    conf: *mut bindings::nxt_common_app_conf_t,
) -> bindings::nxt_int_t {
    handle_result(task, || {
        let wasm_conf = &(*conf).u.wasm_wc;
        let component = CStr::from_ptr(wasm_conf.component).to_str()?;
        let mut dirs = Vec::new();
        if !wasm_conf.access.is_null() {
            let dirs_ptr = bindings::nxt_conf_get_object_member(
                wasm_conf.access,
                &mut bindings::nxt_string("filesystem"),
                ptr::null_mut(),
            );
            for i in 0..bindings::nxt_conf_object_members_count(dirs_ptr) {
                let value = bindings::nxt_conf_get_array_element(
                    dirs_ptr,
                    i.try_into().unwrap(),
                );
                let mut s = bindings::nxt_string("");
                bindings::nxt_conf_get_string(value, &mut s);
                dirs.push(
                    std::str::from_utf8(std::slice::from_raw_parts(
                        s.start, s.length,
                    ))?
                    .to_string(),
                );
            }
        }

        let result = GLOBAL_CONFIG.set(GlobalConfig {
            component: component.to_string(),
            dirs,
            execution_timeout: wasm_conf.execution_timeout.into(),
        });
        assert!(result.is_ok());
        Ok(())
    })
}

unsafe extern "C" fn start(
    task: *mut bindings::nxt_task_t,
    data: *mut bindings::nxt_process_data_t,
) -> bindings::nxt_int_t {
    let mut rc: i32 = 0;

    let result = handle_result(task, || {
        let config = GLOBAL_CONFIG.get().unwrap();
        let state = GlobalState::new(&config)
            .context("failed to create initial state")?;
        let res = GLOBAL_STATE.set(state);
        assert!(res.is_ok());

        let conf = (*data).app;
        let mut wasm_init = MaybeUninit::uninit();
        let ret =
            bindings::nxt_unit_default_init(task, wasm_init.as_mut_ptr(), conf);
        if ret != bindings::NXT_OK as bindings::nxt_int_t {
            bail!("nxt_unit_default_init() failed");
        }
        let mut wasm_init = wasm_init.assume_init();
        wasm_init.callbacks.request_handler = Some(request_handler);

        let unit_ctx = bindings::nxt_unit_init(&mut wasm_init);
        if unit_ctx.is_null() {
            bail!("nxt_unit_init() failed");
        }

        rc = bindings::nxt_unit_run(unit_ctx);
        bindings::nxt_unit_done(unit_ctx);

        Ok(())
    });

    if result != bindings::NXT_OK as bindings::nxt_int_t {
        return result;
    }

    exit(rc);
}

unsafe fn handle_result(
    task: *mut bindings::nxt_task_t,
    func: impl FnOnce() -> Result<()>,
) -> bindings::nxt_int_t {
    let rc = match func() {
        Ok(()) => bindings::NXT_OK as bindings::nxt_int_t,
        Err(e) => {
            alert(task, &format!("{e:?}"));
            bindings::NXT_ERROR as bindings::nxt_int_t
        }
    };
    return rc;

    unsafe fn alert(task: *mut bindings::nxt_task_t, msg: &str) {
        let log = (*task).log;
        let msg = CString::new(msg).unwrap();
        ((*log).handler).unwrap()(
            bindings::NXT_LOG_ALERT as bindings::nxt_uint_t,
            log,
            "%s\0".as_ptr().cast(),
            msg.as_ptr(),
        );
    }
}

unsafe extern "C" fn request_handler(
    info: *mut bindings::nxt_unit_request_info_t,
) {
    // Enqueue this request to get processed by the Tokio event loop, and
    // otherwise immediately return.
    let state = GLOBAL_STATE.get().unwrap();
    state
        .sender
        .blocking_send(NxtRequestInfo {
            info,
            response_started: false,
        })
        .unwrap();
}

struct GlobalConfig {
    component: String,
    dirs: Vec<String>,
    /// "execution_timeout" in milliseconds, 0 for unbounded.
    execution_timeout: u64,
}

struct GlobalState {
    engine: Engine,
    component: ProxyPre<StoreState>,
    global_config: &'static GlobalConfig,
    sender: mpsc::Sender<NxtRequestInfo>,
    /// `None` unless "execution_timeout" is configured, in which case the
    /// engine has epoch interruption on and every store needs a deadline.
    epoch: Option<Epoch>,
}

/// What a store needs to be armed: who advances the epoch, and how far
/// ahead the deadline sits.
///
/// The two travel together because neither is correct alone.  With
/// `epoch_interruption(true)` a store that is never given a deadline traps
/// on its first instruction, and a deadline nothing advances never fires,
/// so a single `Option` is what keeps a later `Store::new` from getting
/// either.
struct Epoch {
    ticker: Arc<EpochTicker>,
    /// The deadline, in epoch ticks.  See `epoch_deadline_ticks()`.
    deadline: u64,
}

impl Epoch {
    /// Arms `store` and keeps the ticker awake until the guard is dropped.
    fn arm(&self, store: &mut Store<StoreState>) -> Ticking {
        store.set_epoch_deadline(self.deadline);

        self.ticker.ticking()
    }
}

impl GlobalState {
    fn new(global_config: &'static GlobalConfig) -> Result<GlobalState> {
        // Configure Wasmtime, e.g. the component model is enabled here.
        // (Async support is unconditional as of wasmtime 47.) Other
        // configuration can include:
        //
        // * Epochs/fuel - enables async yielding to prevent any one request
        //   starving others.
        // * Pooling allocator - accelerates instantiation at the cost of a
        //   large virtual memory reservation.
        // * Memory limits/etc.
        let mut config = Config::new();
        config.wasm_component_model(true);

        // A guest yields only at an async wasi import, so one that computes
        // without calling any stays inside a single `call_handle()` poll for
        // as long as it likes: `JoinHandle::abort()` is never observed, the
        // store is held, and a runtime worker thread is pinned.  Epoch
        // interruption is what lets the host stop it.  Fuel would too, but
        // it counts every instruction to be deterministic, where this only
        // reads a counter at function entries and loop headers and costs
        // 2-3x less (wasmtime Config::epoch_interruption docs); a deadline
        // does not need to be reproducible, only enforced.
        //
        // Off unless asked for: it changes the code generated for every
        // guest, and a component that legitimately computes for a long time
        // must keep working.
        let bounded = global_config.execution_timeout != 0;

        if bounded {
            config.epoch_interruption(true);
        }

        let engine = Engine::new(&config)?;

        // One `bounded` for both: with epoch interruption on, a store that
        // is never armed traps at once, so the engine setting and the
        // `Epoch` that arms every store cannot be allowed to disagree.
        let epoch = bounded.then(|| Epoch {
            ticker: EpochTicker::start(engine.clone()),
            deadline: epoch_deadline_ticks(global_config.execution_timeout),
        });

        // Compile the binary component on disk in Wasmtime. This is then
        // pre-instantiated with host APIs defined by WASI. The result of
        // this is a "pre-instantiated instance" which can be used to
        // repeatedly instantiate later on. This will frontload
        // compilation/linking/type-checking/etc to happen once rather than on
        // each request.
        // NB: wasmtime 47 returns its own `wasmtime::Error` rather than an
        // `anyhow::Error`, so `anyhow::Context` does not apply here. Use
        // `wasmtime::Error`'s inherent `context()` and let `?` convert the
        // result into the `anyhow::Error` these functions return.
        let component = Component::from_file(&engine, &global_config.component)
            .map_err(|e| e.context("failed to compile component"))?;
        let mut linker = Linker::<StoreState>::new(&engine);
        add_to_linker_async(&mut linker)
            .map_err(|e| e.context("failed to add wasi to linker"))?;
        wasmtime_wasi_http::p2::add_only_http_to_linker_sync(&mut linker)
            .map_err(|e| e.context("failed to add wasi:http to linker"))?;
        let component = linker.instantiate_pre(&component).map_err(|e| {
            e.context("failed to pre-instantiate the provided component")
        })?;
        let proxy = ProxyPre::new(component)
            .map_err(|e| e.context("failed to conform to proxy"))?;

        // Spin up the Tokio async runtime in a separate thread with a
        // communication channel into it. This thread will send requests to
        // Tokio and the results will be calculated there.
        let (sender, receiver) = mpsc::channel(10);
        std::thread::spawn(|| GlobalState::run(receiver));

        Ok(GlobalState {
            engine,
            component: proxy,
            sender,
            global_config,
            epoch,
        })
    }

    /// Worker thread that executes the Tokio runtime, infinitely receiving
    /// messages from the provided `receiver` and handling those requests.
    ///
    /// Each request is handled in a separate subtask so processing can all
    /// happen concurrently.
    fn run(mut receiver: mpsc::Receiver<NxtRequestInfo>) {
        let rt = tokio::runtime::Runtime::new().unwrap();
        rt.block_on(async {
            while let Some(msg) = receiver.recv().await {
                let state = GLOBAL_STATE.get().unwrap();
                tokio::task::spawn(async move { state.handle(msg).await });
            }
        });
    }

    /// Handle one request, and finish it whatever happens.
    ///
    /// Every error below used to reach `.expect()` in `run()`, which under
    /// `panic = 'abort'` killed the worker and every other request on it.
    async fn handle(&'static self, mut info: NxtRequestInfo) {
        match self.handle_inner(&mut info).await {
            Ok(()) => info.request_done(),
            Err(e) => info.fail(&e),
        }
    }

    async fn handle_inner(
        &'static self,
        info: &mut NxtRequestInfo,
    ) -> Result<()> {
        // Create a "Store" which is the unit of per-request isolation in
        // Wasmtime.
        let data = StoreState {
            ctx: {
                let mut cx = WasiCtxBuilder::new();
                // NB: while useful for debugging untrusted code probably
                // shouldn't get raw access to stdout/stderr.
                cx.inherit_stdout();
                cx.inherit_stderr();
                cx.inherit_env();
                for dir in self.global_config.dirs.iter() {
                    cx.preopened_dir(
                        dir,
                        dir,
                        DirPerms::all(),
                        FilePerms::all(),
                    )?;
                }
                cx.build()
            },
            table: ResourceTable::default(),
            http: WasiHttpCtx::new(),
            hooks: DenyOutboundHttp,
        };
        let mut store = Store::new(&self.engine, data);

        // Convert the `nxt_*` representation into the representation required
        // by Wasmtime's `wasi-http` implementation using the Rust `http`
        // crate.
        // A request that the `http` crate refuses to represent must fail on
        // its own, not take the worker with it: `run()` turns an `Err` from
        // here into `.expect()`, and both profiles set `panic = 'abort'`.
        // Unit forwards bytes that `http` rejects -- a `Host` holding
        // obs-text, a target holding `<`, `>` or a control byte -- so this is
        // reachable from an unauthenticated request.
        let builder = self.to_request_builder(info).context(BadRequest)?;

        // Not BadRequest: a negative read comes from read() on the spooled
        // body failing (src/nxt_unit.c), which is our I/O, not the client's
        // doing, so it answers 500.
        let body = self.to_request_body(info)?;
        let request = builder
            .body(body)
            .map_err(anyhow::Error::from)
            .context(BadRequest)?;

        let (sender, receiver) = tokio::sync::oneshot::channel();

        // Instantiate the WebAssembly component and invoke its `handle`
        // function which receives a request and where to put a response.
        //
        // Note that this is done in a sub-task to work concurrently with
        // writing the response when it's available. This enables wasm to
        // generate headers, write those below, and then compute the body
        // afterwards.
        let mut task = GuestTask(Some(tokio::spawn(async move {
            // Arm the deadline here rather than beside `Store::new`, and
            // hold the ticker awake for as long as the guest can run.  The
            // budget is spent by the epoch, which advances in wall clock
            // while any request is in flight, so everything before this
            // point would spend it without the guest running: the wait for
            // a runtime worker -- under load, behind the very guests this
            // bounds -- and the blocking `request_read()` inside
            // `to_request_body()`.  A task that is dropped before it is
            // ever polled now also never wakes the ticker.
            let _ticking = self.epoch.as_ref().map(|e| e.arm(&mut store));

            let req = store
                .data_mut()
                .http()
                .new_incoming_request(Scheme::Http, request)?;
            let out = store.data_mut().http().new_response_outparam(sender)?;
            self.component
                .instantiate_async(&mut store)
                .await?
                .wasi_http_incoming_handler()
                .call_handle(&mut store, req, out)
                .await
                .map_err(|e| e.context("failed to invoke wasm `handle`"))?;
            Ok::<_, anyhow::Error>(())
        })));

        // Wait for the wasm to produce the initial response. If this succeeds
        // then propagate that failure. If this fails then wait for the above
        // task to complete to see if it failed, otherwise panic since that's
        // unexpected.
        let response = match receiver.await {
            Ok(response) => response.context("response generation failed")?,
            Err(_) => {
                task.join().await?;
                bail!("the sender of the response disappeared");
            }
        };

        // Send the headers/status which will extract the body for the next
        // phase.
        let body = self.send_response(info, response)?;

        // Send the body, a blocking operation, over time as it becomes
        // available.
        self.send_response_body(info, body)
            .await
            .context("failed to write response body")?;

        // Join on completion of the wasm task which should be done by this
        // point.
        task.join().await?;

        Ok(())
    }

    fn to_request_builder(
        &self,
        info: &NxtRequestInfo,
    ) -> Result<http::request::Builder> {
        let mut request = http::Request::builder();

        request = request.method(info.method().as_ref());
        request = match &*info.version() {
            "HTTP/0.9" => request.version(http::Version::HTTP_09),
            "HTTP/1.0" => request.version(http::Version::HTTP_10),
            "HTTP/1.1" => request.version(http::Version::HTTP_11),
            "HTTP/2.0" => request.version(http::Version::HTTP_2),
            "HTTP/3.0" => request.version(http::Version::HTTP_3),
            version => {
                println!("unknown version: {version}");
                request
            }
        };

        let uri = http::Uri::builder()
            .scheme(if info.tls() { "https" } else { "http" })
            .authority(info.server_name().as_ref())
            .path_and_query(info.target().as_ref())
            .build()
            .context("failed to build URI")?;
        request = request.uri(uri);

        for (name, value) in info.fields() {
            request = request.header(name.as_ref(), value.as_ref());
        }
        Ok(request)
    }

    fn to_request_body(
        &self,
        info: &mut NxtRequestInfo,
    ) -> Result<BoxBody<Bytes, Error>> {
        // TODO: should convert the body into a form of `Stream` to become an
        // async stream of frames. The return value can represent that here
        // but for now this slurps up the entire body into memory and puts it
        // all in a single `BytesMut` which is then converted to `Bytes`.
        let mut body =
            BytesMut::with_capacity(info.content_length().try_into().unwrap());

        // TODO: can this perform a partial read?
        // TODO: how to make this async at the nxt level?
        info.request_read(&mut body)?;

        Ok(Full::new(body.freeze()).map_err(|e| match e {}).boxed())
    }

    fn send_response<T>(
        &self,
        info: &mut NxtRequestInfo,
        response: http::Response<T>,
    ) -> Result<T> {
        info.init_response(
            response.status().as_u16(),
            response.headers().len().try_into().unwrap(),
            response
                .headers()
                .iter()
                .map(|(k, v)| k.as_str().len() + v.len())
                .sum::<usize>()
                .try_into()
                .unwrap(),
        )?;
        for (k, v) in response.headers() {
            info.add_field(k.as_str().as_bytes(), v.as_bytes())?;
        }
        info.send_response()?;

        Ok(response.into_body())
    }

    async fn send_response_body(
        &self,
        info: &mut NxtRequestInfo,
        mut body: HyperOutgoingBody,
    ) -> Result<()> {
        loop {
            // Acquire the next frame, and because nothing is actually async
            // at the moment this should never block meaning that the
            // `Pending` case should not happen.
            let frame = match body.frame().await {
                Some(Ok(frame)) => frame,
                Some(Err(e)) => break Err(e.into()),
                None => break Ok(()),
            };
            match frame.data_ref() {
                Some(data) => {
                    info.response_write(&data)?;
                }
                None => {
                    // TODO: what to do with trailers?
                }
            }
        }
    }
}

/// How often the epoch advances while a guest is running.
const EPOCH_TICK: Duration = Duration::from_millis(100);

/// Turn a timeout in milliseconds into `Store::set_epoch_deadline()` ticks.
///
/// A store is armed at an arbitrary point between two ticks, so with `n`
/// ticks the guest is stopped somewhere in `[(n - 1) * TICK, n * TICK]`.
/// The extra tick is what keeps the lower end at or after the configured
/// timeout: a request is never cut short of it.
///
/// The far end is one tick past the timeout once the rounding up is a
/// no-op, which it always is here -- the config is whole seconds, so
/// `timeout_ms` is a multiple of `TICK` -- and two ticks otherwise.  Add
/// to that whatever the guest takes to reach its next epoch check, which
/// wasmtime puts at function entries and loop headers.
fn epoch_deadline_ticks(timeout_ms: u64) -> u64 {
    let tick = EPOCH_TICK.as_millis() as u64;

    timeout_ms.saturating_add(tick - 1) / tick + 1
}

/// Advances the engine's epoch so an armed deadline can fire.
///
/// The ticker is its own OS thread rather than a task on the module's tokio
/// runtime.  A guest that never yields occupies a runtime worker thread for
/// as long as it runs, so enough of them starve exactly the task that is
/// meant to preempt them -- on a one-core machine, the first one does.  A
/// thread outside the runtime cannot be starved by the code it bounds.
///
/// It also sleeps while nothing is running.  A worker with no request in
/// flight has nothing to preempt, and a server can hold many idle workers,
/// so the ticker waits on `wake` until a request arms a deadline.
struct EpochTicker {
    running: Mutex<usize>,
    wake: Condvar,
}

impl EpochTicker {
    fn start(engine: Engine) -> Arc<EpochTicker> {
        let ticker = Arc::new(EpochTicker {
            running: Mutex::new(0),
            wake: Condvar::new(),
        });

        let thread = ticker.clone();
        std::thread::spawn(move || thread.run(engine));

        ticker
    }

    fn run(&self, engine: Engine) {
        loop {
            {
                let mut running = self.lock();

                while *running == 0 {
                    running = match self.wake.wait(running) {
                        Ok(running) => running,
                        Err(e) => e.into_inner(),
                    };
                }
            }

            // Sleeping outside the lock means a request that finishes
            // meanwhile still gets this increment.  That is one wasted
            // atomic add, not a missed deadline.
            std::thread::sleep(EPOCH_TICK);
            engine.increment_epoch();
        }
    }

    /// Keeps the ticker awake until the returned guard is dropped.
    fn ticking(self: &Arc<Self>) -> Ticking {
        *self.lock() += 1;
        self.wake.notify_one();

        Ticking(self.clone())
    }

    /// `panic = 'abort'` means a lock is never poisoned, but taking the
    /// inner value rather than unwrapping keeps a second panic -- which
    /// would end the worker -- off the table if that ever changes.
    fn lock(&self) -> MutexGuard<'_, usize> {
        match self.running.lock() {
            Ok(running) => running,
            Err(e) => e.into_inner(),
        }
    }
}

/// Marks one request as running, so the ticker keeps the epoch moving.
struct Ticking(Arc<EpochTicker>);

impl Drop for Ticking {
    fn drop(&mut self) {
        *self.0.lock() -= 1;
    }
}

/// Owns the wasm task so that leaving `handle_inner()` early stops it.
/// Dropping a `JoinHandle` only detaches the task, it does not cancel it:
/// a client that disconnects mid-response fails the write, the request is
/// finished without us, and the wasm invocation would keep its `Store` and
/// burn CPU in the background.  `join()` takes the handle back out, so a
/// task that ran to completion is never aborted.
struct GuestTask(Option<tokio::task::JoinHandle<Result<()>>>);

impl GuestTask {
    /// Wait for the wasm task, reporting a cancelled or panicked one as an
    /// error.  `unwrap()` here would panic a second time, and under
    /// `panic = 'abort'` that ends the worker.  A missing handle cannot
    /// happen -- both callers return right after -- and is reported the
    /// same way for the same reason.
    async fn join(&mut self) -> Result<()> {
        match self.0.take() {
            Some(task) => match task.await {
                Ok(result) => result,
                Err(e) => bail!("the wasm task did not finish: {e}"),
            },
            None => bail!("the wasm task was already joined"),
        }
    }
}

impl Drop for GuestTask {
    fn drop(&mut self) {
        if let Some(task) = &self.0 {
            task.abort();
        }
    }
}

/// Marks an error caused by what the client sent, so it is answered 400
/// rather than 500.
#[derive(Debug)]
struct BadRequest;

impl std::fmt::Display for BadRequest {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("malformed request")
    }
}

impl std::error::Error for BadRequest {}

/// libunit reports failure as a non-zero return.  Turn that into an error
/// rather than a panic: under `panic = 'abort'` a panic here would take the
/// worker down and with it every other request it is serving.
fn check_rc(rc: c_int, what: &str) -> Result<()> {
    if rc != 0 {
        bail!("{what} failed with {rc}");
    }
    Ok(())
}

struct NxtRequestInfo {
    info: *mut bindings::nxt_unit_request_info_t,
    // Once the status line is out, a failure can no longer be answered with
    // a status -- all that is left is to end the request.
    response_started: bool,
}

// TODO: is this actually safe?
unsafe impl Send for NxtRequestInfo {}
unsafe impl Sync for NxtRequestInfo {}

impl NxtRequestInfo {
    fn method(&self) -> Cow<'_, str> {
        unsafe {
            let raw = (*self.info).request;
            self.get_str(&(*raw).method, (*raw).method_length.into())
        }
    }

    fn tls(&self) -> bool {
        unsafe { (*(*self.info).request).tls != 0 }
    }

    fn version(&self) -> Cow<'_, str> {
        unsafe {
            let raw = (*self.info).request;
            self.get_str(&(*raw).version, (*raw).version_length.into())
        }
    }

    fn server_name(&self) -> Cow<'_, str> {
        unsafe {
            let raw = (*self.info).request;
            self.get_str(&(*raw).server_name, (*raw).server_name_length.into())
        }
    }

    fn target(&self) -> Cow<'_, str> {
        unsafe {
            let raw = (*self.info).request;
            self.get_str(&(*raw).target, (*raw).target_length.into())
        }
    }

    fn content_length(&self) -> u64 {
        unsafe {
            let raw_request = (*self.info).request;
            (*raw_request).content_length
        }
    }

    fn fields(&self) -> impl Iterator<Item = (Cow<'_, str>, Cow<'_, str>)> {
        unsafe {
            let raw = (*self.info).request;
            (0..(*raw).fields_count).map(move |i| {
                let field = (*raw).fields.as_ptr().add(i as usize);
                let name =
                    self.get_str(&(*field).name, (*field).name_length.into());
                let value =
                    self.get_str(&(*field).value, (*field).value_length.into());
                (name, value)
            })
        }
    }

    fn request_read(&mut self, dst: &mut BytesMut) -> Result<()> {
        unsafe {
            let rest = dst.spare_capacity_mut();
            let mut total_bytes_read = 0;
            while total_bytes_read < rest.len() {
                let amt = bindings::nxt_unit_request_read(
                    self.info,
                    rest.as_mut_ptr().wrapping_add(total_bytes_read).cast(),
                    rest.len() - total_bytes_read,
                );

                // A read returns a signed count.  `amt as usize` on a
                // negative one wraps to a huge number, which ends the loop
                // and then moves `set_len()` past the allocation.
                if amt < 0 {
                    bail!("failed to read the request body");
                }

                // Nothing left to read.  Without this a steady 0 spins here
                // forever, because 0 never reaches `rest.len()`.
                if amt == 0 {
                    break;
                }

                // libunit should never return more than it was asked for,
                // but this count decides `set_len()` below, so do not take
                // its word for it: a wrong one here is a buffer overrun,
                // not a wrong answer.
                let amt = amt as usize;
                if amt > rest.len() - total_bytes_read {
                    bail!("the request body read returned more than it was given room for");
                }

                total_bytes_read += amt;
            }
            dst.set_len(dst.len() + total_bytes_read);
        }
        Ok(())
    }

    fn response_write(&mut self, data: &[u8]) -> Result<()> {
        unsafe {
            let rc = bindings::nxt_unit_response_write(
                self.info,
                data.as_ptr().cast(),
                data.len(),
            );
            check_rc(rc, "nxt_unit_response_write")?;
        }
        Ok(())
    }

    fn init_response(
        &mut self,
        status: u16,
        headers: u32,
        headers_size: u32,
    ) -> Result<()> {
        unsafe {
            let rc = bindings::nxt_unit_response_init(
                self.info,
                status,
                headers,
                headers_size,
            );
            check_rc(rc, "nxt_unit_response_init")?;
        }
        Ok(())
    }

    fn add_field(&mut self, key: &[u8], val: &[u8]) -> Result<()> {
        // libunit takes the name length as a u8.  A guest is free to emit a
        // longer one, and `unwrap()` here would abort the worker over a
        // header -- the failure this whole path exists to stop.
        let Ok(key_len) = key.len().try_into() else {
            bail!("a response header name is longer than 255 bytes");
        };
        let Ok(val_len) = val.len().try_into() else {
            bail!("a response header value is too long");
        };

        unsafe {
            let rc = bindings::nxt_unit_response_add_field(
                self.info,
                key.as_ptr().cast(),
                key_len,
                val.as_ptr().cast(),
                val_len,
            );
            check_rc(rc, "nxt_unit_response_add_field")?;
        }
        Ok(())
    }

    fn send_response(&mut self) -> Result<()> {
        unsafe {
            let rc = bindings::nxt_unit_response_send(self.info);
            check_rc(rc, "nxt_unit_response_send")?;
        }
        // Only now has a status line reached the router.  `response_init`
        // alone fills a local buffer, and libunit permits a second one, so
        // a failure before this point can still be answered.
        self.response_started = true;
        Ok(())
    }

    /// Fail this one request, leaving the worker alive.
    ///
    /// Answers 500 if nothing has gone out yet.  Once the status line is
    /// sent there is no status left to send, so all this can do is end the
    /// request and let the client see a truncated response.
    fn fail(mut self, err: &anyhow::Error) {
        let bad = err.downcast_ref::<BadRequest>().is_some();
        let status = if bad { 400 } else { 500 };

        self.log_err(&format!("failed to handle a request: {err:#}"));

        if !self.response_started
            && self.init_response(status, 0, 0).is_ok()
            && self.send_response().is_ok()
        {
            self.request_done();
            return;
        }

        self.request_failed();
    }

    fn log_err(&self, msg: &str) {
        unsafe {
            if let Ok(msg) = CString::new(msg) {
                bindings::nxt_unit_req_log(
                    self.info,
                    bindings::NXT_UNIT_LOG_ERR as i32,
                    "%s\0".as_ptr().cast(),
                    msg.as_ptr(),
                );
            }
        }
    }

    fn request_failed(self) {
        unsafe {
            bindings::nxt_unit_request_done(
                self.info,
                bindings::NXT_UNIT_ERROR as i32,
            );
        }
    }

    fn request_done(self) {
        unsafe {
            bindings::nxt_unit_request_done(
                self.info,
                bindings::NXT_UNIT_OK as i32,
            );
        }
    }

    unsafe fn get_str(
        &self,
        ptr: &bindings::nxt_unit_sptr_t,
        len: u32,
    ) -> Cow<'_, str> {
        let ptr = bindings::nxt_unit_sptr_get(ptr);
        let slice = std::slice::from_raw_parts(ptr, len.try_into().unwrap());
        String::from_utf8_lossy(slice)
    }
}

struct StoreState {
    ctx: WasiCtx,
    http: WasiHttpCtx,
    table: ResourceTable,
    hooks: DenyOutboundHttp,
}

/// Guest-initiated outbound HTTP is deliberately not supported.
///
/// `send_request` normally comes from the "default-send-request" feature of
/// wasmtime-wasi-http, which pulls in the whole rustls TLS stack (rustls,
/// tokio-rustls, webpki-roots, rustls-webpki) purely so that a guest can dial
/// out.  That subtree is a recurring source of advisories and Unit itself
/// never needs it, so Cargo.toml builds the crate without the feature.  With
/// it off the trait method has no default body, which is what makes this
/// denial mandatory rather than merely conventional: restoring the feature
/// without noticing cannot happen silently, because dropping this impl stops
/// compiling.
///
/// `wasi:http/outgoing-handler` is still linked, so components that merely
/// import it -- as the `wasi:http/proxy` world requires -- continue to
/// instantiate and run.  Only an actual outbound call fails, and it fails with
/// a well-defined error rather than a trap or a hang.
#[derive(Default)]
struct DenyOutboundHttp;

impl WasiHttpHooks for DenyOutboundHttp {
    fn send_request(
        &mut self,
        _request: hyper::Request<HyperOutgoingBody>,
        _config: OutgoingRequestConfig,
    ) -> HttpResult<HostFutureIncomingResponse> {
        Err(ErrorCode::HttpRequestDenied.into())
    }
}

impl WasiView for StoreState {
    fn ctx(&mut self) -> WasiCtxView<'_> {
        WasiCtxView {
            ctx: &mut self.ctx,
            table: &mut self.table,
        }
    }
}

impl WasiHttpView for StoreState {
    fn http(&mut self) -> WasiHttpCtxView<'_> {
        WasiHttpCtxView {
            ctx: &mut self.http,
            table: &mut self.table,
            hooks: &mut self.hooks,
        }
    }
}

impl StoreState {}
