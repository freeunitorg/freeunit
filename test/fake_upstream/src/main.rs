/// fake_upstream — live HTTP/1.x mock upstream for FreeUnit proxy tests.
///
/// Usage:
///   fake_upstream --port <N> --mode <mode> [--requests <N>]
///                 [--size <MiB>] [--delay-ms <N>]
///
/// Modes:
///   requires-cl    411 if Transfer-Encoding: chunked without Content-Length
///   no-te          400 if Transfer-Encoding present; 411 if no Content-Length
///   strict         400 if Transfer-Encoding present; 411 if no CL;
///                  400 if body length != CL value; else 200 + echo
///   echo             200 + echo body (no header enforcement)
///   chunked-response 200 + Transfer-Encoding: chunked response of --size MiB
///                    deterministic bytes, split across mixed chunk sizes
///                    (no Content-Length). Upstream half of the proxy
///                    chunked-response relay path (#72). Mirrors pytest
///                    test_proxy_chunked_response_* (shared `chunked_response`
///                    token — grep finds both sides).
///   abort-mid        chunked, write --size bytes, then close without the
///                    terminal 0-chunk (upstream dies mid-stream, #72 case 4)
///   slow-drip        chunked, one 512-byte chunk every --delay-ms ms, proper
///                    terminal chunk (relay vs proxy_read_timeout, #72 case 5)
///   dup-te           Transfer-Encoding: chunked header sent twice + valid
///                    chunked body (nginx/unit#1088, #72 case 6)
///
/// --requests N   exit after handling N connections (default: run forever)
/// --size N       chunked-response: response body size in MiB (default: 1)
/// --delay-ms N   chunked-response: sleep between chunks in ms (default: 0)

use std::env;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::process;
use std::str;

// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Debug)]
enum Mode {
    RequiresCl,
    NoTe,
    Strict,
    Echo,
    ChunkedResponse,
    AbortMid,
    SlowDrip,
    DupTe,
}

// ---------------------------------------------------------------------------

/// Runtime options resolved from the CLI.
struct Opts {
    mode: Mode,
    /// chunked-stream: total response body size in bytes.
    size: usize,
    /// chunked-stream: delay between chunks in milliseconds (0 = none).
    delay_ms: u64,
}

/// Deterministic body content: byte at global offset `i` is `PATTERN[i % 16]`.
/// Tests regenerate the same sequence to verify the relayed body byte-for-byte.
const PATTERN: &[u8; 16] = b"0123456789abcdef";

/// Chunk-size schedule (bytes), cycled across the body. Mixes tiny chunks,
/// chunks > 16 KB, and sizes deliberately not aligned to a power-of-two read
/// buffer, to exercise multi-buffer / mid-chunk parser paths in the relay.
const CHUNK_SCHEDULE: &[usize] = &[1, 17, 255, 4096, 16384, 16385, 65521, 131072, 7];

// ---------------------------------------------------------------------------

struct Request {
    method: String,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
}

impl Request {
    fn header_val(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(name))
            .map(|(_, v)| v.as_str())
    }

    fn has_header(&self, name: &str) -> bool {
        self.header_val(name).is_some()
    }

    fn has_te_chunked(&self) -> bool {
        self.header_val("Transfer-Encoding")
            .map(|v| v.to_ascii_lowercase().contains("chunked"))
            .unwrap_or(false)
    }

    fn content_length(&self) -> Option<usize> {
        self.header_val("Content-Length")?.trim().parse().ok()
    }
}

// ---------------------------------------------------------------------------

fn read_request(stream: &TcpStream) -> std::io::Result<Request> {
    let mut reader = BufReader::new(stream);

    // Request line
    let mut req_line = String::new();
    reader.read_line(&mut req_line)?;
    let method = req_line.split_whitespace().next().unwrap_or("GET").to_string();

    // Headers
    let mut headers: Vec<(String, String)> = Vec::new();
    loop {
        let mut line = String::new();
        reader.read_line(&mut line)?;
        let trimmed = line.trim_end_matches(['\r', '\n']);
        if trimmed.is_empty() {
            break;
        }
        if let Some(pos) = trimmed.find(':') {
            let name = trimmed[..pos].trim().to_string();
            let value = trimmed[pos + 1..].trim().to_string();
            headers.push((name, value));
        }
    }

    // Body — detect encoding
    let te_chunked = headers
        .iter()
        .any(|(k, v)| {
            k.eq_ignore_ascii_case("transfer-encoding")
                && v.to_ascii_lowercase().contains("chunked")
        });

    let body = if te_chunked {
        read_chunked_body(&mut reader)?
    } else {
        let cl: usize = headers
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case("content-length"))
            .and_then(|(_, v)| v.trim().parse().ok())
            .unwrap_or(0);
        let mut buf = vec![0u8; cl];
        if cl > 0 {
            reader.read_exact(&mut buf)?;
        }
        buf
    };

    Ok(Request { method, headers, body })
}

fn read_chunked_body(reader: &mut BufReader<&TcpStream>) -> std::io::Result<Vec<u8>> {
    let mut body = Vec::new();
    loop {
        let mut size_line = String::new();
        reader.read_line(&mut size_line)?;
        // Strip chunk extensions (RFC 9112: chunk-size [";" chunk-ext]).
        let size_field = size_line
            .trim_end_matches(['\r', '\n'])
            .split(';')
            .next()
            .unwrap_or("")
            .trim();
        let chunk_size = usize::from_str_radix(size_field, 16).map_err(|e| {
            std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                format!("invalid chunk size {:?}: {}", size_field, e),
            )
        })?;
        if chunk_size == 0 {
            // consume trailing CRLF after terminal chunk
            let mut crlf = String::new();
            reader.read_line(&mut crlf)?;
            break;
        }
        let mut chunk = vec![0u8; chunk_size];
        reader.read_exact(&mut chunk)?;
        body.extend_from_slice(&chunk);
        // consume trailing CRLF after chunk data
        let mut crlf = String::new();
        reader.read_line(&mut crlf)?;
    }
    Ok(body)
}

// ---------------------------------------------------------------------------

fn respond(stream: &mut TcpStream, status: u16, reason: &str, body: &[u8]) {
    let head = format!(
        "HTTP/1.1 {} {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        status,
        reason,
        body.len()
    );
    let _ = stream.write_all(head.as_bytes());
    let _ = stream.write_all(body);
}

/// Stream a `Transfer-Encoding: chunked` response of `size` deterministic bytes
/// (no Content-Length), splitting the body across CHUNK_SCHEDULE sizes. This is
/// the upstream half of the proxy chunked-response relay path (#72); the name
/// mirrors the consuming pytest `test_proxy_chunked_response_*`.
fn respond_chunked_response(stream: &mut TcpStream, size: usize, delay_ms: u64) {
    let head = "HTTP/1.1 200 OK\r\n\
                Content-Type: application/octet-stream\r\n\
                Transfer-Encoding: chunked\r\n\
                Connection: close\r\n\r\n";
    if stream.write_all(head.as_bytes()).is_err() {
        return;
    }

    let mut offset = 0usize;
    let mut sched = 0usize;

    while offset < size {
        let want = CHUNK_SCHEDULE[sched % CHUNK_SCHEDULE.len()];
        sched += 1;
        let n = want.min(size - offset);

        let mut chunk = Vec::with_capacity(n);
        for i in 0..n {
            chunk.push(PATTERN[(offset + i) % PATTERN.len()]);
        }
        offset += n;

        // <hex-size>\r\n<data>\r\n
        if stream
            .write_all(format!("{:x}\r\n", n).as_bytes())
            .and_then(|_| stream.write_all(&chunk))
            .and_then(|_| stream.write_all(b"\r\n"))
            .is_err()
        {
            return; // client/proxy went away mid-stream
        }

        if delay_ms > 0 {
            std::thread::sleep(std::time::Duration::from_millis(delay_ms));
        }
    }

    // Terminal chunk.
    let _ = stream.write_all(b"0\r\n\r\n");
    let _ = stream.flush();
}

/// Start a `Transfer-Encoding: chunked` response, write `size` deterministic
/// bytes, then close the socket **without** the terminal `0\r\n\r\n` — an
/// upstream that dies mid-stream (#72 case 4). FreeUnit must surface a clean
/// truncation to the client (closed connection, short body), never hang or
/// crash the router. Mirrors pytest `test_proxy_chunked_response_abort_mid`.
fn respond_abort_mid(stream: &mut TcpStream, size: usize) {
    let head = "HTTP/1.1 200 OK\r\n\
                Content-Type: application/octet-stream\r\n\
                Transfer-Encoding: chunked\r\n\
                Connection: close\r\n\r\n";
    if stream.write_all(head.as_bytes()).is_err() {
        return;
    }

    let mut offset = 0usize;
    let mut sched = 0usize;

    while offset < size {
        let want = CHUNK_SCHEDULE[sched % CHUNK_SCHEDULE.len()];
        sched += 1;
        let n = want.min(size - offset);

        let mut chunk = Vec::with_capacity(n);
        for i in 0..n {
            chunk.push(PATTERN[(offset + i) % PATTERN.len()]);
        }
        offset += n;

        if stream
            .write_all(format!("{:x}\r\n", n).as_bytes())
            .and_then(|_| stream.write_all(&chunk))
            .and_then(|_| stream.write_all(b"\r\n"))
            .is_err()
        {
            return;
        }
    }

    // Deliberately omit the terminal chunk and drop the stream — the upstream
    // aborts mid-response.
    let _ = stream.flush();
}

/// Stream a `Transfer-Encoding: chunked` response one small chunk at a time,
/// sleeping `delay_ms` between chunks, with a proper terminal chunk (#72
/// case 5). Probes relay behaviour vs `proxy_read_timeout`. Mirrors pytest
/// `test_proxy_chunked_response_slow_drip`.
fn respond_slow_drip(stream: &mut TcpStream, size: usize, delay_ms: u64) {
    const DRIP: usize = 512;

    let head = "HTTP/1.1 200 OK\r\n\
                Content-Type: application/octet-stream\r\n\
                Transfer-Encoding: chunked\r\n\
                Connection: close\r\n\r\n";
    if stream.write_all(head.as_bytes()).is_err() {
        return;
    }

    let mut offset = 0usize;

    while offset < size {
        let n = DRIP.min(size - offset);

        let mut chunk = Vec::with_capacity(n);
        for i in 0..n {
            chunk.push(PATTERN[(offset + i) % PATTERN.len()]);
        }
        offset += n;

        if stream
            .write_all(format!("{:x}\r\n", n).as_bytes())
            .and_then(|_| stream.write_all(&chunk))
            .and_then(|_| stream.write_all(b"\r\n"))
            .and_then(|_| stream.flush())
            .is_err()
        {
            return;
        }

        if delay_ms > 0 {
            std::thread::sleep(std::time::Duration::from_millis(delay_ms));
        }
    }

    let _ = stream.write_all(b"0\r\n\r\n");
    let _ = stream.flush();
}

/// Respond with the `Transfer-Encoding: chunked` header sent **twice** plus a
/// valid chunked body (nginx/unit#1088, #72 case 6). FreeUnit must not relay a
/// duplicated framing header to the client. Mirrors pytest
/// `test_proxy_chunked_response_dup_te`.
fn respond_dup_te(stream: &mut TcpStream, size: usize) {
    let head = "HTTP/1.1 200 OK\r\n\
                Content-Type: application/octet-stream\r\n\
                Transfer-Encoding: chunked\r\n\
                Transfer-Encoding: chunked\r\n\
                Connection: close\r\n\r\n";
    if stream.write_all(head.as_bytes()).is_err() {
        return;
    }

    let mut offset = 0usize;
    let mut sched = 0usize;

    while offset < size {
        let want = CHUNK_SCHEDULE[sched % CHUNK_SCHEDULE.len()];
        sched += 1;
        let n = want.min(size - offset);

        let mut chunk = Vec::with_capacity(n);
        for i in 0..n {
            chunk.push(PATTERN[(offset + i) % PATTERN.len()]);
        }
        offset += n;

        if stream
            .write_all(format!("{:x}\r\n", n).as_bytes())
            .and_then(|_| stream.write_all(&chunk))
            .and_then(|_| stream.write_all(b"\r\n"))
            .is_err()
        {
            return;
        }
    }

    let _ = stream.write_all(b"0\r\n\r\n");
    let _ = stream.flush();
}

fn handle(mut stream: TcpStream, opts: &Opts) {
    let req = match read_request(&stream) {
        Ok(r) => r,
        Err(_) => {
            respond(&mut stream, 400, "Bad Request", b"failed to parse request");
            return;
        }
    };

    match opts.mode {
        Mode::ChunkedResponse => {
            respond_chunked_response(&mut stream, opts.size, opts.delay_ms);
        }

        Mode::AbortMid => {
            respond_abort_mid(&mut stream, opts.size);
        }

        Mode::SlowDrip => {
            respond_slow_drip(&mut stream, opts.size, opts.delay_ms);
        }

        Mode::DupTe => {
            respond_dup_te(&mut stream, opts.size);
        }

        Mode::Echo => {
            respond(&mut stream, 200, "OK", &req.body);
        }

        Mode::RequiresCl => {
            if req.has_te_chunked() && !req.has_header("Content-Length") {
                respond(&mut stream, 411, "Length Required", b"Content-Length required");
                return;
            }
            respond(&mut stream, 200, "OK", &req.body);
        }

        Mode::NoTe => {
            if req.has_header("Transfer-Encoding") {
                respond(&mut stream, 400, "Bad Request", b"Transfer-Encoding not allowed");
                return;
            }
            if !req.has_header("Content-Length") && req.method != "GET" && req.method != "HEAD" {
                respond(&mut stream, 411, "Length Required", b"Content-Length required");
                return;
            }
            respond(&mut stream, 200, "OK", &req.body);
        }

        Mode::Strict => {
            if req.has_header("Transfer-Encoding") {
                respond(&mut stream, 400, "Bad Request", b"Transfer-Encoding not allowed");
                return;
            }
            match req.content_length() {
                None => {
                    respond(&mut stream, 411, "Length Required", b"Content-Length required");
                    return;
                }
                Some(cl) if cl != req.body.len() => {
                    let msg = format!(
                        "Content-Length mismatch: header={} body={}",
                        cl,
                        req.body.len()
                    );
                    respond(&mut stream, 400, "Bad Request", msg.as_bytes());
                    return;
                }
                _ => {}
            }
            respond(&mut stream, 200, "OK", &req.body);
        }
    }
}

// ---------------------------------------------------------------------------

fn usage() -> ! {
    eprintln!(
        "Usage: fake_upstream --port <N> \
         --mode <requires-cl|no-te|strict|echo|chunked-response|\
         abort-mid|slow-drip|dup-te> \
         [--requests <N>] [--size <MiB>] [--delay-ms <N>]"
    );
    process::exit(1);
}

fn main() {
    let args: Vec<String> = env::args().collect();

    let mut port: Option<u16> = None;
    let mut mode: Option<Mode> = None;
    let mut max_requests: Option<usize> = None;
    let mut size_mib: usize = 1;
    let mut delay_ms: u64 = 0;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--port" => {
                i += 1;
                port = args.get(i).and_then(|v| v.parse().ok());
            }
            "--mode" => {
                i += 1;
                mode = args.get(i).and_then(|v| match v.as_str() {
                    "requires-cl" => Some(Mode::RequiresCl),
                    "no-te" => Some(Mode::NoTe),
                    "strict" => Some(Mode::Strict),
                    "echo" => Some(Mode::Echo),
                    "chunked-response" => Some(Mode::ChunkedResponse),
                    "abort-mid" => Some(Mode::AbortMid),
                    "slow-drip" => Some(Mode::SlowDrip),
                    "dup-te" => Some(Mode::DupTe),
                    _ => None,
                });
            }
            "--requests" => {
                i += 1;
                max_requests = args.get(i).and_then(|v| v.parse().ok());
            }
            "--size" => {
                i += 1;
                size_mib = args.get(i).and_then(|v| v.parse().ok()).unwrap_or(size_mib);
            }
            "--delay-ms" => {
                i += 1;
                delay_ms = args.get(i).and_then(|v| v.parse().ok()).unwrap_or(delay_ms);
            }
            _ => {}
        }
        i += 1;
    }

    let port = port.unwrap_or_else(|| usage());
    let mode = mode.unwrap_or_else(|| usage());
    let opts = Opts {
        mode,
        size: size_mib * 1024 * 1024,
        delay_ms,
    };

    let listener = TcpListener::bind(("127.0.0.1", port)).unwrap_or_else(|e| {
        eprintln!("bind {}:{} — {}", "127.0.0.1", port, e);
        process::exit(1);
    });

    let mut count = 0usize;
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                handle(s, &opts);
                count += 1;
                if max_requests.map_or(false, |n| count >= n) {
                    break;
                }
            }
            Err(_) => break,
        }
    }
}
