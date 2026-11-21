# rust-image — WASI 0.2 component (`"type": "wasm-wasi-component"`)

A thumbnail service with the [`image`](https://crates.io/crates/image)
crate: `POST /?w=W&h=H` with a PNG, JPEG, GIF or WebP body answers with the
image resized to fit WxH (Lanczos3, aspect kept) as PNG.  `GET /?w=W&h=H`
does the same for a generated 512x512 gradient.  W and H default to 128 and
are clamped to 1..2048.

The biggest guest here: several codecs and a resampler, so it shows how
compile time grows with code size.

* Source: written for wasm-bench (Apache-2.0); dependencies: `wasi`,
  `image` 0.25 with the png, jpeg, gif and webp features (MIT/Apache-2.0).
* Size: about 938 KiB, 2.3 MiB as `.cwasm`.

## Build

```sh
rustup target add wasm32-wasip2
test/wasm-bench/build.sh rust     # -> test/wasm-bench/build/rust-image.wasm
```

## Run

```json
{
    "listeners": { "127.0.0.1:8404": { "pass": "applications/rust-image" } },
    "applications": {
        "rust-image": {
            "type": "wasm-wasi-component",
            "component": "/path/to/test/wasm-bench/build/rust-image.wasm"
        }
    }
}
```

```console
$ curl -o thumb.png 'http://127.0.0.1:8404/?w=64&h=48'
$ file thumb.png
thumb.png: PNG image data, 48 x 48, 8-bit/color RGB, non-interlaced
$ curl --data-binary @photo.jpg -o thumb.png 'http://127.0.0.1:8404/?w=200&h=200'
```
