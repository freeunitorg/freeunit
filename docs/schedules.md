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
| `pass`         | string  | yes      | none       | `applications/<app>` or `applications/<app>/<target>`. No variables. A route (`proxy`/an upstream) is rejected: v1 only runs schedules against an application. |
| `uri`          | string  | yes      | none       | Request target, `/path?query`, sent as a `GET`. 1 to 4096 bytes, must start with `/`. Only printable ASCII (0x21 to 0x7E) except `#`: no whitespace, no control bytes, no bytes 0x7F or above. Percent-encode anything else. |
| `interval`     | integer | yes      | none       | Seconds between runs, measured from one scheduled start to the next. 1 to 2147483 (about 24.8 days; timers are 32-bit millisecond values). |
| `jitter`       | integer | no       | `0`        | Up to this many seconds, uniformly random, added to each wait. `0` to `interval`; `interval + jitter` must not exceed 2147483. |
| `timeout`      | integer | no       | `interval` | Seconds before an unfinished run is abandoned. 1 to 2147483. See "Timeout" below. |
| `overlap`      | string  | no       | `"skip"`   | `"skip"`, the only value. See "Overlap" below. |
| `headers`      | object  | no       | `{}`       | Extra request headers, string to string. `Host` also sets `server_name`. |
| `run_on_start` | boolean | no       | `false`    | The first time the schedule appears, run it 1 s plus a random part of up to `jitter` s after the configuration is applied, instead of waiting a full interval. |

`GET` and `HTTP/1.1` are the only method and version in v1.

### Reserved headers

`headers` cannot set `Content-Length`, `Transfer-Encoding`, `Connection`,
`Upgrade`, `Keep-Alive`, `TE`, `Expect`, or any `Sec-WebSocket-*` name. A
run has no request body and no client connection. These fields frame a body
(`Content-Length`, `Transfer-Encoding`, `Expect`), control a connection
(`Connection`, `Upgrade`, `Keep-Alive`, `TE`), or start a WebSocket
handshake (`Sec-WebSocket-*`), so they have no meaning for a run. Header
values may not contain control characters other than tab, and all headers
together are limited to 8192 bytes. A header name is at most 255 bytes, or
250 bytes when `pass` names a PHP, Perl or Ruby application: those receive
each name with the `HTTP_` prefix added, and a longer name would fail every
run with 431.

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
first run happen 1 second plus a random amount between `0` and `jitter`
seconds after the configuration that introduces the schedule is applied,
instead of waiting a full interval. Every later run follows the normal
`interval`/`jitter` schedule.

A reconfiguration that does not change `interval` or `jitter` keeps the
current wait, also when it changes `pass`, `uri`, `headers`, `timeout` or
`run_on_start`. A reconfiguration that changes `interval` or `jitter` starts
a new wait, counted from when the current wait started. If that time is
already over, the run is due at once, and it is skipped if the previous run
is still in progress.

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
- The abandoned worker still counts against `processes.max` until it
  answers or exits. With `"processes": {"max": 1}`, the next runs and the
  client requests wait in the queue until then, or until their own timeout.
  A run that waits longer than its `timeout` gets `503` from the queue and
  never reaches the application. FreeUnit does not end the worker. In PHP,
  `max_execution_time` can end the script, but on Linux it counts CPU time,
  not wall-clock time, so a script that waits for I/O or sleeps can run
  longer.
- If the response header was not sent yet, the run gets a synthetic `503`
  response. If the application already sent the response header, the
  router drops the rest of the response. In both cases, `last_status` is
  `503` and `timed_out` increases by 1.
- The schedule can run again at its next interval.

## Status

`/status/schedules` has, per schedule:

- `runs`: the runs started.
- `skipped`: the runs not started because the previous run was still in
  progress.
- `failed`: the runs that ended with no response status, with a status of
  400 or more, or with a response the router discarded, plus the runs that
  could not start because the router had no memory or no worker thread.
  Those runs are not in `runs`. A run that timed out is in `timed_out`, not
  in `failed`.
- `timed_out`: the runs that reached their `timeout`.
- `running`: `1` while a run is in progress, else `0`.
- `last_start` (Unix time, seconds), `last_status` and `last_duration_ms`.

`runs`, `skipped`, `failed` and `timed_out` are unsigned 32-bit counters:
after 4294967295 they start again at 0. The counters stay across a
reconfiguration that keeps the schedule. A schedule that is removed and
added again later starts with all counters at 0. But if its last run was
still in progress at the removal, and the schedule is added again before
that run ends, the state and the counters carry over. The object is absent
when no schedule is configured. A schedule that was removed while its run
was in progress stays in the object until that run ends. Runs also count in
`requests.total`.

## Caveats

- **A timeout does not stop the application.** The worker keeps running the
  overdue request and still counts against `processes`. Keep the schedule
  `timeout` at or below the application's `limits.timeout`, and leave enough
  `processes` for visitors.

- **The access log contains the full URI, including any secret in it.** Use
  an `access_log` `format` without `$request_line` and `$uri` if the log is
  shared. The `access_log` object is global, so this format applies to all
  requests.

- **The error log shows only a part of the URI.** It drops the query, and
  then cuts the path after its last `/`: `/cron/KEY` shows as `/cron/...`,
  and `/cron.php?key=a/b` shows as `/...`. A successful run is logged at
  `debug` level only. A failed run, a timeout and a skip are logged at `warn`
  level. The line for a failed run also shows up to the first 256 bytes of
  the response body, with each byte below 0x20 or above 0x7E shown as `.`.

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
