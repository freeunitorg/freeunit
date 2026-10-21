# libunit ABI notes for embedders

An embedder links libunit (`build/lib/libunit.a`) into an application
written in another language and calls it through a foreign function
interface.  snunit (Scala Native) is one.  A C or C++ program that includes
the installed headers gets the layouts from its compiler.  A binding that
declares the structs again by hand does not, and nothing checks such a
mirror.  This page lists what a mirror has to match, and how to use the
response calls without losing data or speed.

The sizes and offsets below are for LP64 (x86_64, aarch64).  Print them
for your platform with `tools/perf/layout-dump.sh` (see
[Checking a mirror](#checking-a-mirror)).


## libunit and the daemon must come from one build

unitd starts an external application with the `NXT_UNIT_INIT` environment
variable.  Since 1.38 it has this form, on one line:

```
<version>/<protocol>;<stream>;
<ready pid>,<ready id>,<ready fd>;
<router pid>,<router id>,<router fd>;
<read pid>,<read id>,<read in fd>,<read out fd>;
<shared port fd>,<shared queue fd>;
<log fd>,<shm limit>,<request limit>
```

`nxt_unit_init()` refuses to start, with a "versions mismatch" or a
"protocol mismatch" alert, unless `<version>` is the `NXT_VERSION` and
`<protocol>` is the `NXT_UNIT_PROTOCOL_VERSION` that libunit was built
with.  The protocol number covers the port messages, the queues, the
shared memory and this variable.  It changes when they change, also
between two builds of one release: before 1.38, a libunit from 1.37.0 plus
some commits and a daemon from a later commit both said 1.37.0, and the
libunit was accepted although the queue protocol had changed.

- A daemon before 1.38 writes only `<version>`.  libunit 1.38 reads that
  as protocol 0 and refuses it.  A libunit before 1.38 refuses a 1.38
  daemon, because `1.38.0/1` is not its version.
- An application that links libunit statically keeps the libunit of its
  build.  Build it from the tag of the unitd it runs under, and build it
  again for each release.  A unitd found at run time, as with `which
  unitd`, can be another release or build, and libunit then refuses it.
- Do not parse or build `NXT_UNIT_INIT`.  Leave it in the environment of
  the process that calls `nxt_unit_init()`, and zero-fill the
  `nxt_unit_init_t` you pass, so that libunit reads the variable.


## The structs

| Struct | Header | LP64 size | Used for |
|---|---|---|---|
| `nxt_unit_init_t` | `nxt_unit.h` | 248 | the argument of `nxt_unit_init()` |
| `nxt_unit_callbacks_t` | `nxt_unit.h` | 96 | held by value in `nxt_unit_init_t` |
| `nxt_unit_t` | `nxt_unit.h` | 8 | `data` |
| `nxt_unit_ctx_t` | `nxt_unit.h` | 16 | `data`, and `unit`, a pointer |
| `nxt_unit_port_id_t` | `nxt_unit.h` | 12 | in `nxt_unit_port_t` |
| `nxt_unit_port_t` | `nxt_unit.h` | 32 | `add_port`, `remove_port`, your own event loop |
| `nxt_unit_buf_t` | `nxt_unit.h` | 24 | `start`, `free`, `end` |
| `nxt_unit_request_info_t` | `nxt_unit.h` | 96 | each request |
| `nxt_unit_read_info_t` | `nxt_unit.h` | 24 | `nxt_unit_response_write_cb()` |
| `nxt_unit_request_t` | `nxt_unit_request.h` | 96 | the request, in shared memory |
| `nxt_unit_field_t` | `nxt_unit_field.h` | 16 | a header field, with bit fields |
| `nxt_unit_sptr_t` | `nxt_unit_sptr.h` | 4 | an offset in shared memory |
| `nxt_unit_response_t` | `nxt_unit_response.h` | 24 | the response header |
| `nxt_unit_websocket_frame_t` | `nxt_unit_websocket.h` | 48 | each frame |
| `nxt_websocket_header_t` | `nxt_websocket_header.h` | 10 | bytes and bit fields |

The enum constants that a mirror copies are in the same headers:
`NXT_UNIT_OK`, `NXT_UNIT_ERROR`, `NXT_UNIT_AGAIN`, `NXT_UNIT_CANCELLED`,
the `NXT_UNIT_LOG_*` levels, the `NXT_UNIT_HASH_*` field hashes and the
`NXT_WEBSOCKET_OP_*` and `NXT_WEBSOCKET_CR_*` values.

`tools/perf/layout-structs.txt` lists each of them, and the layout job of
each pull request fails when one changes.  So a change to this ABI is
always named in the pull request that makes it.

### `nxt_unit_init_t`

| Field | Offset | Size |
|---|---|---|
| `data`, `ctx_data` | 0, 8 | 8 each |
| `max_pending_requests` (unused) | 16 | 4 |
| `request_data_size`, `shm_limit`, `request_limit` | 20, 24, 28 | 4 each |
| `callbacks` | 32 | 96 |
| `ready_port` | 128 | 32 |
| `ready_stream` | 160 | 4 |
| `router_port`, `read_port` | 168, 200 | 32 each |
| `shared_port_fd`, `shared_queue_fd`, `log_fd` | 232, 236, 240 | 4 each |
| `size` (1.38) | 244 | 4 |

- `callbacks` is a struct of 12 function pointers held by value, not a
  pointer.  Mirror all 12, `ready_handler` last.  A mirror with 11 has
  every later field 8 bytes early and a struct of 240 bytes instead of
  248.  It works only while the struct is zero-filled and nothing after
  `callbacks` is set; libunit then reads `ready_port.id.pid` as 0 and
  takes the ports from `NXT_UNIT_INIT`.  libunit 1.38 also reads `size`,
  at offset 244, which is past the end of such a mirror.
- `nxt_unit_callbacks_t` is frozen since 1.38: a static assertion in
  `nxt_unit.h` keeps it at 12 members.  A hook or field that a later
  release adds goes after `size`, so no field before `size` moves.
- Set `size` to the size of your mirror, 248 for 1.38 on LP64; 0 means
  the 1.38 struct.  libunit reads a field after `size` only when `size`
  covers it, and refuses a size below the 1.38 struct or above its own
  ("nxt_unit_init_t.size is ...").  So a mirror built for a later release
  fails at once against an older libunit.
- Unless `ready_port.id.pid`, `ready_stream` and `read_port.id.pid` are
  all set, libunit ignores the ports, the descriptors, `shm_limit` and
  `request_limit` and reads them from `NXT_UNIT_INIT`.  An external
  application leaves them 0.
- `request_data_size` bytes are allocated with each request for
  `req->data`.

### Common mistakes in a mirror

- `nxt_unit_ctx_t.unit` is `nxt_unit_t *`, a pointer.  A mirror that
  holds an `nxt_unit_t` by value returns the address of the field, not
  the library.
- `nxt_unit_buf_t` is three pointers, in this order: `start`, `free` (the
  next byte to write) and `end`.  Each accessor needs its own offset.
- `nxt_unit_port_id_t` is `pid`, `hash` and `id`, 12 bytes with 2 bytes
  of padding.  `nxt_unit_port_t` has a hole of 4 bytes before `data`.
- `nxt_unit_field_t` and `nxt_websocket_header_t` have bit fields.  Read
  them as bytes with the masks of the C headers.
- `nxt_unit_sptr_t` is a 32-bit offset from the address of the
  `nxt_unit_sptr_t` itself, not from the start of its struct.

### Checking a mirror

`tools/perf/layout-dump.sh` prints the layout of every struct in the
installed headers, with the offset and the size of each member, and the
value of each enum constant, from a configured tree:

```
./configure && tools/perf/layout-dump.sh > libunit-abi-$(uname -m).txt
```

It needs pahole (the dwarves package).  The first line names the version,
the protocol number and the pointer size.  Compare your mirror with it, or
diff the files of two releases.


## Callbacks and your own event loop

- With `port_send` and `port_recv` set, libunit calls them instead of
  `sendmsg()` and `recvmsg()`.  A `port_send` must send without blocking
  when `nxt_unit_port_send_flags()` has `MSG_DONTWAIT`, and return 0 on
  `EAGAIN`.
- An embedder with its own event loop sets `add_port` and `remove_port`,
  watches `port->in_fd` when it is not -1, and calls
  `nxt_unit_process_port_msg(ctx, port)` when it is readable, until the
  call does not return `NXT_UNIT_OK`.  After `NXT_UNIT_AGAIN` or
  `NXT_UNIT_ERROR` the context can be freed already (see `nxt_unit.h`).
- One of these ports is the shared port of the application.  All its
  processes hold the same open file description, so a flag set with
  `fcntl(F_SETFL)` there, such as `O_NONBLOCK`, applies to all of them.
  Each doorbell on the shared port wakes every process that waits on it,
  and all but one find no request.  With your own loop, run few
  processes.


## Writing a response

- `nxt_unit_response_write(req, p, size)` writes all of `p` or fails.  It
  can wait until the router releases shared memory.  It returns
  `NXT_UNIT_OK` or an error code.
- `nxt_unit_response_write_nb(req, p, size, min_size)` returns the number
  of bytes it wrote, or a negative error code.  It waits only until it has
  written `min_size` bytes.  **With `min_size` 0 it never waits, and a
  result below `size`, also 0, means that the rest was not written.**
  This happens when the application has used its shared memory ("limits":
  {"shm": ...}) and the router has not released it yet.  Keep the rest,
  and write it when libunit calls `shm_ack_handler`.  A writer that checks
  only for a negative result loses the end of the body.

  ```c
  n = nxt_unit_response_write_nb(req, p, size, 0);
  if (n < 0) {
      /* error: -n is the code */
  }

  p += n;
  size -= n;

  if (size > 0) {
      /* write p, size again from shm_ack_handler */
  }
  ```

- `nxt_unit_response_buf_alloc(req, size)` gives a buffer in shared
  memory of up to `nxt_unit_buf_max()` bytes (10 MiB), or NULL for a
  larger size.  Write from `buf->free` up to `buf->end`, move `buf->free`,
  and send it with `nxt_unit_buf_send(buf)`.

### Fast paths

1. A small response: `nxt_unit_response_init()` with room for the fields
   and the body, `nxt_unit_response_add_field()`,
   `nxt_unit_response_add_content()` and `nxt_unit_response_send()`.  The
   header and the body go in one message.  A first
   `nxt_unit_response_write()` before `nxt_unit_response_send()` does the
   same when the body fits in the response buffer.
2. A body that is in memory: one write of the whole body.  libunit copies
   it into shared memory once, in parts of up to `nxt_unit_buf_max()`
   bytes, one message for each part.  Each write call is at least one
   message to the router, so do not write in small parts, and never byte
   by byte.
3. A body that is made piece by piece, from a file or an encoder:
   `nxt_unit_response_buf_alloc()`, make the data in the buffer, and
   `nxt_unit_buf_send()`.  That saves the copy from your own buffer.
4. Text: encode it to bytes once, then write the bytes.
5. Static files: let the router serve them with a `share` action.  A file
   that the application sends crosses shared memory.

The request body starts in shared memory.  `nxt_unit_request_read()` copies
it, and reads the rest from `req->content_fd` when the router kept a large
body in a file.

With `nxt_unit_run()`, scale with "processes".  A small "shm" limit gives
short writes, and "limits": {"requests": N} makes each process quit after
N requests.


## QUIT

- A normal QUIT stops the context at once.  libunit calls `quit`, and
  `close_handler` for each request in progress.  unitd sends it on
  `SIGTERM`, and the router sends it when it stops a process: on a
  reconfiguration, on an application restart, and for an idle process.
- A graceful QUIT stops taking requests, and quits when the requests in
  progress are done.  unitd sends it on `SIGQUIT`, and libunit sends it
  to itself at the request limit.
- Since 1.38, when the wait of `nxt_unit_run()` finds a graceful QUIT and
  a request doorbell together, libunit serves that one request before it
  quits.  Without this, the request waited for the next doorbell, or up to
  1 s for another context.  It does not happen after a normal QUIT, at the
  request limit, or with `nxt_unit_process_port_msg()`.

### Not done: ringing the doorbell again

A context that quits could instead ask the router to ring the shared port
again for another context.  libunit cannot ring it itself: the router
alone writes the shared port, the application holds the read end, and
libunit sets the `out_fd` of that port to -1.  A write on the read end
would land in the router's end, which nothing reads.  So this needs a new
message from the application to the router, a new `_NXT_PORT_MSG_*` slot
sent with `MSG_DONTWAIT`, which the router answers with one
`nxt_port_socket_doorbell()`.  That is a protocol change, with a new
`NXT_UNIT_PROTOCOL_VERSION`, and one more message.  It would cover the
embedders with their own event loop too.


## Changes in 1.38

- `NXT_UNIT_INIT` starts with `<version>/<protocol>`, and libunit checks
  both (`NXT_UNIT_PROTOCOL_VERSION`, 1).
- `nxt_unit_callbacks_t` is frozen at 12 members.
- `nxt_unit_init_t` has `size` at its end.  On LP64 it fills the padding:
  no field moves, and the struct stays 248 bytes.  On ILP32 it grows from
  160 to 164 bytes.
- A graceful QUIT serves the request rung for with it (see above).
