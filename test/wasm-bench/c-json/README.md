# c-json — C API (`"type": "wasm"`)

A JSON transform service: parses the request body with
[cJSON](https://github.com/DaveGamble/cJSON) and answers with a summary
(top-level type, member count, objects/arrays/strings/numbers, sum of the
numbers) and the document compacted.  A `GET` does the same for a built-in
sample, so a plain `curl` works.

* Source: written for wasm-bench (Apache-2.0).  cJSON 1.7.18 (MIT) is not
  vendored: `build.sh` downloads the release tarball from GitHub and checks
  its SHA-256.
* Size: about 281 KiB (`-O2 -g`), 155 KiB as `.cwasm`.

## Build

```sh
make -C pkg/contrib .wasi-sysroot .libunit-wasm
test/wasm-bench/build.sh c        # -> test/wasm-bench/build/c-json.wasm
```

## Run

```json
{
    "listeners": { "127.0.0.1:8401": { "pass": "applications/c-json" } },
    "applications": {
        "c-json": {
            "type": "wasm",
            "module": "/path/to/test/wasm-bench/build/c-json.wasm",
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
$ curl -d '{"a":[1,2,3],"b":{"c":"d"}}' http://127.0.0.1:8401/
{"summary":{"type":"object","members":2,"objects":2,"arrays":1,"strings":1,"numbers":3,"number_sum":6},"document":{"a":[1,2,3],"b":{"c":"d"}}}
```

The body is read in one piece from the request buffer, so keep it well
under the module's shared memory (32 MiB by default).
