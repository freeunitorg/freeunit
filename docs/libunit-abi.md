# libunit ABI notes for embedders

An embedder links libunit (`build/lib/libunit.a`) into an application
written in another language and declares its structs again by hand, as
snunit (Scala Native) does.  Nothing checks such a mirror.  This page lists
what it has to match.  Print the layout for your platform with pahole (the
dwarves package) from a configured tree:

```
make build/src/nxt_unit.o && pahole -C nxt_unit_init_s build/src/nxt_unit.o
```


## libunit and the daemon must come from one build

unitd starts an external application with `NXT_UNIT_INIT` in its
environment.  Since 1.38 it starts with `<version>/<protocol>;`, and
`nxt_unit_init()` refuses to start ("versions mismatch" or "protocol
mismatch") unless they are the `NXT_VERSION` and the
`NXT_UNIT_PROTOCOL_VERSION` that libunit was built with.  The protocol
number covers the port messages, the queues, the shared memory and the
variable, and it tells apart two builds of one release.

- A daemon before 1.38 writes only `<version>`: libunit reads protocol 0
  and refuses it.  A libunit before 1.38 refuses `1.38.0/1` as a version.
- A statically linked libunit stays the one of its build.  Build it from
  the tag of the unitd it runs under, again for each release.
- Do not parse or build `NXT_UNIT_INIT`.  Leave it in the environment and
  zero-fill the `nxt_unit_init_t` you pass, so that libunit reads it.


## `nxt_unit_init_t`

| Field | LP64 offset | ILP32 offset | Size (LP64) |
|---|---|---|---|
| `data`, `ctx_data` | 0, 8 | 0, 4 | 8 each |
| `max_pending_requests` (unused) | 16 | 8 | 4 |
| `request_data_size`, `shm_limit`, `request_limit` | 20, 24, 28 | 12, 16, 20 | 4 each |
| `callbacks` | 32 | 24 | 96 |
| `ready_port` | 128 | 72 | 32 |
| `ready_stream` | 160 | 96 | 4 |
| `router_port`, `read_port` | 168, 200 | 100, 124 | 32 each |
| `shared_port_fd`, `shared_queue_fd`, `log_fd` | 232, 236, 240 | 148, 152, 156 | 4 each |
| `max_processes` (1.38) | 244 | 160 | 4 |
| `size` (1.38) | 248 | 164 | 4 |
| sizeof | 256 | 168 | |

- `callbacks` is a struct of 12 function pointers held by value, not a
  pointer.  Mirror all 12, `ready_handler` last.  A mirror with 11 has
  every later field one pointer early.  It works only while the struct is
  zero-filled and nothing after `callbacks` is set.
- `nxt_unit_callbacks_t` is frozen since 1.38; a static assertion in
  `nxt_unit.h` keeps it at 12 members.  A later hook or field goes after
  `size`, so no field before it moves.
- Set `size` to the size of your mirror, or 0 for the 1.38 struct.
  libunit refuses a size it does not know ("nxt_unit_init_t.size is
  ..."), so a mirror built for a later release fails at once.
- Unless `ready_port.id.pid`, `ready_stream` and `read_port.id.pid` are
  all set, libunit reads the ports, the descriptors, `shm_limit`,
  `request_limit` and `max_processes` from `NXT_UNIT_INIT`.


## Common mistakes in a mirror

- `nxt_unit_ctx_t.unit` is `nxt_unit_t *`, a pointer, not a struct.
- `nxt_unit_buf_t` is three pointers: `start`, `free` (the next byte to
  write) and `end`, each at its own offset.
- `nxt_unit_port_id_t` is `pid`, `hash` and `id`, 12 bytes with 2 bytes
  of padding.  `nxt_unit_port_t` has a hole of 4 bytes before `data` on
  LP64.
- `nxt_unit_field_t` and `nxt_websocket_header_t` have bit fields.
- `nxt_unit_sptr_t` is a 32-bit offset from its own address.


## Short writes

`nxt_unit_response_write_nb(req, p, size, min_size)` returns the number of
bytes it wrote, or a negative error code, and waits only until it has
written `min_size` bytes.  **With `min_size` 0 it never waits, and a result
below `size`, also 0, means that the rest was not written:** the
application used its shared memory ("limits": {"shm"}) and the router did
not release it yet.  Write the rest when libunit calls `shm_ack_handler`.
A writer that checks only for a negative result loses the end of the body.

```c
n = nxt_unit_response_write_nb(req, p, size, 0);
if (n < 0) {
    /* error: -n is the code */
}

p += n;
size -= n;   /* size > 0: write p, size again from shm_ack_handler */
```


## Changes in 1.38

- `NXT_UNIT_INIT` starts with `<version>/<protocol>`, and libunit checks
  both (`NXT_UNIT_PROTOCOL_VERSION`, 1).  It ends with the most processes
  of the application.
- `nxt_unit_callbacks_t` is frozen at 12 members.
- `nxt_unit_init_t` has `max_processes` and `size` at its end: 256 bytes
  on LP64 (was 248) and 168 on ILP32 (was 160).  No earlier field moves.
- The vestigial `nxt_lib_stop()` exported symbol is removed (`src/nxt_lib.c`
  and `src/nxt_main.h`).  It had no caller in the tree, upstream or
  downstream, and only called `exit(0)`; the daemon quits on the
  `nxt_runtime_quit()` path.  Nothing that links libunit called it.
