# rust-hello — WASI 0.2 component (`"type": "wasm-wasi-component"`)

The smallest `wasi:http/proxy` component: one fixed `text/plain` answer.
The same shape as `test/wasm_component/hello_world`, but built with the
`wasm32-wasip2` target, which links a component directly, so no
`cargo-component` is needed.

* Source: written for wasm-bench (Apache-2.0); dependency: the `wasi` crate.
* Size: about 84 KiB, 246 KiB as `.cwasm`.

## Build

```sh
rustup target add wasm32-wasip2
test/wasm-bench/build.sh rust     # -> test/wasm-bench/build/rust-hello.wasm
```

## Run

```json
{
    "listeners": { "127.0.0.1:8402": { "pass": "applications/rust-hello" } },
    "applications": {
        "rust-hello": {
            "type": "wasm-wasi-component",
            "component": "/path/to/test/wasm-bench/build/rust-hello.wasm"
        }
    }
}
```

```console
$ curl http://127.0.0.1:8402/
Hello from rust-hello
```
