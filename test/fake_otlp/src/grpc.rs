//! OTLP/gRPC transport for fake_otlp, compiled only with `--features grpc`.
//!
//! A tonic `TraceServiceServer` that accepts the unary `TraceService/Export`
//! RPC over HTTP/2. The decoded `ExportTraceServiceRequest` is re-encoded to
//! protobuf bytes and written to the dump, so the same byte-substring
//! assertions used for the HTTP path (service.name=FreeUnit, raw trace id)
//! hold across both transports. Mirrors the HTTP contract: `--port`,
//! `--requests`, `--dump`, and the `span_received` stdout line.

use std::io::Write;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use prost::Message;
use tonic::transport::Server;
use tonic::{Request, Response, Status};

use opentelemetry_proto::tonic::collector::trace::v1::{
    trace_service_server::{TraceService, TraceServiceServer},
    ExportTraceServiceRequest, ExportTraceServiceResponse,
};

use crate::{dump_request, HOST};

#[derive(Clone)]
struct Collector {
    dump: Option<String>,
    count: Arc<AtomicUsize>,
    max_requests: Option<usize>,
    shutdown: Arc<tokio::sync::Notify>,
}

#[tonic::async_trait]
impl TraceService for Collector {
    async fn export(
        &self,
        request: Request<ExportTraceServiceRequest>,
    ) -> Result<Response<ExportTraceServiceResponse>, Status> {
        let msg = request.into_inner();

        // Re-encode the decoded request so the dump carries the same protobuf
        // bytes a byte-substring assertion expects (service name as UTF-8, the
        // trace id as 16 raw bytes) — identical to the HTTP path's raw body.
        let mut bytes = Vec::new();
        msg.encode(&mut bytes)
            .map_err(|e| Status::internal(format!("encode: {e}")))?;

        dump_request(self.dump.as_deref(), &bytes);

        println!(
            "span_received content_length={} content_type=application/grpc",
            bytes.len()
        );
        let _ = std::io::stdout().flush();

        let n = self.count.fetch_add(1, Ordering::SeqCst) + 1;
        if self.max_requests.map_or(false, |m| n >= m) {
            // Let serve_with_shutdown finish this in-flight response, then stop.
            self.shutdown.notify_one();
        }

        Ok(Response::new(ExportTraceServiceResponse::default()))
    }
}

/// Serve OTLP/gRPC until `max_requests` exports have arrived (or forever when
/// `None`). Runs on a multi-thread tokio runtime owned by this thread.
pub fn serve_grpc(port: u16, max_requests: Option<usize>, dump: Option<String>) {
    let rt = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .expect("build tokio runtime");

    rt.block_on(async move {
        let shutdown = Arc::new(tokio::sync::Notify::new());
        let collector = Collector {
            dump,
            count: Arc::new(AtomicUsize::new(0)),
            max_requests,
            shutdown: shutdown.clone(),
        };

        let addr = format!("{HOST}:{port}").parse().expect("parse listen addr");

        let signal = async move {
            if max_requests.is_some() {
                shutdown.notified().await;
            } else {
                // No cap: run until the process is killed.
                std::future::pending::<()>().await;
            }
        };

        if let Err(e) = Server::builder()
            .add_service(TraceServiceServer::new(collector))
            .serve_with_shutdown(addr, signal)
            .await
        {
            eprintln!("fake_otlp: grpc serve error: {e}");
            std::process::exit(1);
        }
    });
}
