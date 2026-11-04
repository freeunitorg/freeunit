# Router-worker request handoff: ownership, races and the slot state machine

Design note for issue #892 (1.38).  It builds on #665, #886, #889 and #890.

Every `file:line` in this note is at commit `6e8693af0`, master with #889 merged.
That commit is `master` (`13c6ae78b`) plus the #889 change.  #890 is not in
it; where this note cites #890 it says so.

## 1. Scope

The router gives a request to a worker of an application over two channels:

- the shared application queue (`src/nxt_app_queue.h`), a ring of 131072
  slots in shared memory.  A slot holds up to 31 bytes of message and a
  32-bit `tracking` word (`src/nxt_app_queue.h:19-23`);
- the port sockets: `READ_QUEUE`, `REQ_HEADERS_ACK`, `RPC_ERROR`, `DATA`,
  `SHM_ACK`, `OOSM`, `QUIT` and `REMOVE_PID` (`src/nxt_port.h:14-64`).

Who owns a request follows from both channels together.  This note:

1. lists every transition of ownership in the current code;
2. maps the races that #886, #889 and #890 closed to the orderings that were
   missing, and names the windows that stay open;
3. proposes a state machine for a slot, with one owner per transition;
4. gives a migration in steps behind the existing queue API;
5. answers the open questions of #892 where the code allows it.

Step 1 of the migration is in this pull request.  Section 8 says what it
changes and what it does not.

## 2. The current ownership model

### 2.1 The two sides of a slot

A slot index is in exactly one of three places: the `free_items` ring, the
`queue` ring, or in neither (a reader holds it between the pop and the push
back).  The `tracking` word is separate from that.  It holds the stream of
the request while the request owns the slot, and 0 after a claim or a
retraction (`src/nxt_app_queue.h:113`, `:176`, `:231`).  A stream is never 0
(`src/nxt_port_rpc.c:185`).

The router keeps the only record of a retraction: `nxt_msg_info_t.cancel`
(`src/nxt_router_request.h:13-34`).  A win and a loss of the retraction both
leave the word at 0, so the router asks once.  The worker keeps the only
record of a claim: the answer of its own compare-and-set
(`src/nxt_unit.c:9448`).  Nothing in shared memory says which side won, and
nothing says which worker.

### 2.2 Every transition

The table lists every place that changes who owns a request or a slot.  The
"ack" column says whether the router knows the worker at that point.

| # | Transition | Actor | Mechanism | Where |
|---|---|---|---|---|
| T1 | slot free -> queued | router | pop `free_items`; store the stream in `tracking`; push `queue` | `src/nxt_app_queue.h:104`, `:113`, `:119`, from `src/nxt_router.c:8744` |
| T1f | enqueue failed -> free | router | push `free_items` | `src/nxt_app_queue.h:122` |
| T2 | queued -> dequeued | worker | pop `queue`; the word does not change | `src/nxt_app_queue.h:201`, from `src/nxt_unit.c:9442` |
| T3 | dequeued -> claimed | worker | CAS `tracking`: stream -> 0, before the push to `free_items` (#889) | `src/nxt_app_queue.h:236`; answer at `src/nxt_unit.c:9448` |
| T3l | claim lost -> dropped | worker | the message is dropped; the worker reads the next one | `src/nxt_unit.c:9468-9470` |
| T4 | queued or dequeued -> retracted | router | the same CAS: stream -> 0; the answer is memoised | `src/nxt_app_queue.h:176` via `src/nxt_router.c:1364-1402` (`:1383`, `:1386`, `:1395`) |
| T5 | claimed or retracted -> free | worker | push `free_items`; clear the one-reader doorbell when the queue is empty | `src/nxt_app_queue.h:248`, `:248` |
| T6 | doorbell | router | `READ_QUEUE` to the shared port, bounded by queue position; a worker's own port gets a plain write | `src/nxt_router.c:8755-8770`; after a worker closes with a non-empty queue, `:8185-8189` |
| T6r | doorbell consumed | worker | reads the datagram, clears the legacy flag, re-reads the queue | `src/nxt_unit.c:9292-9297`; `src/nxt_app_queue.h:164` |
| T7 | claimed -> acknowledged | worker | `REQ_HEADERS_ACK` from the context's read port, once the reply port is ready | `src/nxt_unit.c:2269-2295` (`:2286`), sent at `:2017` or, after `NEW_PORT`, at `:7078` |
| T7r | acknowledged (router side) | router | sets the RPC peer; unparks from `ack_waiting_req`; `app_port` becomes the worker's port; `active_requests++`; sends the body; arms `timeout` | `src/nxt_router.c:6769-6881`: `:6783`, `:6791`, `:6827`, `:6811`, `:6846`, `:6878` |
| T8 | claimed -> failed before the ack | worker | `RPC_ERROR` when the reply port is known and ready; else a silent release | `src/nxt_unit.c:2319-2378` (`:2366`); silent at `:2011`; callers `:1383`, `:2061` |
| T8r | failed (router side) | router | `RPC_ERROR` handler answers 503 and unlinks | `src/nxt_router.c:6904-6929` (`:6927`) |
| T9 | acknowledged -> completed | worker | `DATA` with `last`, or `RPC_ERROR` | `src/nxt_unit.c:4980-5033` (`:5028`) |
| T9r | completed (router side) | router | response handler; unlink; port released | `src/nxt_router.c:6438` |
| T10 | parked | router | links the request in `ack_waiting_req`; retains the pool; holds an application reference | `src/nxt_router.c:8491`, `:8498`; `src/nxt_router.h:278-284` |
| T11 | parked -> failed, no process | router | when `processes == 0` and none pending, each parked request gets `err_work`: 503, unlink (the unlink retracts, T4) | `src/nxt_router.c:7430-7470` (`:7443`), `:8606-8620` (`:8614`) |
| T12 | parked -> expired | router | `nxt_router_app_timeout()`: retract (T4); if claimed, alert at the first expiry and 503 at the second; else 503 | `src/nxt_router.c:9217-9339` (`:9279`, `:9291`, `:9306`, `:9339`); armed at `:8527` only when `timeout != 0` |
| T13 | worker died, acknowledged request | router | `REMOVE_PID` -> `nxt_port_rpc_remove_peer()` -> `RPC_ERROR` to the request's handler -> 503 | `src/nxt_router.c:2432-2470`; `src/nxt_port_rpc.c:495`; peer set only at T7r (`:6783`, `src/nxt_port_rpc.c:307`) |
| T14 | worker died, port closed | router | removes the port from the hash, `processes--`, rings `READ_QUEUE` if the queue is not empty.  Does not look at `ack_waiting_req` | `src/nxt_router.c:8078-8200` (`:8106`, `:8130`, `:8185`) |
| T15 | quit | router -> worker | `QUIT`; the worker fails every active request (close handler or `RPC_ERROR`) | `src/nxt_router.c:8303`, `:8402` (prototype); `src/nxt_unit.c:8384-8455` (`:8444`) |
| T16 | headers chunk | both | the chunk is the worker's once `is_port_mmap_sent` is set; a lost retraction keeps the flag | `src/nxt_router.c:8789`, `:1426` |
| T17 | out of shared memory | worker | sets `hdr->oosm`, sends `OOSM`, blocks in recv for `SHM_ACK` | `src/nxt_unit.c:5299`, `:5402` |
| T17r | shared memory freed | router | clears `oosm` with a CAS and broadcasts `SHM_ACK` to every port of the process; skips a closed port (#886); also on `OOSM` when a free chunk exists | `src/nxt_port_memory.c:241-244`, `:1144-1178` (`:1168`, `:1174`); `src/nxt_router.c:9680`, `:9749` |
| T18 | the other direction of T17 | router sets, worker clears and sends `SHM_ACK` | `src/nxt_port_memory.c:632`; `src/nxt_unit.c:6559`, `:6567` |

T6, T17 and T18 do not change ownership.  They are in the table because the
fixes in section 3 touch them, and because a wake or an acknowledgement that
goes to the wrong process is how an ownership bug shows.

### 2.3 What the router can and cannot see

For a parked request the router has: the cookie, the stream, and
`msg_info.cancel`.  From the word it can tell "still mine" (word == stream)
from "taken" (word != stream).  It cannot tell:

- a slot that a worker dequeued and has not claimed (T2 done, T3 not) from
  one that is still in the ring: the word is the stream in both;
- a claim from a retraction after the fact: both leave 0;
- which worker claimed.

## 3. The race windows

Each window is a missing ordering between two transitions of section 2.2.

### 3.1 Closed by #889: free before claim (T5 before T3)

Before #889 the worker pushed the slot to `free_items` (T5) and then ran the
claim CAS (T3) from `nxt_unit.c`.  Between the two, the router could pop the
same slot for a new request (T1) and store a new stream.  Then the worker's
CAS compared the old stream, failed, and dropped the request (T3l).  The
router's later retraction (T4) failed too, so it recorded `NXT_MSG_CLAIMED`
and waited for an acknowledgement that no worker would send.  #889 runs T3
before T5 (`src/nxt_app_queue.h:236` before `:235`).  The fix needs the
router to cycle through the whole free ring while the worker sits between
the two steps, so it is rare, but nothing else prevents it.

What stays: a reader that uses `nxt_app_queue_recv()` with no claim
(`src/nxt_app_queue.h:269`, the unit tests) still frees with the stream in
the word, and so does a libunit from before #889.  That is why T1 is a store
and not a CAS (section 8).

### 3.2 Closed in part by #890: a claim with no acknowledgement (T3 without T7)

A worker that dies or stalls after T3 and before T7 leaves the request
parked for ever: `REMOVE_PID` (T13) finds only requests whose peer was set at
T7r, and `nxt_router_app_port_close()` (T14) does not look at
`ack_waiting_req`.  With `"limits": {"timeout"}` at 0, the default, no timer
runs (T12 is armed at `src/nxt_router.c:8527` only when `timeout != 0`).  The
parked request holds an application reference (T10), so
`nxt_router_free_app()` never sends `QUIT` to the prototype
(`src/nxt_router.c:8402`).  This is the leftover process of #665.

#890 arms the timer at dispatch in every case, with `NXT_APP_ACK_TIMEOUT`
(5 s) when `timeout` is 0.  At each expiry it reads the word without taking
it (`nxt_app_queue_queued()`): word == stream means "still waiting for
capacity", so the timer is re-armed and the request stays; anything else
means a worker took the request, and the claimed path of T12 applies (an
alert, then 503 at the next expiry).  Attribution is by elimination.

What stays, and what #890 cannot see:

- **W2b, shown by the #665 log**: a death between T2 and T3.  The word is
  still the stream, so the router sees "waiting for capacity" and re-arms
  for ever.  The #665 comment of 2026-11-02 shows a worker whose last line
  was `app_queue_recv: 28`, killed before its CAS.  #889 shrinks this window
  to a few instructions; it does not close it.
- **W5, shown by reading, no test**: the same death leaks the slot.  The
  index was popped from `queue` (`src/nxt_app_queue.h:201`) and never pushed
  to `free_items` (`:235`).  Nobody frees it.  Each such death removes one
  of 131072 slots for the life of the application.

### 3.3 Closed by #886: `SHM_ACK` to a closed port (T17r after T14)

A port of a dead process stays in `process->ports` while a queued chunk
holds a reference.  `nxt_port_close()` sets `pair[1]` to -1 and keeps the
old number in `socket.fd`.  The broadcast walked every port and wrote to
that number: `EBADF`, or 16 bytes into a new worker that got the same
number, which then quit with "too many port socket messages".  #886 skips a
port whose `pair[1]` is -1 (`src/nxt_port_memory.c:1168`).

The missing ordering: the port object outlives the socket, and the broadcast
read the first state as if it were the second.  The same shape, a reference
that outlives the thing it points to, is W5 above.

### 3.4 Other windows

- **W6, suspected, documented in code**: a late acknowledgement after the
  second expiry of T12.  The router unlinked the request; the ack finds no
  handler and is dropped.  The worker runs the request.  If the body did not
  fit in the headers message, the worker waits for a body that never comes,
  until the next `QUIT` (T15).  The comment at `src/nxt_router.c:9265-9269`
  states this.  The worker holds a request and its memory for that time.  No
  test shows it.
- **W7, shown by #665**: a pre-acknowledgement failure when the reply port
  is not ready: libunit releases the request and sends nothing
  (`src/nxt_unit.c:2011`).  Only the deadline of #890 answers it.
- **W8, suspected**: a worker claims a request, asks for the reply port
  (`GET_PORT`, `src/nxt_unit.c:2239`), and the router refuses without a
  reply (the #665 step 4).  The worker parks the request in `awaiting_req`
  for ever.  With #890 the router answers 503 after about 10 s, while the
  worker still holds the request and its headers chunk.  Same shape as W6.

## 4. The proposed state machine

### 4.1 States

A slot has one state at a time.  The state is the pair (where the index is,
what the word holds).  The router-side records (`ack_waiting_req`,
`msg_info.cancel`, the RPC peer) stay, but they follow the slot; they do not
define ownership.

| State | Index is in | Word holds | Owner |
|---|---|---|---|
| FREE | `free_items` | 0 (see 4.4 for old peers) | router: only the router pops `free_items` |
| QUEUED | `queue` | stream | shared: the router may retract, a worker may dequeue; the CAS arbitrates |
| DEQUEUED | no ring | stream | the worker that popped it; the router can find it (4.3) |
| CLAIMED | no ring | pid of the claimant | the worker |
| ACKNOWLEDGED | `free_items` | 0 | the worker, through its own port; the slot is free |
| COMPLETED, CANCELLED | `free_items` | 0 | router (the unlink) |

Two changes from today, both in libunit:

1. the claim CAS writes the claimant's pid, not 0: stream -> pid;
2. the worker frees the slot after it sent `REQ_HEADERS_ACK` (or
   `RPC_ERROR`), not at the claim.  Until then the word holds the pid.

The retraction stays a CAS stream -> 0.  Both sides still run one CAS on one
word, so exactly one wins.

### 4.2 Transitions

| Transition | Single owner | Mechanism | Today |
|---|---|---|---|
| FREE -> QUEUED | router | pop `free_items`; CAS word 0 -> stream (a store until 4.4); push `queue` | T1 |
| QUEUED -> DEQUEUED | worker | pop `queue` | T2 |
| DEQUEUED -> CLAIMED | worker | CAS word stream -> pid | T3 (writes 0) |
| QUEUED or DEQUEUED -> CANCELLED | router | CAS word stream -> 0.  If the index is in `queue`, a worker pops it, its claim fails, and it frees the slot.  If the index is in no ring (4.3), the router frees it | T4, then T5 by the worker |
| CLAIMED -> ACKNOWLEDGED | worker | `REQ_HEADERS_ACK`; then word -> 0; push `free_items` | T7; the free moves from T3 to after T7 |
| CLAIMED -> CANCELLED (failed before the ack) | worker | `RPC_ERROR`, or nothing when no port is ready; then word -> 0; push `free_items` | T8 |
| CLAIMED -> CANCELLED (worker died) | router | at `REMOVE_PID`, for each parked request whose word == the dead pid: 503, word -> 0, push `free_items` | none |
| ACKNOWLEDGED -> COMPLETED | worker | `DATA` with `last`, or `RPC_ERROR` | T9 |
| ACKNOWLEDGED -> CANCELLED | router | `timeout`, client close, `REMOVE_PID` through the RPC peer | T12, T13 |

### 4.3 Worker death in each state

| State at death | Who sees it | What happens |
|---|---|---|
| FREE | nobody needs to | nothing |
| QUEUED | another worker, or the router | another worker pops it.  If it was the last worker, the router starts one or fails the parked requests (T11), as today |
| DEQUEUED | the router, at the ack deadline | the word is the stream and the index is in no ring.  The router scans `queue` for the cookie between `head` and `tail`.  Not there: it retracts (the CAS wins, nobody else can), answers 503, and pushes the index to `free_items`.  This closes W2b and W5 |
| CLAIMED | the router, at `REMOVE_PID` | the word is the dead pid: 503, word -> 0, push `free_items`.  For an old libunit the word is 0 (4.4): the elimination path of #890 applies |
| ACKNOWLEDGED | the router, at `REMOVE_PID` | the RPC peer is set: `RPC_ERROR` -> 503, as today (T13) |

The scan in the DEQUEUED row reads a lock-free ring without a lock.  A stale
snapshot is safe: the only action is the retraction CAS, which a live worker
races as today, and one side wins.  The scan is O(depth) per request and
runs only at a deadline, never on the fast path.  Its cost under a full
queue is an open question (section 7).

The `REMOVE_PID` sweep reads the word of every parked request of the
application.  A parked request is one that a worker has not acknowledged, so
the sweep is short in the normal case.

### 4.4 Why the word encoding stays compatible

The word gains one class of value, a pid, where today there is 0:

- an old router (before this design) retracts with a CAS stream -> 0.  It
  fails on a pid, as it fails on 0 today, and records `NXT_MSG_CLAIMED`.
  Its queued check (#890) compares with the stream: a pid is "taken".
  Correct in both cases;
- an old libunit writes 0 at the claim and frees at once.  A new router
  reads 0: "taken, claimant unknown", and uses the elimination path of #890.
  Degraded, not broken;
- a pid can equal a stream number.  The router compares the word of a parked
  request with that request's own stream first.  A different value is a pid
  only if the slot still belongs to this request, which holds while the
  worker keeps the slot until the ack.  An old libunit frees at the claim,
  so the slot can be reused and the word can be a new stream; the new router
  then checks the value against its process table before it trusts it as a
  pid, and otherwise falls back to elimination.

The struct layout does not change.  `nxt_app_queue_item_t` and
`nxt_app_queue_t` are `shm` entries in `tools/perf/layout-structs.txt`, and
every step keeps them as they are.

### 4.5 Messages

The design adds no message.  The claim is visible in the word, and the
acknowledgement is the message it is today.  A `CLAIM` datagram from the
worker to the router port (the port libunit always has, the one `SHM_ACK`
uses, `src/nxt_unit.c:6567`) would do the same with one more datagram per
request.  The #665 follow-up comment rejected it for that cost.  Section 7
says what measurement would reopen that choice.

## 5. Migration in steps

Each step is one pull request, behind the existing queue API
(`nxt_app_queue_send_bound()`, `nxt_app_queue_recv_claim()`,
`nxt_app_queue_cancel()`), and each works with the other side at any older
step.

| Step | Side | Change | Compatible with |
|---|---|---|---|
| 1 (this PR) | header, tests | named slot states; one function per transition; debug checks; tests.  No behaviour change, no message | everything |
| 2 | libunit | free the slot after the ack or the pre-ack failure, not at the claim; write 0 before the push; the claim CAS writes the pid | any router: it never frees a slot and treats a non-stream word as "taken" |
| 3 | router | at `REMOVE_PID`, sweep `ack_waiting_req` by pid; at the ack deadline of #890, the ring scan for a DEQUEUED death; the router frees those slots | any libunit: an old one gives 0, and the #890 elimination path stays |
| 4 | router | FREE -> QUEUED becomes a CAS 0 -> stream; `nxt_app_queue_recv()` without a claim goes away; the elimination path goes away | a libunit at step 2 or later.  Needs the protocol number (5.1) |
| 5, optional | both | a `CLAIM` message instead of the pid in the word, only if the benchmark of section 7 says the cost is acceptable | a new protocol number |

Steps 2 and 3 are independent of each other and of #805.  #890 should land
before step 3, because step 3 extends its deadline handler.  #702 (queue
wrap arithmetic) should land before step 2.

### 5.1 libunit and module compatibility, and #805

A module that links libunit statically (Go, snunit) keeps the libunit of its
own build.  Steps 1 to 3 keep such a module working against a 1.38 router,
with the elimination path instead of the pid.  A 1.38 module against an
older router works too: the old router never reads a pid.

Step 4 needs every reader to free its slot with 0 in the word.  A libunit
from before #889 frees with the stream in it (section 3.1), so a CAS 0 ->
stream would refuse those slots.  That is a protocol change.  #805 adds
`NXT_UNIT_PROTOCOL_VERSION` (1) to `NXT_UNIT_INIT` and makes libunit refuse
a daemon with another number.  Step 4 bumps the number to 2.  Then a
pre-#889 libunit refuses a step-4 daemon at start instead of losing
requests, which is the behaviour #805 chose for every shared-memory
protocol change.  Until #805 merges, step 4 waits.

`NXT_UNIT_PROTOCOL_VERSION` is a number for the shared-memory and queue
protocol as a whole.  It is not a capability bit.  Steps 2 and 3 do not bump
it, because an old peer stays correct with them.

## 6. The model repository

The freeunit-model repository models the queue in
`src/unit_model/engine/ipc_queue.py` with the states `UNQUEUED`, `QUEUED`,
`SOCKET` and `DELIVERED`, and a TLA+ specification in
`formal/tla/FreeUnitIPC.tla`.  Its `recv()` frees the slot at the dequeue
and has no claim CAS, so it cannot show W2b, W5 or the #889 window.  The
state machine of section 4 is the one to add there: the word as a CAS
variable with the values 0, stream and pid, the index in one of three
places, and a worker death as an action in every state.  The invariants to
check: every request that the router parked is answered or acknowledged,
and every slot index is in exactly one ring once no worker holds it.

## 7. Open questions

**Memory peak under load.**  Partly answered.  The slot pool is fixed:
`sizeof(nxt_app_queue_t)` is 5767188 bytes per application, whatever the
load (131072 slots of 36 bytes plus two rings).  Request bodies are not in
slots; they are in port mmap chunks, bounded by the `"shm"` limit and the
65536 mmaps limit.  Step 2 holds a slot from the claim to the ack instead of
freeing it at the claim.  That is microseconds, or the length of one
`GET_PORT` round trip, so the pool does not run out of slots for that
reason.  What stays open is the peak of the mmap chunks, which this design
does not change.  The measurement: RSS of the router and of the workers
with 1000 and 10000 concurrent POST requests with 1 MiB bodies, `master`
against the branch, and the count of `too many port mmaps` alerts.

**Migration path.**  Answered in section 5.

**`EPOLLEXCLUSIVE` or a targeted wake.**  Answered.  The worker wake is not
an epoll wake.  `READ_QUEUE` is a datagram on the shared port socket, and
the kernel gives a datagram to one reader.  So one doorbell wakes one idle
worker, and the router rings up to `depth_bound` doorbells for a burst
(`src/nxt_app_queue.h:74-78`, `src/nxt_router.c:8755-8770`).  There is no
herd to exclude.  A targeted wake of a known idle worker would need the
router to track idle readers of the shared queue, which it does not, and
the datagram already does the targeting.  `EPOLLEXCLUSIVE` is set on the
router's listen sockets (`nxt_epoll_enable_accept()`,
`src/nxt_epoll_engine.c:654-656`), where several router threads wait on one
socket.  It is out of this design.  The measurement that
would show a problem: `queue__enqueue`, `queue__no_doorbell` and
`queue__dequeue` (`docs/usdt.md`) under a burst, against the number of
workers.  More doorbells than idle workers is the only waste.

**Message cost against the fast path.**  Open.  Today a request costs one
slot write, at most one `READ_QUEUE` (coalesced), one `REQ_HEADERS_ACK`
and one `DATA` or `RPC_ERROR`.  Sections 4 and 5 add no datagram.  Step 5
would add one.  The benchmark: a trivial application, `wrk` at a fixed
connection count, `master` against the branch, with the `port__send` and
`port__recv` counts and the p50 and p99 latency.  The two things to look
at in steps 2 and 3 are the extra store at the free (word -> 0) and the ack
that now precedes the free; both are on the worker, not on the router.

**For the maintainer**:

1. The pid in the word (4.4) or a `CLAIM` message (4.5)?  This note
   recommends the pid: no new message, compatible both ways, and the
   `REMOVE_PID` sweep is then by identity.
2. The ring scan at the deadline (4.3): acceptable as a deadline-only cost,
   or does it need a bound (scan at most N entries, then fall back to
   elimination)?
3. Step 4 and the protocol number: bump to 2 at step 4, or treat steps 2 to
   4 as one 1.38 protocol change and bump once with #805?

## 8. Step 1: this pull request

### 8.1 What it changes

`src/nxt_app_queue.h`:

- `NXT_APP_QUEUE_TRACKING_IDLE` (0) names the word value that means "no
  request owns the slot".
- `nxt_app_queue_slot_state_t` names what the word can tell:
  `NXT_APP_QUEUE_SLOT_QUEUED` (word == stream) and
  `NXT_APP_QUEUE_SLOT_IDLE`.  `nxt_app_queue_slot_state()` reads it.
- One function per transition on the word or the free ring:
  `nxt_app_queue_slot_queue()` (T1), `nxt_app_queue_slot_claim()` (T3),
  `nxt_app_queue_slot_retract()` (T4), both over the single CAS
  `nxt_app_queue_slot_take()`, and `nxt_app_queue_slot_free()` (T5, T1f).
  `nxt_app_queue_send_bound()`, `nxt_app_queue_cancel()` and
  `nxt_app_queue_recv_claim()` call them.  The generated code is the same
  store, the same CAS and the same ring operations as before.
- `nxt_app_queue_assert()`: a debug check that aborts.  libunit does not link
  the thread context that `nxt_assert()` needs (`src/nxt_unit.c:4536`), and
  this header is part of libunit.  The checks are on the caller's own
  values: the slot index, and a stream the router made, which is never 0.

`src/test/nxt_app_queue_test.c`:

- "app queue slot states" drives every legal transition and checks that the
  illegal ones fail and change nothing: a retraction or a claim with the
  wrong stream; a retraction after a claim; a claim after a retraction; a
  second retraction.  It also shows the no-claim free of section 3.1.
- "app queue take race": eight threads run the one CAS on one queued slot;
  exactly one wins each of 2000 rounds, and the slot is idle after it.

Two tests sent 0 as the tracking value (`src/test/nxt_port_doorbell_test.c`,
and the race workers in `src/test/nxt_app_queue_test.c`).  They send
`i + 1` now, because 0 is the idle value and the debug check refuses it.

### 8.2 What it does not change

- No shared struct changes.  `sizeof` and `offsetof` of
  `nxt_app_queue_item_t`, `nxt_app_queue_t` and `nxt_app_nncq_t` are the
  same at the base and at the head.
- No message changes, and no message is added.
- No behaviour changes in the router or in libunit.  T1 stays a store
  (section 3.1 says why); the claim still writes 0; the free still follows
  the claim at once.
- `nxt_app_queue_slot_claim()` does not check the stream against 0.  The
  worker reads the stream from shared memory that a sibling process can
  write, and a debug abort on a poisoned item would be new behaviour.
- "ack" is not a slot transition today, so there is no function for it.
  Step 2 adds the free after the ack.

### 8.3 What it does not do, and why

The issue asks for every transition to be a single CAS that checks the
expected previous state.  T1 cannot be one without a behaviour change: a
slot on `free_items` can hold a stale stream (section 3.1).  T5 cannot check
the word either, for the same reason, and because a poisoned item can leave
the stream in it after a failed claim.  The note says where each becomes a
CAS: T1 at step 4, T5 at step 2.

With #890, `nxt_app_queue_queued()` is the same read as
`nxt_app_queue_slot_state()`.  After the rebase it becomes a wrapper over
it.
