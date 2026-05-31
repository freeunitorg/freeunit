/// fake_otlp — mock OTLP collector for FreeUnit OpenTelemetry tests. Mirrors the
/// test/fake_upstream/ pattern: a single Rust binary installed to
/// /usr/local/bin/fake_otlp in CI. FreeUnit *exports* spans to it; the test
/// drives FreeUnit and asserts what the collector received (dumped bytes).
///
/// Usage:
///   fake_otlp --port <N> [--protocol http|grpc] [--requests <N>] [--dump <FILE>]
///
///   --port N       TCP port to listen on (127.0.0.1)
///   --protocol P   transport: "http" (default) or "grpc"
///                  (HTTP/2 unary TraceService/Export)
///   --requests N   exit after receiving N export requests (default: forever)
///   --dump FILE    append each received request to FILE
///
/// HTTP behaviour:
///   * Accepts POST /v1/traces with Content-Type: application/x-protobuf and a
///     non-empty body; malformed exports are rejected with 400 and not counted.
///   * Reads the full request using Content-Length.
///   * Replies 200 OK with an empty body. An OTLP ExportTraceServiceResponse
///     is an empty protobuf message, so an empty 200 is a valid response.
///   * Prints `span_received content_length=<N> content_type=<CT>` per request.
///
/// gRPC behaviour: accepts the unary TraceService/Export RPC, re-encodes the
/// decoded ExportTraceServiceRequest to the dump so the same byte-substring
/// assertions hold across both transports.
///
/// Span attributes, the service name and resource attributes are encoded in the
/// protobuf body as length-delimited UTF-8, so a test can assert their presence
/// with a plain byte-substring search over the dumped request — no protobuf
/// parser required.

use std::env;
use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::process;

mod grpc;

pub const HOST: &str = "127.0.0.1";

// ---------------------------------------------------------------------------

fn find_subsequence(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || haystack.len() < needle.len() {
        return None;
    }
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
}

fn header_value(headers: &str, name: &str) -> Option<String> {
    for line in headers.split("\r\n") {
        if let Some(pos) = line.find(':') {
            if line[..pos].trim().eq_ignore_ascii_case(name) {
                return Some(line[pos + 1..].trim().to_string());
            }
        }
    }
    None
}

/// Parse the HTTP request line: `METHOD PATH HTTP/x.y`.
fn request_line(raw: &[u8]) -> (String, String) {
    let end = find_subsequence(raw, b"\r\n").unwrap_or(raw.len());
    let line = String::from_utf8_lossy(&raw[..end]);
    let mut parts = line.split_whitespace();
    let method = parts.next().unwrap_or_default().to_string();
    let path = parts.next().unwrap_or_default().to_string();
    (method, path)
}

// ---------------------------------------------------------------------------

struct ExportRequest {
    raw: Vec<u8>,
    content_length: usize,
    content_type: String,
}

/// Read one HTTP request: the full header block plus a Content-Length body.
fn read_request(stream: &mut TcpStream) -> std::io::Result<ExportRequest> {
    let mut raw = Vec::new();
    let mut tmp = [0u8; 4096];

    // Read until the end of the header block.
    let header_end = loop {
        if let Some(pos) = find_subsequence(&raw, b"\r\n\r\n") {
            break pos + 4;
        }
        let n = stream.read(&mut tmp)?;
        if n == 0 {
            // connection closed before headers completed
            return Ok(ExportRequest {
                raw,
                content_length: 0,
                content_type: String::new(),
            });
        }
        raw.extend_from_slice(&tmp[..n]);
    };

    let headers = String::from_utf8_lossy(&raw[..header_end]).into_owned();
    let content_length = header_value(&headers, "Content-Length")
        .and_then(|v| v.parse::<usize>().ok())
        .unwrap_or(0);
    let content_type = header_value(&headers, "Content-Type").unwrap_or_default();

    // Read the remaining body until we have the full Content-Length.
    while raw.len() < header_end + content_length {
        let n = stream.read(&mut tmp)?;
        if n == 0 {
            break;
        }
        raw.extend_from_slice(&tmp[..n]);
    }

    Ok(ExportRequest {
        raw,
        content_length,
        content_type,
    })
}

fn respond(stream: &mut TcpStream, status: &str) {
    // ExportTraceServiceResponse is an empty proto message → empty body.
    let head = format!(
        "HTTP/1.1 {status}\r\n\
         Content-Type: application/x-protobuf\r\n\
         Content-Length: 0\r\n\
         Connection: close\r\n\r\n"
    );
    let _ = stream.write_all(head.as_bytes());
    let _ = stream.flush();
}

/// Append a received export (raw bytes) to the dump file, record-separated so
/// multiple requests stay distinguishable. Shared by the HTTP and gRPC paths.
pub fn dump_request(dump: Option<&str>, bytes: &[u8]) {
    if let Some(path) = dump {
        if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(path) {
            let _ = f.write_all(bytes);
            let _ = f.write_all(b"\n--fake_otlp-request-boundary--\n");
        }
    }
}

/// Handle one connection. Returns `true` only when a valid OTLP/HTTP export was
/// received, so a bare TCP probe (the test harness's readiness check, which
/// connects and immediately closes) and malformed requests are not counted
/// against --requests and are not written to the dump.
fn handle(mut stream: TcpStream, dump: Option<&str>) -> bool {
    let req = match read_request(&mut stream) {
        Ok(r) => r,
        Err(_) => return false,
    };

    // A readiness probe opens and closes the socket without sending anything;
    // read_request returns an empty buffer. Ignore it (no response possible).
    if req.raw.is_empty() {
        return false;
    }

    // Harden the contract: a real OTLP/HTTP export is POST /v1/traces with a
    // non-empty protobuf body. Reject anything else with 400 so a FreeUnit
    // exporter regression surfaces as a test failure instead of a false pass.
    let (method, path) = request_line(&req.raw);
    let ct_ok = req.content_type.starts_with("application/x-protobuf");
    if method != "POST" || path != "/v1/traces" || !ct_ok || req.content_length == 0 {
        eprintln!(
            "fake_otlp: rejecting malformed export: \
             method={method:?} path={path:?} content_type={:?} content_length={}",
            req.content_type, req.content_length
        );
        respond(&mut stream, "400 Bad Request");
        return false;
    }

    dump_request(dump, &req.raw);

    println!(
        "span_received content_length={} content_type={}",
        req.content_length, req.content_type
    );
    let _ = std::io::stdout().flush();

    respond(&mut stream, "200 OK");
    true
}

/// Serve OTLP/HTTP until `max_requests` valid exports have been received (or
/// forever when `None`).
fn serve_http(port: u16, max_requests: Option<usize>, dump: Option<&str>) {
    let listener = TcpListener::bind((HOST, port)).unwrap_or_else(|e| {
        eprintln!("bind {HOST}:{port} — {e}");
        process::exit(1);
    });

    let mut count = 0usize;
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                if handle(s, dump) {
                    count += 1;
                    if max_requests.map_or(false, |n| count >= n) {
                        break;
                    }
                }
            }
            // A transient accept error (e.g. a peer reset before accept
            // completes) must not kill the collector mid-test — log and keep
            // serving subsequent connections.
            Err(e) => eprintln!("fake_otlp: accept error: {e}"),
        }
    }
}

// ---------------------------------------------------------------------------

fn usage() -> ! {
    eprintln!(
        "Usage: fake_otlp --port <N> [--protocol http|grpc] \
         [--requests <N>] [--dump <FILE>]"
    );
    process::exit(1);
}

fn main() {
    let args: Vec<String> = env::args().collect();

    let mut port: Option<u16> = None;
    let mut protocol = String::from("http");
    let mut max_requests: Option<usize> = None;
    let mut dump: Option<String> = None;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--port" => {
                i += 1;
                port = args.get(i).and_then(|v| v.parse().ok());
            }
            "--protocol" => {
                i += 1;
                if let Some(v) = args.get(i) {
                    protocol = v.clone();
                }
            }
            "--requests" => {
                i += 1;
                max_requests = args.get(i).and_then(|v| v.parse().ok());
            }
            "--dump" => {
                i += 1;
                dump = args.get(i).cloned();
            }
            _ => {}
        }
        i += 1;
    }

    let port = port.unwrap_or_else(|| usage());

    match protocol.as_str() {
        "http" => serve_http(port, max_requests, dump.as_deref()),
        "grpc" => grpc::serve_grpc(port, max_requests, dump),
        other => {
            eprintln!("fake_otlp: unknown --protocol {other:?} (use http or grpc)");
            process::exit(1);
        }
    }
}
