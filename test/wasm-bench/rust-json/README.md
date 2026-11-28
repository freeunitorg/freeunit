# rust-json — WASI 0.2 component (`"type": "wasm-wasi-component"`)

A small JSON API with serde/serde_json:

* `POST /transform` — JSON body in; answers with a summary (counts by
  type, nesting depth, sum of numbers) and the document with object keys
  sorted.
* `POST /shorten` with `{"url": "..."}` — answers with a stable 8-character
  code (FNV-1a in base62): the stateless half of a URL shortener.
* `GET` on any path — the service description.

* Source: written for wasm-bench (Apache-2.0); dependencies: `wasi`,
  `serde`, `serde_json` (MIT/Apache-2.0).
* Size: about 167 KiB, 435 KiB as `.cwasm`.

## Build

```sh
rustup target add wasm32-wasip2
test/wasm-bench/build.sh rust     # -> test/wasm-bench/build/rust-json.wasm
```

## Run

```json
{
    "listeners": { "127.0.0.1:8403": { "pass": "applications/rust-json" } },
    "applications": {
        "rust-json": {
            "type": "wasm-wasi-component",
            "component": "/path/to/test/wasm-bench/build/rust-json.wasm"
        }
    }
}
```

```console
$ curl -d '{"z":1,"a":[true,null,"x",2.5]}' http://127.0.0.1:8403/transform
{"document":{"a":[true,null,"x",2.5],"z":1},"summary":{"arrays":1,"bools":1,"max_depth":2,"nulls":1,"number_sum":3.5,"numbers":2,"objects":1,"strings":1}}
$ curl -d '{"url":"https://freeunit.org/"}' http://127.0.0.1:8403/shorten
{"code":"XQjKGA34","url":"https://freeunit.org/"}
```
