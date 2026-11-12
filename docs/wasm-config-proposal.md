# Proposal: a `wasm` block for Wasm application config

Status: design proposal only. Nothing in this file is implemented. The
control API does not accept any of these options yet. This file exists so
reviewers can agree on names, types and defaults before any runtime code is
written.

Related issues: #902 (compilation cache), #904 (AOT `.cwasm` load path),
#905 (wasm benchmark), #909 (wasm umbrella issue), #912 (parallel
compilation, merged for 1.38.0).

## Why

Today a Wasm app always compiles its module or component with the
Wasmtime JIT when the app process starts. The two Wasm app types do not
start the same way today:

- `"type": "wasm"` (core module) builds the C API of Wasmtime with
  `parallel-compilation` on by default (`pkg/contrib` cmake defaults; in
  Wasmtime 48.0.8, `feature(parallel-compilation ON)`). The engine is
  created with `wasm_engine_new()` in `src/wasm/nxt_rt_wasmtime.c`, so a
  core module already compiles in parallel.
- `"type": "wasm-wasi-component"` built the Rust crate without that
  feature (`src/wasm-wasi-component/Cargo.toml` uses
  `default-features = false`).  #912 turned the feature on for 1.38.0, so
  both runtimes now compile in parallel by default; the benchmark below
  was measured before that.

The benchmark posted on #905 (from `test/wasm-bench/RESULTS.md` in #911)
shows how much the first start costs, and how much the other ways save.
Each row below names which runtime was measured:

| Mode | Runtime measured | Time to a ready module or component |
|------|------------------|-------------------------------------|
| JIT, C guests | C API (`"type": "wasm"`), already parallel | about 15-20 ms |
| JIT, Rust guests | component crate (`"type": "wasm-wasi-component"`), no parallel compile (before #912) | about 80-800 ms |
| JIT with parallel compile | component crate with `parallel-compilation` on, which #912 enabled for 1.38.0 | about 4x faster on large components (rust-image: about 800 ms to about 140 ms) |
| Compilation cache, warm | both runtimes (C API and component crate harnesses) | about 1-8 ms |
| Compilation cache, first start | both runtimes | about the same as JIT for that runtime |
| AOT `.cwasm` | both runtimes | about 0.5-0.7 ms |

The first start matters most when the router starts new app processes
under load, after a crash, or after a reconfig. The user should be able
to choose the trade-off for each app.

## Where the block goes

The new block is an optional member named `wasm`, and it is the same for
both Wasm app types: `"type": "wasm"` (core module) and
`"type": "wasm-wasi-component"`. It sits next to the existing members
(`module` or `component`, `request_handler`, `malloc_handler`,
`free_handler`, the optional `*_handler` members, `access`, and, for
components, `execution_timeout`). It follows the style of the existing
`limits` object: lower-case names with `_`, numbers for sizes in bytes and
for timeouts in seconds, and every member optional.

If the block is missing, the app works exactly as it does today.

```json
"wasm": {
    "compilation": "jit" | "aot",
    "parallel": true,
    "precompiled": "<path to .cwasm>",
    "fallback": true,

    "cache": {
        "enabled": false,
        "path": "<directory>",
        "size": 536870912
    },

    "max_memory": 268435456,
    "max_stack": 524288
}
```

`execution_timeout` stays at the app level (not inside `wasm`). It already
works for `"type": "wasm-wasi-component"` and this proposal makes it valid
for `"type": "wasm"` too. There is no second name for the guest timeout.

## Axis 1: compilation strategy

| Option        | Type    | Default | What it does | Issue |
|---------------|---------|---------|--------------|-------|
| `compilation` | string: `"jit"` or `"aot"` | `"jit"` | How the code is turned into machine code when the app process starts. | #912, #904 |
| `parallel`    | boolean | `true`  | With `"jit"`: compile on several threads when the build has Wasmtime parallel compilation. Set to `false` only when one thread is wanted. Has no effect with `"aot"` (unless fallback to JIT). | #912 |
| `precompiled` | string (path) | none; required when `compilation` is `"aot"` | Where to find the AOT file. There is no derived default from the module or component path. | #904 |
| `fallback`    | boolean | `true`  | With `"aot"`: if the `.cwasm` cannot be used, fall back to JIT loudly (see below). With `false` the app fails to start (strict mode for CI / pre-production). | #904 |

The two `compilation` values:

- `"jit"`: compile when the process starts. With `parallel: true` (the
  default) this matches today's behaviour of both runtimes: the core
  module always had parallel compilation and #912 enabled it for the
  component runtime for 1.38.0. With `parallel: false` the engine turns
  parallel compilation off for that app. Setting `"jit"` does not mean
  "one thread"; use `parallel: false` for that.
- `"aot"`: load a `.cwasm` file that was compiled ahead of time. The
  `precompiled` path must be set explicitly. If the file is missing,
  stale, corrupt, or does not match this build, use `fallback`.

`"parallel"` is not a `compilation` value. Parallel compilation is a
build feature that #912 enabled for the component runtime (1.38.0) and
that the C API build always had. A boolean keeps the strategy list short
and matches what Wasmtime exposes.

### AOT fallback (default: loud JIT)

When `compilation` is `"aot"` and `fallback` is `true` (the default), a
failed AOT load does not stop the app. Unit falls back to JIT, but it
must be loud:

- Log at **ERROR** (or raise an alert) with the Wasmtime reason string,
  so operators see the miss without digging in debug logs.
- Expose the per-app result in `/status`, for example
  `"code": "aot"` when the `.cwasm` loaded, or
  `"code": "jit-fallback"` when JIT ran instead. Exact field names are
  open; the point is that status shows which path was taken.
- `fallback: false` is the strict switch: the app fails to start. Use
  that in CI and pre-production to catch a stale or missing `.cwasm`
  before production.

### Source hash with the `.cwasm`

Store the **sha256 of the source `.wasm`** together with the `.cwasm`
(embedded in the file, or as a sidecar next to it). On load, hash the
current `.wasm` and compare. A mismatch means the source changed and the
`.cwasm` is stale: treat it as a load failure and follow `fallback`.

An optional checksum of the `.cwasm` bytes themselves can catch
corruption. Wasmtime does **not** detect a changed code byte in a
`.cwasm`: a one-byte edit was tested and the file still loaded, and the
guest returned 43 instead of 42. So Unit must check integrity itself;
Wasmtime's deserialize is not a integrity check.

### When to rebuild the `.cwasm`

Rebuild the `.cwasm` after every Unit upgrade, including patch releases.
Wasmtime 48 compares only the **major** version when loading a
`.cwasm`, so a Cranelift fix in a patch release does not invalidate old
files on its own. A file built against an older Unit build can still
load and run the old machine code.

Also rebuild after changing `execution_timeout`. That option toggles
epoch interruption in the engine, and a `.cwasm` built without it (or
with a different setting) then fails to load.

### How to load the file

Read the `.cwasm` bytes into memory and call deserialize. Do **not**
mmap the file for the lifetime of the process: a deploy that overwrites
the path in place can change mapped pages under the engine. Deploy by
writing a new file and renaming it into place, or keep the
read-then-deserialize path so the process holds a stable copy.

### Cost of a fallback

A fallback is a one-time JIT cost for that app process:

- Core module (`"type": "wasm"`): about **15-19 ms** once per app
  process (the C API already compiles in parallel).
- Component (`"type": "wasm-wasi-component"`): about **80-140 ms** per
  worker for typical guests; a large guest such as rust-image is about
  **0.8 s**, and the process pays about **+28 MiB RSS** at start for the
  compile.

Under load, many workers falling back at once can pile up start cost.
That is one reason the router restart back-off in #596 matters for Wasm
apps: without a delay, a wave of new processes can all hit the slow
path together.

### Prior art

Other runtimes that load ahead-of-time or cached code take a similar
stance: fall back or refuse, and surface the miss.

- Java HotSpot `AOTMode`: `auto` falls back; `required` fails closed.
- .NET ReadyToRun: a mismatched image falls back to JIT.
- WasmEdge: AOT modules; version or format mismatch refuses the file.
- Node.js `vm` script `cachedData`: emits `cachedDataRejected` and runs
  without the cache.
- PHP OPcache: checksums source (and optionally the cache) so a changed
  script is recompiled.
- `wasmtime run --allow-precompiled`: opt-in unsafe load of a
  `.cwasm`.
- Envoy Wasm `allow_precompiled`: strict precompiled load, no source
  hash of its own.

Unit's default matches the "auto / fall back loudly" side; `fallback:
false` matches the "required / fail closed" side.

Open questions:

- **Trust of `.cwasm` files.** Loading a `.cwasm` file runs its machine
  code with no checks (Wasmtime marks `deserialize` as unsafe). A changed
  `.cwasm` file is the same as running any code the attacker wants inside
  the app process. The source sha256 catches a stale `.wasm`, and an
  optional `.cwasm` checksum catches corruption, but neither replaces an
  ownership check. Should Unit refuse a `.cwasm` file that the app user
  can write, or that is not owned by root or the unitd user?
- **Who makes the `.cwasm`.** A packaging step with `wasmtime compile`
  only matches if it uses the same Wasmtime version and settings as unitd.
  A `unitd` helper that prints or builds the right file may be safer. That
  belongs in #904.
- **Status field name.** Is `"code": "aot" | "jit-fallback"` the right
  shape under `/status`, or should it sit under a `wasm` object?
- **Name.** `compilation` or `compile` or `strategy`?

## Axis 2: compilation cache

| Option          | Type    | Default | What it does | Issue |
|-----------------|---------|---------|--------------|-------|
| `cache`         | object or boolean | not set (no cache) | Wasmtime compilation cache. `true` is short for `{"enabled": true}`. | #902 |
| `cache.enabled` | boolean | `false` | Turn the cache on or off without removing the other settings. | #902 |
| `cache.path`    | string (directory) | open question, see below | Where compiled code is kept between process starts. Written into a generated TOML file as `directory`. | #902 |
| `cache.size`    | integer (bytes) | open question, for example 512 MiB | Soft limit for the total size of the cache. Written into the same TOML as `files-total-size-soft-limit`. Old entries are removed when it is exceeded. | #902 |

Wasmtime does not take cache path or size as engine parameters. The C API
only accepts a path to a cache config file
(`wasmtime_config_cache_config_load`). The core-module build already has
the cache feature compiled in (`feature(cache ON)`), but the cache stays
off until that file is loaded. The component crate still needs the
`cache` feature added in `src/wasm-wasi-component/Cargo.toml` (the line
where "parallel-compilation" now sits, after #912).

So Unit must write a small per-app TOML from `cache.path` and
`cache.size`, for example:

```toml
[cache]
directory = "/var/cache/unit/wasm/my-app"
files-total-size-soft-limit = "512MiB"
```

and pass that file to Wasmtime. The TOML file itself must be readable
inside the app's isolation (rootfs, mount namespace), because the app
process loads it. The cache directory must be writable by the app
`user`.

The cache helps the second and later starts (about 1-8 ms). The first
start still pays the full JIT cost, so the cache and `parallel: true`
work well together. With `"aot"` the cache is not used, unless the
fallback to JIT happens.

Open questions (ownership and isolation):

- **Who creates the TOML file?** Unit on reconfig? A packaging step?
  Where does it live, and who owns it (root, unitd user, app user)?
- **Who can write the cache directory?** The app process writes the
  cache, so the directory must be writable by the app `user`, and it must
  exist inside the app's isolation. Should Unit create it? Should it
  refuse a directory that other users can write?
- **Sharing.** Can two apps with different users share one cache
  directory? A shared cache lets one app place code that another app
  then runs. A safe default is one directory per app, or per user.
- **Default path.** No cache by default, or a default under the Unit
  state directory?
- **Size limit.** Wasmtime cleans up the cache in the background with a
  soft limit, so the directory can be larger than `size` for a short time.
  Is a soft limit enough?

## Axis 3: per-app limits

| Option               | Type    | Default | What it does | Issue |
|----------------------|---------|---------|--------------|-------|
| `wasm.max_memory`    | integer (bytes) | no limit beyond what the guest declares (as today) | Largest size one linear memory may grow to. A `memory.grow` past it fails in the guest. | #909 |
| `wasm.max_stack`     | integer (bytes) | 524288 (512 KiB, Wasmtime's `max_wasm_stack` default) | Largest Wasm stack. Deeper calls trap. | #909 |
| `execution_timeout`  | integer (seconds), app-level | `0`, no limit (as today for components) | Longest time one request may run inside the guest. When it passes, the guest is stopped. Valid for both `"type": "wasm"` and `"type": "wasm-wasi-component"`. | #909 |

There is no `wasm.limits` object and no `wasm.limits.timeout`. One guest
timeout name is enough: the existing `execution_timeout`. Flat names
`max_memory` and `max_stack` sit under `wasm` so they are not confused
with the app-level `limits` object.

How these relate to existing options:

- `execution_timeout` already exists for `wasm-wasi-component` (seconds,
  `0` means no limit, mapped to milliseconds by `NXT_CONF_MAP_MSEC`,
  enforced with Wasmtime epoch interruption in
  `src/wasm-wasi-component/src/lib.rs`). It stays at the app level and
  keeps working. This proposal also makes it valid for `"type": "wasm"`:
  today a core module has no guest time limit.
- `execution_timeout` is not the app-level `limits.timeout` that all app
  types already have. That one is how long the router waits for an answer
  from the app. `execution_timeout` stops the guest code itself.
- The app-level `limits` object (`timeout`, `start_timeout`, `requests`,
  `shm`) and `processes` stay as they are.
- `max_stack` is not `thread_stack_size` (Python). That is the native
  thread stack. This is the Wasm stack inside Wasmtime
  (`max_wasm_stack`).
- **Stack bound with async.** Wasmtime's default `max_wasm_stack` is
  512 KiB. When async support is on, Wasmtime requires `max_wasm_stack`
  to stay below `async_stack_size`, or engine creation fails. Config
  validation must check that `max_stack` is below the async stack size
  used for that engine (or document the fixed bound if async stack size
  is not configurable).

Open questions:

- Should `max_memory` and `max_stack` take size strings such as `"256m"`,
  or only numbers in bytes like `shm`? This proposal uses bytes, like
  `shm`.
- Does a memory limit break guests that reserve a large memory up front?

## Examples

### Development: JIT with a cache

Fast restarts while you change the code. The cache keeps later starts at a
few milliseconds, and parallel compilation (now on by default for both
runtimes) makes the first start after a change faster. Guest timeout stays
as `execution_timeout`.

```json
{
    "listeners": {
        "*:8080": {
            "pass": "applications/dev-component"
        }
    },

    "applications": {
        "dev-component": {
            "type": "wasm-wasi-component",
            "component": "/srv/app/build/app.wasm",
            "user": "app",
            "execution_timeout": 60,

            "wasm": {
                "compilation": "jit",

                "cache": {
                    "enabled": true,
                    "path": "/var/cache/unit/wasm/dev-component",
                    "size": 268435456
                }
            }
        }
    }
}
```

### Production: AOT with loud JIT fallback

The `.cwasm` file is built when the package is built and is owned by root.
`precompiled` is set explicitly, and the source sha256 is stored with it.
Every process starts in under 1 ms when the file matches. If the file is
missing, stale, or does not match after a Unit upgrade, the app still
starts with JIT, logs at ERROR with the reason, and `/status` shows
`jit-fallback`. Rebuild the `.cwasm` on every Unit upgrade and after
changing `execution_timeout`.

```json
{
    "listeners": {
        "*:80": {
            "pass": "applications/prod-component"
        }
    },

    "applications": {
        "prod-component": {
            "type": "wasm-wasi-component",
            "component": "/srv/app/app.wasm",
            "user": "app",
            "execution_timeout": 30,

            "processes": {
                "max": 8,
                "spare": 2
            },

            "wasm": {
                "compilation": "aot",
                "precompiled": "/srv/app/app.cwasm",
                "fallback": true,

                "max_memory": 268435456,
                "max_stack": 524288
            }
        }
    }
}
```

### Core module (`"type": "wasm"`)

The same block works for a core module. Parallel compilation is already
on today; `parallel: false` would turn it off for this app.
`execution_timeout` would be new for this type.

```json
{
    "applications": {
        "c-module": {
            "type": "wasm",
            "module": "/srv/app/handler.wasm",
            "request_handler": "luw_request_handler",
            "malloc_handler": "luw_malloc_handler",
            "free_handler": "luw_free_handler",
            "execution_timeout": 10,

            "wasm": {
                "compilation": "jit",
                "cache": {
                    "enabled": true,
                    "path": "/var/cache/unit/wasm/c-module",
                    "size": 134217728
                },

                "max_memory": 67108864
            }
        }
    }
}
```

Note: `"cache": true` with no `path` only works if reviewers agree on a
default cache path (see the open questions above). The examples above set
`path` explicitly.

## Not in this proposal

- Any runtime code or config validation (beyond stating the
  `max_stack` / `async_stack_size` check and the AOT fallback rules
  above).
- How the `.cwasm` file is built and how the source sha256 is embedded
  or stored as a sidecar (see #904).
- The exact `/status` JSON shape for `"code": "aot" | "jit-fallback"`.
- Fuel limits, async yielding, or a pooling allocator.
