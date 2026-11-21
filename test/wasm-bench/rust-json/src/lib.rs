// SPDX-License-Identifier: Apache-2.0

//! rust-json: a small JSON API on wasi:http/proxy with serde_json.
//!
//! * `POST /transform` with a JSON body: answers with a summary of the
//!   document (counts by type, nesting depth, sum of numbers) and the
//!   document with object keys sorted.
//! * `POST /shorten` with `{"url": "..."}`: answers with a stable short code
//!   (FNV-1a of the URL in base62), the stateless half of a URL shortener.
//! * `GET` on any path: answers with the service description.

use serde::Serialize;
use serde_json::{json, Map, Value};
use wasi::http::types::{
    Fields, IncomingRequest, Method, OutgoingBody, OutgoingResponse,
    ResponseOutparam,
};
use wasi::io::streams::StreamError;

wasi::http::proxy::export!(Component);

struct Component;

#[derive(Default, Serialize)]
struct Summary {
    objects: u64,
    arrays: u64,
    strings: u64,
    numbers: u64,
    bools: u64,
    nulls: u64,
    max_depth: u64,
    number_sum: f64,
}

fn walk(v: &Value, depth: u64, s: &mut Summary) -> Value {
    s.max_depth = s.max_depth.max(depth);
    match v {
        Value::Object(m) => {
            s.objects += 1;
            let mut keys: Vec<&String> = m.keys().collect();
            keys.sort();
            let mut out = Map::new();
            for k in keys {
                out.insert(k.clone(), walk(&m[k], depth + 1, s));
            }
            Value::Object(out)
        }
        Value::Array(a) => {
            s.arrays += 1;
            Value::Array(a.iter().map(|x| walk(x, depth + 1, s)).collect())
        }
        Value::String(_) => {
            s.strings += 1;
            v.clone()
        }
        Value::Number(n) => {
            s.numbers += 1;
            s.number_sum += n.as_f64().unwrap_or(0.0);
            v.clone()
        }
        Value::Bool(_) => {
            s.bools += 1;
            v.clone()
        }
        Value::Null => {
            s.nulls += 1;
            v.clone()
        }
    }
}

fn short_code(url: &str) -> String {
    const ALPHABET: &[u8] =
        b"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    let mut h: u64 = 0xcbf29ce484222325;
    for b in url.bytes() {
        h ^= b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    let mut out = Vec::new();
    for _ in 0..8 {
        out.push(ALPHABET[(h % 62) as usize]);
        h /= 62;
    }
    String::from_utf8(out).unwrap()
}

fn read_body(req: IncomingRequest) -> Vec<u8> {
    let mut data = Vec::new();
    let Ok(body) = req.consume() else { return data };
    let Ok(stream) = body.stream() else {
        return data;
    };
    loop {
        match stream.blocking_read(64 * 1024) {
            Ok(chunk) => data.extend_from_slice(&chunk),
            Err(StreamError::Closed) => break,
            Err(_) => break,
        }
    }
    drop(stream);
    data
}

fn respond(out: ResponseOutparam, status: u16, value: &Value) {
    let mut body = serde_json::to_vec(value).unwrap();
    body.push(b'\n');
    let hdrs = Fields::new();
    let _ = hdrs.set("Content-Type", &[b"application/json".to_vec()]);
    let _ = hdrs.set("Content-Length", &[body.len().to_string().into_bytes()]);
    let resp = OutgoingResponse::new(hdrs);
    resp.set_status_code(status).unwrap();
    let ob = resp.body().unwrap();
    ResponseOutparam::set(out, Ok(resp));
    let w = ob.write().unwrap();
    for chunk in body.chunks(4096) {
        w.blocking_write_and_flush(chunk).unwrap();
    }
    drop(w);
    OutgoingBody::finish(ob, None).unwrap();
}

impl wasi::exports::http::incoming_handler::Guest for Component {
    fn handle(req: IncomingRequest, out: ResponseOutparam) {
        let path = req.path_with_query().unwrap_or_default();
        let method = req.method();

        if !matches!(method, Method::Post | Method::Put) {
            return respond(
                out,
                200,
                &json!({
                    "service": "rust-json",
                    "endpoints": ["POST /transform", "POST /shorten"],
                }),
            );
        }

        let body = read_body(req);
        let doc: Value = match serde_json::from_slice(&body) {
            Ok(v) => v,
            Err(e) => {
                return respond(out, 400, &json!({ "error": e.to_string() }))
            }
        };

        if path.starts_with("/shorten") {
            return match doc.get("url").and_then(Value::as_str) {
                Some(url) => respond(
                    out,
                    200,
                    &json!({ "url": url, "code": short_code(url) }),
                ),
                None => respond(out, 400, &json!({ "error": "missing url" })),
            };
        }

        let mut s = Summary::default();
        let sorted = walk(&doc, 0, &mut s);
        respond(out, 200, &json!({ "summary": s, "document": sorted }));
    }
}
