# USDT probes

## Building

```
apt-get install -y systemtap-sdt-dev   # <sys/sdt.h>
./configure --usdt ...
make -j2
```

Without `--usdt`, `NXT_USDT()` (`src/nxt_usdt.h`) compiles to nothing; with
it, each probe is a `nop` until a tracer attaches.

## Probes

| Probe | Call site | Arguments |
|---|---|---|
| `freeunit:port__send` | `nxt_port_socket_write2()` | `stream, type` |
| `freeunit:port__recv` | `nxt_port_read_handler()` and `nxt_port_queue_read_handler()`, once per message processed | `port->pid` |
| `freeunit:mmap__chunk__alloc` | `nxt_port_incoming_port_mmap()` | `process->pid, PORT_MMAP_SIZE` |
| `freeunit:mmap__chunk__get` | `nxt_router_prepare_msg()` | `req_size + content_length` |
| `freeunit:queue__enqueue` | `nxt_app_queue_send()` | `slot index, tracking id` |
| `freeunit:queue__dequeue` | `nxt_app_queue_recv()`, in the application process | `slot index, tracking id` |
| `freeunit:process__spawn` | `nxt_process_create()`, parent only | `child pid` (the global one, also with pid isolation) |
| `freeunit:request__start` | `nxt_h1p_conn_request_init()`, once the request can be parsed | `(uintptr_t) r` |
| `freeunit:request__done` | `nxt_http_request_close_handler()`, once per request, on every exit | `(uintptr_t) r, status` (0: no response) |

The names are the probe names as `bpftrace -l` lists them: the `__` is
literal, it is not turned into `-`.

A message is identified by its (slot index, tracking id) pair on both
queue probes: the slot alone repeats across app queues, and a queue
pointer differs between the router and the application process.

## Example

```
bpftrace -l 'usdt:/usr/sbin/unitd:*'
bpftrace tools/usdt/requests-by-status.bt -p $(pgrep -f 'unit: router')
```

`tools/usdt/` also has `port-rtt.bt` and `queue-residency.bt`. The scripts
have not been run against a live process yet.
