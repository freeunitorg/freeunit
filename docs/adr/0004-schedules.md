# 0004. `schedules`: periodic internal requests to an application

Status: accepted, shipped in 1.36.2. User reference:
[docs/schedules.md](../schedules.md).

## Context

PHP applications such as Drupal need a periodic trigger. The options were
`automated_cron` (needs traffic, runs at random moments inside a visitor
request), a host or sidecar cron running `curl` (one more moving part, outside
the FreeUnit configuration), or a worker loop (not possible in PHP).

We want FreeUnit to issue the request itself, configured next to the
application. The hard part is injecting a request that has no client
connection into a router that assumes one: `r->conf` is dereferenced without
NULL checks in about a dozen places, protocol calls stall or crash on a NULL
`r->proto`, and configuration memory lives only as long as its reference
counts.

## Decision

A top-level `schedules` object; each run is a synthetic `GET` through a
"devnull" HTTP protocol, holding an internal socket configuration and joint
exactly as a listener request holds its own.

Rejected: a request with `r->proto.any = NULL` and `r->conf = NULL`. It never
gets past `nxt_http_request_start()`, never completes, leaks its pool, and
making it work means NULL guards on the hot path. Plan B was a loopback
client through the proxy peer code, or a `curl` loop in the image.

### 1. Configuration

Each schedule names a `pass`, a `uri`, an `interval`, and optionally
`jitter`, `timeout`, `overlap`, `headers` and `run_on_start`; see
docs/schedules.md. Runs are `GET` over `HTTP/1.1`.

### 2. Validation

- `pass` must be `applications/<app>[/<target>]`, without variables: routes
  can reach `proxy` and upstreams, which need a peer protocol and
  `r->conf->upstreams`.
- `uri` is 1 to 4096 bytes, starts with `/`, and has no whitespace, control
  bytes or `#`. The validator runs the router's request parser on it, so a
  target the router would refuse fails the `PUT`.
- `interval` and `timeout` are 1 to 2147483 s, `jitter` at most `interval`,
  and `interval + jitter` at most 2147483: timers are `nxt_msec_t` compared as
  a signed 32-bit difference, about 24.8 days.
- Header names are tokens and values have no control bytes other than tab.
  A name fits the application protocol's `uint8_t` with the target type's
  prefix: 255 bytes, or 250 for PHP, Perl and Ruby (`HTTP_`).
  `Content-Length`, `Transfer-Encoding`, `Connection`, `Upgrade`,
  `Keep-Alive`, `TE`, `Expect` and `Sec-WebSocket-*` are reserved: their h1
  handlers assume a connection or a body. All headers fit in 8 KiB.
- Names are 1 to 128 printable ASCII bytes.

### 3. Router data

`nxt_router_schedules_t`, from `rtcf->mem_pool`, holds the parsed schedules
with prebuilt request headers, and the internal `skcf` and joint. A state per
name, touched only on the main engine, survives reconfiguration and holds
the timer, `running` and the counters. Parsing allocates only from the pool,
so a failed configuration leaves nothing behind.

### 4. Activation and reconfiguration

`nxt_router_schedules_apply()` runs on the main engine once the configuration
can no longer fail. States are matched by name, so a run in flight still
counts as running under the new configuration. A new schedule is armed for
`interval` plus jitter, or about 1 s with `run_on_start`. If only `uri` or the
headers change, the timer is kept; if `interval` or `jitter` changes, the new
wait counts from when the current one started. A removed schedule's state is
freed at once, or when its run in flight completes. The previous joint's base
reference is released by a job on its engine, so runs in flight keep their
configuration alive.

### 5. Timer and worker engine

The state timer lives on the main engine, like the application idle timer.
When it fires it re-arms first. If a run is still in flight the new one is
skipped, counted and logged (`overlap: "skip"`, the only mode). Otherwise the
run is posted to the joint's engine, the first entry of `router->engines`.
It must not run on the main engine, whose port would parse the application's
reply as a new configuration.

### 6. The synthetic request

#### 6.3 The internal joint

A run needs a fully populated `r->conf`. Each configuration with schedules
gets one `nxt_socket_conf_t` with a listener's defaults and `settings.http`
applied, and one joint. The chain is the usual one: the joint holds the
`skcf`, the `skcf` holds the rtcf, each run holds the joint. Two consequences:

- a configuration with schedules and no listeners stays alive: without the
  joint `nxt_router_conf_ready()` would free it at once;
- the joint sits in its engine's `joints` queue, so a `listen_threads`
  decrease cannot end that engine under a run.

The `skcf` link is self-linked, since `nxt_router_conf_release()` unlinks it.
The joint count is not atomic, so it is only touched on its engine.

#### 6.4 The devnull protocol

`NXT_HTTP_PROTO_DEVNULL` fills the slot reserved in `nxt_http_proto[]`
(`src/nxt_http_devnull.c`). With it every guarded protocol call works
unchanged: `body_read` calls `ready_handler` with no body, `header_send`
proceeds straight to the body, `send` counts the bytes, keeps the first 256
for the log and completes every buffer, and `close` hands the joint back to
the owner. `$body_bytes_sent` works; `$response_connection` and
`$response_transfer_encoding` now check for h1.

#### 6.5 Building the request

On the worker engine a run creates a request with the joint as `r->conf`,
parses a copy of the prebuilt `GET <uri> HTTP/1.1` header with the client
parser, so the target is normalised as a client's would be, and processes
only the protocol-neutral fields (`Host`, `Cookie`, `User-Agent`, ...). It
adds `User-Agent: FreeUnit-Schedule/<name>` unless one is set, uses
`127.0.0.1` as the remote and `127.0.0.1:80` as the local address (Symfony
builds URLs from `SERVER_PORT`), and calls `nxt_http_request_action()` with
the schedule's own `pass`.

### 7. Completion, timeout and logging

#### 7.1 Completion

The devnull `close` logs the result while the configuration is still held,
posts it to the main engine, which clears `running` and updates the
counters, and then drops the run's joint reference. A status of 400 or more,
or a discarded response, counts as `failed`.

#### 7.2 Timeout

A run has its own timer; `r->timer` already carries `limits.timeout`. On
expiry it calls `nxt_router_request_expire()`, factored out of
`nxt_router_app_timeout()`: a queued request is retracted, a claimed but
unacknowledged one is retried later, and a running one is abandoned and
answered with 503. The application is not stopped; the timeout only frees
the schedule.

#### 7.3 Logging

`info` logs each run with the URI cut after its last `/`, where a cron key
usually starts; failures, timeouts and skips are `warn`. The access log
records runs from `127.0.0.1` with the schedule's user agent.

### 8. Status

`/status/schedules` reports per schedule `runs`, `skipped`, `failed`,
`timed_out`, `running`, `last_start`, `last_status` and `last_duration_ms`,
and is absent when no schedule is configured. Runs also count in
`requests.total`.

## Consequences

- A run gets `limits`, the application queue, OTel and the access log like
  any request; lifetimes reuse `nxt_router_conf_release()`, and the listener
  path is unchanged. Devnull can serve other internal requests later.
- `fastcgi_finish_request()` ends a run early while the worker goes on, so
  `overlap: "skip"` does not cover the trailing work.
- A timed-out run still occupies its process; with `processes.max: 1` a long
  run blocks the site.
- State is in memory only: after a restart `run_on_start` fires again and may
  overlap an orphaned worker.
- `overlap: "queue"` shipped in 1.36.2 and was removed: no user needed it,
  and a cron endpoint catches up on its next run.
