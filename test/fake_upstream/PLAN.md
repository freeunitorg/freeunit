# fake_upstream — test plan & case backlog

Living checklist for proxy/chunked coverage driven by `fake_upstream`. Captures
the FreeUnit incident behind
[#72](https://github.com/freeunitorg/freeunit/issues/72) **plus** reproducible
bugs inherited from the archived `nginx/unit` tracker, so we do not forget to
add the matching regression test.

`fake_upstream` is a live HTTP/1.x mock upstream. Unit proxies to it; the test
drives Unit and asserts what the **client** receives. Keep it dependency-free
(std-only Rust) and deterministic.

## Current modes (implemented)

| Mode | Behavior |
|------|----------|
| `echo` | 200 + echo body (no header enforcement) |
| `requires-cl` | 411 if `Transfer-Encoding: chunked` without `Content-Length` |
| `no-te` | 400 if any `Transfer-Encoding`; 411 if no CL |
| `strict` | 400 on TE; 411 on no CL; 400 if body != CL; else 200 echo |
| `chunked-response` | 200 + `Transfer-Encoding: chunked`, body = `--size N` MiB deterministic `0123456789abcdef`, split across mixed chunk sizes (small, >16 KB, unaligned). Upstream half of the #72 relay path. Rust `respond_chunked_response`. |
| `abort-mid` | as `chunked-response`, but after the full `--size` body closes the socket **without** the terminal `0\r\n\r\n` — upstream provides no framing EOF. Rust `respond_abort_mid`. |
| `slow-drip` | chunked, one 512-byte chunk every `--delay-ms` ms, proper terminal chunk — relay vs `proxy_read_timeout`. Rust `respond_slow_drip`. |
| `dup-te` | 200 with `Transfer-Encoding: chunked` header sent **twice** + valid chunked body — duplicate-TE relay (nginx/unit#1088). Rust `respond_dup_te`. |

`echo`/`requires-cl`/`no-te`/`strict` answer with `Content-Length` only;
`chunked-response`/`abort-mid`/`slow-drip`/`dup-te` are chunked emitters #72
needs. Naming/port convention (one reserved port + shared token per test, Rust
fn mirrors test prefix) is in [README.md](README.md).

All modes in the original backlog are now implemented. `--size` and `--delay-ms`
are wired; keep any new flag backward-compatible with the existing CLI.

**`abort-mid` scope (review follow-up):** the current handler writes the **whole**
`--size` body, then drops the connection without the terminal chunk — it tests
the *missing-framing-EOF* path (a graceful relay must not synthesize a clean
`0\r\n\r\n`; see Defect C). It does **not** exercise an abort **inside** a data
chunk (truncated hex size / short data), which is a different relay-parser path.
If that path needs coverage, add a separate `abort-mid-data` mode rather than
overloading `abort-mid`.

## Case matrix

Status: ✅ covered · 🔲 to add

### #72 — chunked **response** relay (the incident)

| # | Case | Mode | Listener | Assertion | Status |
|---|------|------|----------|-----------|--------|
| 1 | 64 MiB chunked response, plain | `chunked-response --size 64` | `*:8080` | exact byte count + byte-pattern, no early EOF | ✅ `test_proxy_chunked_response_plain` |
| 2 | 64 MiB chunked response, TLS | `chunked-response --size 64` | `*:8443` + cert | same as #1 | ✅ `test_proxy_chunked_response_tls` |
| 3 | Varied chunk sizes (>16 KB, unaligned) | `chunked-response` | both | full body intact | ✅ folded into #1/#2 (`CHUNK_SCHEDULE`) |
| 4 | Upstream aborts mid-stream | `abort-mid` | plain | client must see truncation (no synthesized terminal chunk); router survives | ✅ `test_proxy_chunked_response_abort_mid` |
| 5 | Slow relay vs read timeout | `slow-drip` | plain | relay completes byte-exact under the timeout | ✅ `test_proxy_chunked_response_slow_drip` |

**Result of cases 1–3:** the basic chunked-**response** relay is NOT broken —
64 MiB streams byte-exact over both plain (H1) and TLS listeners. So the Gitea
incident is not a relay-truncation bug; see case 7 below for the actual root
cause.

### Upstream-inherited (add as regression while we are here)

| # | Case | Source | Mode | Assertion | Status |
|---|------|--------|------|-----------|--------|
| 6 | Response TE not duplicated | nginx/unit#1088 | `dup-te` | client sees ≤1 `Transfer-Encoding`; body decodes once | ✅ `test_proxy_chunked_response_dup_te` |
| 7 | Proxy + TLS + large download + client disconnect | nginx/unit#828 | `chunked-response` (TLS) + client RST mid-read | router process survives (no SIGSEGV); disconnect not logged at `alert` | ✅ `test_proxy_chunked_response_client_abort` |
| 8 | Chunked client **request** → CL to upstream | nginx/unit#445, FreeUnit #58 | `strict` | upstream gets correct `Content-Length`; >16 KB multi-buffer intact | ✅ (`test_proxy_chunked.py`) |
| 9 | Streaming request to target (no full buffering) | nginx/unit#1282 | — | future; epic nginx/unit#1278 deferred mode | 🔲 (deferred) |

### Already covered (do not duplicate)

`test_proxy_chunked.py`: request→CL ≤ 2 MiB, multi-chunk, empty body, chunk
extensions (`da63c36a` / RFC 9112 §7.1), POST/PUT/DELETE, concurrent.

## Findings (2026-05-30) — case 7, the Gitea `git clone` incident

Reproduced with `test_proxy_chunked_response_client_abort`: TLS listener proxies
`chunked-response --size 64`; the client reads one record, then hard-closes with
`SO_LINGER 0` (RST) mid-relay. **Two distinct defects surface** — the second is
the real root cause.

### Defect A — client disconnect logged at `[alert]` (severity) — FIXED

The reported log line:

```
[alert] SSL_write(23, …, 4096) failed (11: Resource temporarily unavailable)
        (32: [null]) (OpenSSL: error:80000020:system library::Broken pipe:…)
```

Trace: client aborts → `SSL_write` ≤0; `errno` reads `EAGAIN(11)`, but on
OpenSSL 3.x the real `EPIPE(32)` / `ECONNRESET(104)` is in the **error queue**
(`ERR_LIB_SYS`). `nxt_openssl_log_error_level()` ran `ERR_GET_REASON()` (= the
errno) through the `SSL_R_*` switch, which has no case for 32/104 → fell to the
`default: NXT_LOG_ALERT`. A plain peer disconnect must not be an alert.

**Fix (applied, `src/nxt_openssl.c`):** in `nxt_openssl_log_error_level()`, when
`ERR_GET_LIB(err) == ERR_LIB_SYS`, classify via
`nxt_socket_error_level(ERR_GET_REASON(err))` (EPIPE/ECONNRESET → `NXT_LOG_INFO`).
Verified: 5/5 runs now log the failure at `[info]`, never `[alert]`.

### Defect B — router SIGSEGV (use-after-free) — ROOT CAUSE — FIXED

The same abort also crashes the router (matches nginx/unit#828):

```
[alert] process 3501 exited on signal 11 (core dumped)   ← router segfault
```

Backtrace (debug build):

```
#0 nxt_h1p_peer_timer_value   src/nxt_h1proto.c:3044
       peer = c->socket.data;
       return nxt_value_at(nxt_msec_t, peer->request->conf->socket_conf, data);
#1 nxt_conn_timer             src/nxt_conn.c:111   (state = nxt_h1p_peer_read_state)
#2 nxt_conn_io_read           src/nxt_conn_read.c:126
#3 nxt_event_engine_start     src/nxt_event_engine.c:542
#4 nxt_router_thread_start    src/nxt_router.c:3727
```

Core evidence:

| expr | value | meaning |
|------|-------|---------|
| `c` (peer conn) | `0x…7210` | alive |
| `c->socket.data` (`peer`) | `0x…d640` | non-NULL |
| `peer->request` | **`0x5555555555555555`** | freed-memory **poison** (0x55 fill) |
| `c->socket.fd` | `93` | peer socket still open / readable |
| `c->read_state` | `nxt_h1p_peer_read_state` | armed for upstream read |

**Root cause:** when the client RSTs mid-relay, the request is closed and freed
(memory poisoned to `0x55…`), but the **upstream (peer) connection still has a
pending read event** and its fd stays registered. The queued
`nxt_conn_io_read` then fires on the peer conn, `nxt_conn_timer` calls the
`timer_value` callback `nxt_h1p_peer_timer_value`, which dereferences the freed
`peer->request->conf->socket_conf` → SIGSEGV. The peer read path is not
disarmed/cancelled before the request teardown frees its objects.

**Fix (applied, `src/nxt_h1proto.c`, `nxt_h1p_peer_close`):** set `c->block_read = 1`
and call `nxt_timer_disable(engine, &c->read_timer)` before the deferred
`nxt_conn_close`.  `block_read` makes the already-queued `nxt_conn_io_read` bail
at `nxt_conn_read.c:52`; the timer disable kills the autoreset arm.  fd removal
stays deferred to `nxt_conn_close_handler` as before.

**Reframed #72:** Defect B (router crash) is the actual incident root cause; the
client's `GnuTLS recv error (-9)` is the symptom of the router dying mid-stream.
Defect A is real but secondary (log noise). The AI-drafted bug report's "router
crash" guess was correct; the "no crash" reading during this investigation was
wrong (first log grep read the wrong file).

### Defect C — upstream abort mid-stream masked as complete — FIXED

`abort-mid`: upstream writes the full body then closes **without** the terminal
`0\r\n\r\n`. Previously FreeUnit emitted its own terminal chunk, so the client
saw a well-framed "complete" response despite the backend never finishing.

Fix (`src/nxt_h1proto.c`, `nxt_h1p_peer_closed`): when upstream EOF arrives
mid-chunked-relay (`h1p->chunked && !h1p->chunked_parse.last`), route to the
error path (NXT_HTTP_BAD_GATEWAY) rather than synthesising a last buffer.  With
headers already sent the error path calls `discard`, resetting the client
connection — the client detects the truncation, matching nginx behaviour.

## Review follow-ups (REVIEW.AI.GLM-5.1, 2026-05-30) — APPLIED

All three items applied in this session:

- ✅ `test_proxy_chunked_response_client_abort`: positive `[info]` assertion added.
- ✅ `test_proxy_chunked_response_abort_mid`: `assert len(raw) > 200` floor added.
- ✅ `test_proxy_chunked_response_dup_te`: status-code pin added before `te_count` check.

Declined (low value): "ERR_peek_error double-log" in `nxt_openssl.c` is
speculative — the prior code already used `peek`, behavior is preserved. The
`Opts`-has-no-port, `read_chunked_body` invalid-hex→0, `_recv_all` 120 s timeout,
and README port-7995 wording notes are correct nits, not blockers.

## Notes

- Test sizes for #72 must exceed the ≤ 2 MiB ceiling of the existing request
  tests; the live incident was ~52 MiB. The implemented tests use 64 MiB and a
  custom O(n) reader (the shared harness `recvall`/`_parse_chunked_body` are
  O(n²) and choke at that size).
- H1/H2 split is the whole point: case 1 (plain) isolates the chunked-relay
  parser; case 2 (TLS) adds `nxt_openssl.c`. Both pass → relay is sound.
- Reproducing the crash needs a debug build + core dump: the router drops
  privileges, so set `fs.suid_dumpable=1` and a world-writable
  `kernel.core_pattern` (e.g. `/tmp/core.%p`); `--privileged` + `ulimit -c
  unlimited` in the container.
