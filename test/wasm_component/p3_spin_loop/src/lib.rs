//! A WASI 0.3 guest that never yields and never answers.
//!
//! The WASI 0.2 spin_loop next to it exists to prove the host can preempt a
//! guest with the wasmtime epoch deadline that "execution_timeout" arms.  A
//! WASI 0.3 handler runs inside `Store::run_concurrent()`, so the same test
//! is needed on this path: without the deadline the store keeps the task
//! running, and the request never completes.
//!
//! black_box() keeps the loop from being optimised away.

use wasip3::http::types::{ErrorCode, Request, Response};
use wasip3::wit_bindgen;

wasip3::http::service::export!(Spin);

struct Spin;

impl wasip3::exports::http::handler::Guest for Spin {
    async fn handle(_request: Request) -> Result<Response, ErrorCode> {
        let mut n: u64 = 0;

        loop {
            n = core::hint::black_box(n).wrapping_add(1);
        }
    }
}
