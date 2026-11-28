# c-echo — C API (`"type": "wasm"`)

Echoes the request (path, method, query, headers, body) back as
`text/plain`.  The smallest guest: libunit-wasm, wasi-libc and one handler.

* Source: adapted from `examples/c/luw-echo-request.c` in
  [freeunitorg/freeunit-wasm](https://github.com/freeunitorg/freeunit-wasm)
  (libunit-wasm), Apache-2.0, Copyright (C) Andrew Clayton, (C) F5, Inc.
* Size: about 155 KiB (`-O2 -g`, as the CI tests build; most of it is
  DWARF), 87 KiB as `.cwasm`.

## Build

```sh
make -C pkg/contrib .wasi-sysroot .libunit-wasm
test/wasm-bench/build.sh c        # -> test/wasm-bench/build/c-echo.wasm
```

## Run

```json
{
    "listeners": { "127.0.0.1:8400": { "pass": "applications/c-echo" } },
    "applications": {
        "c-echo": {
            "type": "wasm",
            "module": "/path/to/test/wasm-bench/build/c-echo.wasm",
        "request_handler": "luw_request_handler",
        "malloc_handler": "luw_malloc_handler",
        "free_handler": "luw_free_handler",
        "module_init_handler": "luw_module_init_handler",
        "module_end_handler": "luw_module_end_handler"
        }
    }
}
```

```console
$ curl -d hello=wasm 'http://127.0.0.1:8400/echo?x=1'
c-echo

[Request Info]
REQUEST_PATH = /echo?x=1
METHOD       = POST
...
[POST data]
hello=wasm
```
