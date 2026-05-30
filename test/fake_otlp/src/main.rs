/// fake_otlp — std-only mock OTLP/HTTP collector for FreeUnit OpenTelemetry
/// tests. Mirrors the test/fake_upstream/ pattern: a single Rust binary with no
/// external dependencies, installed to /usr/local/bin/fake_otlp in CI.
///
/// Usage:
///   fake_otlp --port <N> [--requests <N>] [--dump <FILE>]
///
///   --port N       TCP port to listen on (127.0.0.1)
///   --requests N   exit after receiving N export requests (default: forever)
///   --dump FILE    append each received request (raw headers + body) to FILE
///
/// Behaviour:
///   * Accepts POST /v1/traces (any path, really — we are lenient).
///   * Reads the full request using Content-Length.
///   * Replies 200 OK with an empty body. An OTLP ExportTraceServiceResponse
///     is an empty protobuf message, so an empty 200 is a valid response.
///   * Prints `span_received content_length=<N> content_type=<CT>` per request.
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

const HOST: &str = "127.0.0.1";

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

fn respond_ok(stream: &mut TcpStream) {
    // ExportTraceServiceResponse is an empty proto message → empty 200 body.
    let head = "HTTP/1.1 200 OK\r\n\
                Content-Type: application/x-protobuf\r\n\
                Content-Length: 0\r\n\
                Connection: close\r\n\r\n";
    let _ = stream.write_all(head.as_bytes());
    let _ = stream.flush();
}

/// Handle one connection. Returns `true` only when a real HTTP request was
/// received, so a bare TCP probe (e.g. the test harness's readiness check,
/// which connects and immediately closes) is not counted against --requests
/// and is not written to the dump.
fn handle(mut stream: TcpStream, dump: Option<&str>) -> bool {
    let req = match read_request(&mut stream) {
        Ok(r) => r,
        Err(_) => return false,
    };

    // A readiness probe opens and closes the socket without sending anything;
    // read_request returns an empty buffer. Ignore it.
    if req.raw.is_empty() {
        return false;
    }

    if let Some(path) = dump {
        if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(path) {
            let _ = f.write_all(&req.raw);
            // record separator so multiple requests stay distinguishable
            let _ = f.write_all(b"\n--fake_otlp-request-boundary--\n");
        }
    }

    println!(
        "span_received content_length={} content_type={}",
        req.content_length, req.content_type
    );
    let _ = std::io::stdout().flush();

    respond_ok(&mut stream);
    true
}

// ---------------------------------------------------------------------------

fn usage() -> ! {
    eprintln!("Usage: fake_otlp --port <N> [--requests <N>] [--dump <FILE>]");
    process::exit(1);
}

fn main() {
    let args: Vec<String> = env::args().collect();

    let mut port: Option<u16> = None;
    let mut max_requests: Option<usize> = None;
    let mut dump: Option<String> = None;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--port" => {
                i += 1;
                port = args.get(i).and_then(|v| v.parse().ok());
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

    let listener = TcpListener::bind((HOST, port)).unwrap_or_else(|e| {
        eprintln!("bind {HOST}:{port} — {e}");
        process::exit(1);
    });

    let mut count = 0usize;
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                if handle(s, dump.as_deref()) {
                    count += 1;
                    if max_requests.map_or(false, |n| count >= n) {
                        break;
                    }
                }
            }
            Err(_) => break,
        }
    }
}
