use wasi::http::types::{IncomingRequest, ResponseOutparam};

wasi::http::proxy::export!(Component);

struct Component;

// A guest that never yields and never answers.  wasi-http is linked async,
// but a component only yields at an async wasi import, so this loop keeps
// the host inside a single call_handle() poll for as long as it runs.  It
// exists to prove the host can preempt such a guest; without a deadline the
// request never completes and the worker thread stays pinned.
//
// black_box() keeps the loop from being optimised away.
impl wasi::exports::http::incoming_handler::Guest for Component {
    fn handle(_request: IncomingRequest, _response_out: ResponseOutparam) {
        let mut n: u64 = 0;

        loop {
            n = core::hint::black_box(n).wrapping_add(1);
        }
    }
}
