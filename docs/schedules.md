# Schedules

`schedules` is a top-level configuration object that makes FreeUnit issue
periodic requests to an application by itself, with no client and no sidecar
process involved. The typical use is a cron trigger for a PHP application
such as Drupal, in place of `automated_cron`, a host cron job running
`curl`, or a worker loop inside the application.

Design: [ADR 0004](adr/0004-schedules.md).

## Configuration

```json
{
    "schedules": {
        "drupal-cron": {
            "pass": "applications/drupal/index",
            "uri": "/cron/SECRET_KEY",
            "interval": 300,
            "jitter": 15,
            "timeout": 240,
            "overlap": "skip",
            "headers": {
                "Host": "example.org"
            },
            "run_on_start": false
        }
    }
}
```

`schedules` is an object keyed by schedule name (1 to 128 printable ASCII
bytes). Each schedule has these fields:

| field          | type    | required | default    | notes |
|----------------|---------|----------|------------|-------|
| `pass`         | string  | yes      | —          | `applications/<app>` or `applications/<app>/<target>`. No variables. A route (`proxy`/an upstream) is rejected: v1 only runs schedules against an application. |
| `uri`          | string  | yes      | —          | Request target, `/path?query`, sent as a `GET`. 1–4096 bytes, must start with `/`, no whitespace, control bytes or `#`. |
| `interval`     | integer | yes      | —          | Seconds between runs, measured from one scheduled start to the next. 1 to 2147483 (about 24.8 days; timers are 32-bit millisecond values). |
| `jitter`       | integer | no       | `0`        | Up to this many seconds, uniformly random, added to each wait. `0` to `interval`; `interval + jitter` must not exceed 2147483. |
| `timeout`      | integer | no       | `interval` | Seconds before an unfinished run is abandoned. 1 to 2147483. See "Timeout" below. |
| `overlap`      | string  | no       | `"skip"`   | `"skip"`, the only value. See "Overlap" below. |
| `headers`      | object  | no       | `{}`       | Extra request headers, string to string. `Host` also sets `server_name`. |
| `run_on_start` | boolean | no       | `false`    | Run about 1 s after the configuration is applied (plus jitter) instead of waiting a full interval, the first time the schedule appears. |

`GET` and `HTTP/1.1` are the only method and version in v1.

### Reserved headers

`headers` cannot set `Content-Length`, `Transfer-Encoding`, `Connection`,
`Upgrade`, `Keep-Alive`, `TE`, `Expect`, or any `Sec-WebSocket-*` name: the
run has no body and no connection, so these have nothing to act on. Header
values may not contain control characters, and all headers together are
limited to 8192 bytes. A header name is at most 255 bytes, or 250 bytes when
`pass` names a PHP, Perl or Ruby application: those receive each name with
the `HTTP_` prefix added, and a longer name would fail every run with 431.

The headers come from the configuration, not from a client. Thus
`settings.http.discard_unsafe_fields` does not apply to them: a run sends a
name with `_` or other token characters, for example `X_Cron`, unchanged.

### Drupal

The example above targets Drupal's `/cron/{cron_key}`, with the key from
`state.get('system.cron_key')`. Set `headers.Host` to a host
`trusted_host_patterns` accepts: without it `server_name` is `localhost`.
Then uninstall `automated_cron`.

## Semantics

### Interval and jitter

Each run is due `interval` seconds after the previous scheduled start (not
after the previous run *finished*), plus a uniformly random amount between
`0` and `jitter` seconds, recomputed on every wait. `run_on_start` makes the
first run happen about 1 second after the configuration that introduces the
schedule is applied, instead of waiting a full interval; every later run
follows the normal `interval`/`jitter` schedule.

### Overlap

If a run is still in flight when the next one comes due, the new run is
dropped and a warning is logged. There is never more than one run of a
given schedule in flight. `"overlap": "skip"` states this explicitly.

### Timeout

Each schedule has its own `timeout`, independent of the application's
`limits.timeout`. When a run's `timeout` expires:

- If the request is still in the queue for a process, the router retracts
  it.
- If a worker has claimed the request but has not acknowledged it yet, the
  router checks it again every second. After the acknowledgement, the next
  check treats the request as running (see the next item).
- If a worker runs the request, the router *abandons* that worker. The
  router does not stop the worker, but it does not return the worker to the
  idle pool. The router discards the answer of the worker.
- If the response header was not sent yet, the run gets a synthetic `503`
  response. If the application already sent the response header, the
  router drops the rest of the response. In both cases, `last_status` is
  `503` and `timed_out` increases by 1.
- The schedule can run again at its next interval.

## Status

`/status/schedules` has, per schedule, `runs`, `skipped`, `failed`,
`timed_out`, `running`, `last_start` (Unix time), `last_status` and
`last_duration_ms`. It is absent when no schedule is configured. A schedule
that was removed while its run was in progress stays in the object until
that run ends. Runs also count in `requests.total`.

## Caveats

- **A timeout does not stop the application.** The worker keeps running the
  overdue request and still counts against `processes`. Keep the schedule
  `timeout` at or below the application's `limits.timeout`, and leave enough
  `processes` for visitors.

- **The access log contains the full URI, including any secret in it.** Use
  an `access_log` `format` without `$request_line` and `$uri` if the log is
  shared. The `info` log line shows the URI only up to its last `/`.

- **`fastcgi_finish_request()` makes a run finish early.** The run is
  complete once the response is sent, so `overlap: "skip"` does not cover
  work the application does afterwards, as `automated_cron`-style endpoints
  do. Drupal's `/cron/{cron_key}` does not.

- **All runs use one router thread.** The runs of all schedules go to the
  same router worker engine: the first one in the list of engines. The
  router does not spread the runs across its threads.

- **Schedule state does not survive a restart.** A `run_on_start` schedule
  fires again, even if a run from before is still executing in an orphaned
  worker.
