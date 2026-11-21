// SPDX-License-Identifier: Apache-2.0

//! rust-hello: the smallest wasi:http/proxy component, one fixed response.

use wasi::http::types::{
    Fields, IncomingRequest, OutgoingBody, OutgoingResponse, ResponseOutparam,
};

wasi::http::proxy::export!(Component);

struct Component;

impl wasi::exports::http::incoming_handler::Guest for Component {
    fn handle(_request: IncomingRequest, response_out: ResponseOutparam) {
        let body = b"Hello from rust-hello\n";
        let hdrs = Fields::new();
        let _ = hdrs.set("Content-Type", &[b"text/plain".to_vec()]);
        let _ =
            hdrs.set("Content-Length", &[body.len().to_string().into_bytes()]);

        let resp = OutgoingResponse::new(hdrs);
        resp.set_status_code(200).unwrap();

        let out_body = resp.body().unwrap();
        ResponseOutparam::set(response_out, Ok(resp));

        let out = out_body.write().unwrap();
        out.blocking_write_and_flush(body).unwrap();
        drop(out);

        OutgoingBody::finish(out_body, None).unwrap();
    }
}
