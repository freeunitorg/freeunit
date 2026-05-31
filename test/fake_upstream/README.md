# fake_upstream

A tiny, dependency-free (std-only Rust) HTTP/1.x **mock upstream** for FreeUnit
proxy tests. FreeUnit proxies to it; the test drives FreeUnit and asserts what
the **client** receives. Each instance plays one deterministic *behavior*
(`--mode`), so a test can pin an exact upstream contract.

See [PLAN.md](PLAN.md) for the case backlog and which mode covers which issue.

## Build

```bash
cargo build --release --manifest-path test/fake_upstream/Cargo.toml
cp test/fake_upstream/target/release/fake_upstream /usr/local/bin/
```

CI builds it in the "Build fake_upstream" step; the pytest cases skip gracefully
when `/usr/local/bin/fake_upstream` is absent.

## CLI

```
fake_upstream --port <N> --mode <mode> [--requests <N>] [--size <MiB>] [--delay-ms <N>]
```

| Flag | Meaning |
|------|---------|
| `--port N` | TCP port to listen on (`127.0.0.1`) |
| `--mode M` | behavior (table below) |
| `--requests N` | exit after N connections (default: run forever) |
| `--size N` | body size in MiB for `chunked-response`/`abort-mid`/`slow-drip`/`dup-te` (default 1) |
| `--delay-ms N` | sleep between chunks in ms for `chunked-response`/`slow-drip` (default 0) |

### Modes

| Mode | Behavior |
|------|----------|
| `echo` | 200 + echo request body (no header enforcement) |
| `requires-cl` | 411 if `Transfer-Encoding: chunked` without `Content-Length` |
| `no-te` | 400 if any `Transfer-Encoding`; 411 if no `Content-Length` |
| `strict` | 400 on TE; 411 on no CL; 400 if body length != CL; else 200 echo |
| `chunked-response` | 200 + `Transfer-Encoding: chunked` response of `--size` MiB deterministic bytes (no `Content-Length`), split across mixed chunk sizes — the upstream half of the proxy chunked-**response** relay path (#72) |
| `abort-mid` | as `chunked-response`, but after `--size` bytes closes the socket **without** the terminal `0\r\n\r\n` — upstream dies mid-stream (#72 case 4) |
| `slow-drip` | chunked, one 512-byte chunk every `--delay-ms` ms, proper terminal chunk — relay vs `proxy_read_timeout` (#72 case 5) |
| `dup-te` | 200 with `Transfer-Encoding: chunked` header sent **twice** + valid chunked body — duplicate-TE relay (#72 case 6, nginx/unit#1088) |

Deterministic body: byte at global offset `i` is `"0123456789abcdef"[i % 16]`,
so a test regenerates the exact sequence and verifies the relayed body
byte-for-byte.

## Test convention: one reserved port per test + name mirroring

Each instance serves exactly one behavior, so behaviors coexist on **distinct
ports** and FreeUnit routes to whichever a case configured. Two rules keep the
suite collision-free and greppable:

**1. One fixed, documented port per test.** A test does not grab an ephemeral
port — it owns a reserved number from the registry below and launches its own
instance on it (torn down in `finally`). Fixed numbers make a run reproducible
by hand (curl / real `git clone` hit the same port the test used) and the
registry is the single place that guarantees no two cases clash.

**2. Mirror the names.** The upstream behavior carries one shared token across
all three layers — pytest function, `--mode` string, and Rust handler — so a
single `grep` finds every side of a case. E.g. token `chunked_response`:
`test_proxy_chunked_response_*` ↔ `--mode chunked-response` ↔
`respond_chunked_response()`.

### Port registry

| Port | Token | Mode (CLI) | Rust handler | pytest |
|------|-------|-----------|--------------|--------|
| 7990 | `echo` | `echo` | `respond` (echo arm) | — (shared sanity) |
| 7991 | `strict` | `strict` | `handle` (Strict arm) | chunked **request** → CL (#445, #58) |
| 7992 | `requires_cl` | `requires-cl` | `handle` (RequiresCl arm) | backend demands `Content-Length` (#1278) |
| 7993 | `no_te` | `no-te` | `handle` (NoTe arm) | backend rejects `Transfer-Encoding` (#1088) |
| 7994 | `chunked_response` | `chunked-response` | `respond_chunked_response` | `test_proxy_chunked_response_{plain,tls}` (#72) |
| 7995 | `chunked_response` | `chunked-response` | `respond_chunked_response` | `test_proxy_chunked_response_client_abort` (#72 case 7 — client RST mid-write must not log SSL_write at `[alert]`) |
| 7996 | `abort_mid` | `abort-mid` | `respond_abort_mid` | `test_proxy_chunked_response_abort_mid` (#72 case 4) |
| 7997 | `slow_drip` | `slow-drip` | `respond_slow_drip` | `test_proxy_chunked_response_slow_drip` (#72 case 5) |
| 7998 | `dup_te` | `dup-te` | `respond_dup_te` | `test_proxy_chunked_response_dup_te` (#72 case 6, nginx/unit#1088) |

A test pins its port as a module constant referencing this table:

```python
UPSTREAM_PORT = 7994                       # reserved here for #72
proc = _run_chunked_response()             # --mode chunked-response --size 64
try:
    client.conf({... "proxy": f"http://127.0.0.1:{UPSTREAM_PORT}" ...})
    ...
finally:
    proc.terminate(); proc.wait()
```

For manual reproduction, launch the same fixed ports by hand and let one
FreeUnit config fan out to all of them — no restart between cases:

```bash
fake_upstream --port 7990 --mode echo &
fake_upstream --port 7991 --mode strict &
fake_upstream --port 7994 --mode chunked-response --size 64 &
```

## Adding a mode

All modes in [PLAN.md](PLAN.md) are implemented. For a new one, give it a
**new reserved port + shared token** in the registry above, a `respond_<token>`
Rust handler, and a matching `test_..._<token>` pytest function, so the
three-way name mirroring stays complete.
