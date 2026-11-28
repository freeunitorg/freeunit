// SPDX-License-Identifier: Apache-2.0

//! rust-image: a thumbnail service on wasi:http/proxy with the `image` crate.
//!
//! * `POST /?w=W&h=H` with a PNG, JPEG, GIF or WebP body: answers with the
//!   image resized to fit WxH (Lanczos3, aspect kept), as PNG.
//! * `GET /?w=W&h=H`: does the same for a generated 512x512 gradient, so the
//!   service can be probed without an input image.
//!
//! W and H default to 128 and are clamped to 1..=2048.  The biggest of the
//! wasm-bench guests: it carries several image codecs and a resampler.

use image::{imageops::FilterType, DynamicImage, ImageFormat, Rgb, RgbImage};
use std::io::Cursor;
use wasi::http::types::{
    Fields, IncomingRequest, Method, OutgoingBody, OutgoingResponse,
    ResponseOutparam,
};
use wasi::io::streams::StreamError;

wasi::http::proxy::export!(Component);

struct Component;

fn param(query: &str, name: &str) -> Option<u32> {
    query.split('&').find_map(|kv| {
        let (k, v) = kv.split_once('=')?;
        (k == name).then(|| v.parse().ok()).flatten()
    })
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

fn gradient() -> DynamicImage {
    let img = RgbImage::from_fn(512, 512, |x, y| {
        Rgb([(x / 2) as u8, (y / 2) as u8, ((x + y) / 4) as u8])
    });
    DynamicImage::ImageRgb8(img)
}

fn respond(out: ResponseOutparam, status: u16, ctype: &str, body: &[u8]) {
    let hdrs = Fields::new();
    let _ = hdrs.set("Content-Type", &[ctype.as_bytes().to_vec()]);
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
        let pq = req.path_with_query().unwrap_or_default();
        let query = pq.split_once('?').map(|(_, q)| q).unwrap_or("");
        let w = param(query, "w").unwrap_or(128).clamp(1, 2048);
        let h = param(query, "h").unwrap_or(128).clamp(1, 2048);

        let src = if matches!(req.method(), Method::Post | Method::Put) {
            let body = read_body(req);
            match image::load_from_memory(&body) {
                Ok(img) => img,
                Err(e) => {
                    let msg = format!("cannot decode image: {e}\n");
                    return respond(out, 400, "text/plain", msg.as_bytes());
                }
            }
        } else {
            gradient()
        };

        let thumb = src.resize(w, h, FilterType::Lanczos3);
        let mut png = Vec::new();
        if let Err(e) =
            thumb.write_to(&mut Cursor::new(&mut png), ImageFormat::Png)
        {
            let msg = format!("cannot encode PNG: {e}\n");
            return respond(out, 500, "text/plain", msg.as_bytes());
        }
        respond(out, 200, "image/png", &png);
    }
}
