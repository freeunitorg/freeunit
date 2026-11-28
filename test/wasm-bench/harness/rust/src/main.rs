// SPDX-License-Identifier: Apache-2.0

//! Time how the "wasm-wasi-component" language module gets a component
//! ready, in three modes.
//!
//! "jit" replays `GlobalState::new()` in src/wasm-wasi-component/src/lib.rs:
//! `Config::new()` with the component model on (and epoch interruption when
//! `--epoch` stands for an `execution_timeout`), `Engine::new()`,
//! `Component::from_file()` (Cranelift compiles every function), then a
//! `Linker` with WASI p2 and wasi:http, `instantiate_pre()` and
//! `ProxyPre::new()`.  The other modes change only how the component is
//! obtained:
//!
//!   jit         Component::from_file()
//!   cache       Config::cache(Cache::from_file(TOML or default)), then jit;
//!               needs `--features cache`
//!   precompile  Engine::precompile_component() and write OUT.cwasm
//!   aot         unsafe Component::deserialize_file(OUT.cwasm)
//!
//! Usage: wasm-bench-load MODE COMPONENT.wasm [--epoch] [--cwasm OUT]
//!        [--cache-config TOML]
//! Prints one line of key=value timings in milliseconds.

use anyhow::{bail, Result};
use std::time::Instant;
use wasmtime::component::{Component, Linker, ResourceTable};
use wasmtime::{Config, Engine};
use wasmtime_wasi::p2::add_to_linker_async;
use wasmtime_wasi::{WasiCtx, WasiCtxView, WasiView};
use wasmtime_wasi_http::p2::bindings::ProxyPre;
use wasmtime_wasi_http::{
    Error as HttpError, RequestOptions, WasiBody, WasiHttpCtx, WasiHttpCtxView,
    WasiHttpHooks, WasiHttpView,
};

struct StoreState {
    ctx: WasiCtx,
    http: WasiHttpCtx,
    table: ResourceTable,
    hooks: DenyOutboundHttp,
}

#[derive(Default)]
struct DenyOutboundHttp;

impl WasiHttpHooks for DenyOutboundHttp {
    fn send_request(
        &mut self,
        _request: hyper::Request<WasiBody>,
        _options: Option<RequestOptions>,
        _fut: Box<
            dyn std::future::Future<Output = Result<(), HttpError>> + Send,
        >,
    ) -> Box<
        dyn std::future::Future<
                Output = Result<
                    (
                        hyper::Response<WasiBody>,
                        Box<
                            dyn std::future::Future<
                                    Output = Result<(), HttpError>,
                                > + Send,
                        >,
                    ),
                    HttpError,
                >,
            > + Send,
    > {
        Box::new(async { Err(HttpError::HttpRequestDenied) })
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

fn ms(a: Instant, b: Instant) -> f64 {
    (b - a).as_secs_f64() * 1e3
}

fn main() -> Result<()> {
    let t0 = Instant::now();
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        bail!(
            "usage: {} jit|cache|precompile|aot COMPONENT.wasm [--epoch] \
               [--cwasm OUT] [--cache-config TOML]",
            args[0]
        );
    }
    let mode = args[1].as_str();
    let path = &args[2];
    let mut epoch = false;
    let mut cwasm = None;
    let mut cache_toml: Option<String> = None;
    let mut i = 3;
    while i < args.len() {
        match args[i].as_str() {
            "--epoch" => epoch = true,
            "--cwasm" => {
                i += 1;
                cwasm = Some(args[i].clone())
            }
            "--cache-config" => {
                i += 1;
                cache_toml = Some(args[i].clone())
            }
            a => bail!("unknown argument {a}"),
        }
        i += 1;
    }

    let mut config = Config::new();
    config.wasm_component_model(true);
    if epoch {
        config.epoch_interruption(true);
    }
    if mode == "cache" {
        #[cfg(feature = "cache")]
        {
            let cache = wasmtime::Cache::from_file(
                cache_toml.as_deref().map(std::path::Path::new),
            )?;
            config.cache(Some(cache));
        }
        #[cfg(not(feature = "cache"))]
        {
            let _ = &cache_toml;
            bail!("built without --features cache");
        }
    }
    let engine = Engine::new(&config)?;
    let t_engine = Instant::now();

    // Component::from_file() reads the whole file, then compiles; time the
    // read on its own so it can be told apart from Cranelift.
    let bytes = std::fs::read(path)?;
    let t_read = Instant::now();
    drop(bytes);

    let t_load_start = Instant::now();
    let component = match mode {
        "aot" => {
            let p = cwasm.as_ref().expect("--cwasm");
            // Trusted input only: a .cwasm is native code.
            unsafe { Component::deserialize_file(&engine, p)? }
        }
        _ => Component::from_file(&engine, path)?,
    };
    let t_load = Instant::now();

    let mut t_ser = 0.0;
    if mode == "precompile" {
        let s = Instant::now();
        let wasm = std::fs::read(path)?;
        let art = engine.precompile_component(&wasm)?;
        std::fs::write(cwasm.as_ref().expect("--cwasm"), art)?;
        t_ser = ms(s, Instant::now());
    }

    let t_link0 = Instant::now();
    let mut linker = Linker::<StoreState>::new(&engine);
    add_to_linker_async(&mut linker)?;
    wasmtime_wasi_http::p2::add_only_http_to_linker_sync(&mut linker)?;
    let pre = linker.instantiate_pre(&component)?;
    let _proxy = ProxyPre::new(pre)?;
    let t_ready = Instant::now();

    println!(
        "mode={mode} engine={:.3} read={:.3} load={:.3} link={:.3} \
         precompile={:.3} main_total={:.3}",
        ms(t0, t_engine),
        ms(t_engine, t_read),
        ms(t_load_start, t_load),
        ms(t_link0, t_ready),
        t_ser,
        ms(t0, t_ready) - t_ser - ms(t_engine, t_read),
    );
    Ok(())
}
