# USDT probes

`./configure --usdt` adds static probes for bpftrace, perf and SystemTap.
The provider is `freeunit`.  The build needs `sys/sdt.h` (package
`systemtap-sdt-dev` or `systemtap-sdt-devel`).  Without `--usdt`,
`NXT_USDT()` (`src/nxt_usdt.h`) is empty.  With it, each probe is a `nop`
until a tracer attaches.

| Probe | Where | Arguments |
|---|---|---|
| `port__send` | `nxt_port_socket_write2()`, when the message is accepted | `stream, type` |
| `port__recv` | `nxt_port_read_msg_process()`, before the handler gets a whole message (after the last fragment) | `stream, type` |
| `queue__enqueue` | `nxt_app_queue_send()`, before the message is in the queue | `slot, tracking id` |
| `queue__enqueue__fail` | `nxt_app_queue_send()`, when the enqueue then fails | `slot, tracking id` |
| `queue__dequeue` | `nxt_unit_app_queue_recv()`, in the application | `slot, tracking id` |
| `queue__cancel` | `nxt_router_msg_retract()`, in the router | `slot, tracking id` |
| `queue__no_doorbell` | `nxt_router_app_prepare_request()`, when no READ_QUEUE is sent (`notify == 0`): the enqueued item lands at or beyond the doorbell bound, or the shared port is backlogged | `slot, tracking id` |
| `process__spawn` | `nxt_process_create()`, in the parent | `child pid` (global, also with pid isolation) |
| `request__start` | `nxt_h1p_conn_request_init()` | `(uintptr_t) r` |
| `request__done` | `nxt_http_request_close_handler()`, once per request | `(uintptr_t) r, status` (0: no response) |

`type` is the message type without the "last" flag (`nxt_port_msg_type_t`
in `src/nxt_port.h`).  libunit has its own send code, so a message from an
application fires only `port__recv`.  A request reaches an application
through the app queue, so the queue probes cover it.

The application and the router claim a queued message with a
compare-and-swap, and the probe fires only on the side that wins.  So a
message ends at exactly one of `queue__dequeue` and `queue__cancel`.  The
pair (slot, tracking id) names a message: a slot alone repeats across app
queues.

## Scripts

The scripts in `tools/usdt/` take the path of the traced binary.  For a
running daemon, `/proc/<pid>/exe` gives it.  `queue__dequeue` is in libunit,
so `queue-residency.bt` also takes the language module (`.so`) or the
external application binary.

```
UNITD=$(readlink /proc/$(pgrep -f 'unit: main' | head -1)/exe)
bpftrace -l "usdt:$UNITD:*"
bpftrace tools/usdt/requests-by-status.bt $UNITD
bpftrace tools/usdt/queue-residency.bt $UNITD <module or application>
```
