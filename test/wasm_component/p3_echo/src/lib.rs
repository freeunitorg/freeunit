//! Echo the request back: the method, the path and the "x-echo" header in
//! response headers, the request body as the response body.

use wasip3::http::types::{ErrorCode, Fields, Method, Request, Response};
use wasip3::{wit_bindgen, wit_future, wit_stream};

wasip3::http::service::export!(Echo);

struct Echo;

impl wasip3::exports::http::handler::Guest for Echo {
    async fn handle(request: Request) -> Result<Response, ErrorCode> {
        let method = match request.get_method() {
            Method::Get => "GET".to_string(),
            Method::Head => "HEAD".to_string(),
            Method::Post => "POST".to_string(),
            Method::Put => "PUT".to_string(),
            Method::Delete => "DELETE".to_string(),
            Method::Connect => "CONNECT".to_string(),
            Method::Options => "OPTIONS".to_string(),
            Method::Trace => "TRACE".to_string(),
            Method::Patch => "PATCH".to_string(),
            Method::Other(m) => m,
        };
        let path = request.get_path_with_query().unwrap_or_default();
        let echo = request.get_headers().get("x-echo");

        let headers = Fields::new();
        let set = |name: &str, value: &[u8]| {
            headers
                .append(name, value)
                .map_err(|_| ErrorCode::InternalError(None))
        };
        set("x-wasi", b"0.3")?;
        set("x-method", method.as_bytes())?;
        set("x-path", path.as_bytes())?;
        for value in echo {
            set("x-echo", &value)?;
        }

        let (_, request_done) = wit_future::new(|| Ok(()));
        let (body, _trailers) = Request::consume_body(request, request_done);
        let body = body.collect().await;

        let (mut body_tx, body_rx) = wit_stream::new();
        let (trailers_tx, trailers_rx) = wit_future::new(|| Ok(None));
        let (response, _result) =
            Response::new(headers, Some(body_rx), trailers_rx);
        drop(trailers_tx);

        wit_bindgen::spawn_local(async move {
            body_tx.write_all(body).await;
        });

        Ok(response)
    }
}
