/// fake_upstream — live HTTP/1.x mock upstream for FreeUnit proxy tests.
///
/// Usage:
///   fake_upstream --port <N> --mode <mode> [--requests <N>]
///
/// Modes:
///   requires-cl  411 if Transfer-Encoding: chunked without Content-Length
///   no-te        400 if Transfer-Encoding present; 411 if no Content-Length
///   strict       400 if Transfer-Encoding present; 411 if no CL;
///                400 if body length != CL value; else 200 + echo
///   echo         200 + echo body (no header enforcement)
///
/// --requests N   exit after handling N connections (default: run forever)

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
}

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
        let chunk_size = usize::from_str_radix(size_field, 16).unwrap_or(0);
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

fn handle(mut stream: TcpStream, mode: Mode) {
    let req = match read_request(&stream) {
        Ok(r) => r,
        Err(_) => {
            respond(&mut stream, 400, "Bad Request", b"failed to parse request");
            return;
        }
    };

    match mode {
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
        "Usage: fake_upstream --port <N> --mode <requires-cl|no-te|strict|echo> [--requests <N>]"
    );
    process::exit(1);
}

fn main() {
    let args: Vec<String> = env::args().collect();

    let mut port: Option<u16> = None;
    let mut mode: Option<Mode> = None;
    let mut max_requests: Option<usize> = None;

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
                    _ => None,
                });
            }
            "--requests" => {
                i += 1;
                max_requests = args.get(i).and_then(|v| v.parse().ok());
            }
            _ => {}
        }
        i += 1;
    }

    let port = port.unwrap_or_else(|| usage());
    let mode = mode.unwrap_or_else(|| usage());

    let listener = TcpListener::bind(("127.0.0.1", port)).unwrap_or_else(|e| {
        eprintln!("bind {}:{} — {}", "127.0.0.1", port, e);
        process::exit(1);
    });

    let mut count = 0usize;
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                handle(s, mode);
                count += 1;
                if max_requests.map_or(false, |n| count >= n) {
                    break;
                }
            }
            Err(_) => break,
        }
    }
}
