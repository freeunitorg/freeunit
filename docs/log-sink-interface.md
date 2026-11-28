# Log sink interface

The access log hands each finished record to a sink. The sink is the only
place that knows how a record reaches its destination; the writer does not.

This is the first part of the 1.38 work from freeunit-exp issue 77: the
interface and the built-in file sink. No configuration syntax, no module
loader and no syslog sink are in this change. Until they land, the access log
uses the built-in file sink, and its behaviour is unchanged.

## The interface

`src/nxt_log_sink.h` defines `nxt_log_sink_t`, a table with a version, a name,
`open`, `close`, `write` and `flush`. A future module sink exports one symbol,
`nxt_log_sink()`, whose table the loader checks with `NXT_LOG_SINK_VERSION`;
the shape follows the TLS backend table the tree already uses for its two
implementations. The built-in file sink is an ordinary instance of the same
table, so the writer has one path for every destination.

The `write` entry takes one complete record, newline included, and returns the
number of bytes accepted:

- the whole length: the record is in the destination;
- a short count: the destination stopped early, and the caller repairs the
  torn line before the next record and counts the record lost;
- 0: nothing was accepted, and the caller counts the record lost.

Every count other than the whole length carries a defined errno. The built-in
file sink reports `EIO` when it stops without a system call error, a zero-byte
write or an exhausted retry budget, so the caller's warning and loss marker
can name a reason.

A sink must not block indefinitely, must not call back into the access log,
and must not keep the record buffer after the call returns.

The interface differs from the sketch in the design note in one place. The
note says the write returns `-1` on failure and must never write partially.
The tree's writer needs the partial count to know whether a line is torn and
must repair it, so the interface returns the accepted count and leaves errno
as the reason. The whole-record-or-not invariant is still enforced by the
caller: a short count loses the record.

## The built-in file sink

`src/nxt_log_file_sink.c` carries the existing write loop unchanged: at most
three `write()` calls per record, a short write completed with the same
buffer, an `EINTR` not counted as one of the three. The main process still
opens the descriptor and passes it to the router; the sink context is the
access log's `fd` field, so the reopen's `dup2()` is seen by the sink without
a second copy of the descriptor.

The fault hooks for the access log test stay under `#if (NXT_TESTS)` and now
live in the sink, where the `write()` calls they fake are made.

## Still open

The note leaves two decisions open, and this change does not take them:

- the configuration shape for a sink and what the validator refuses;
- what the loader does when a module's version does not match (refuse the
  configuration, or keep the built-in file).

The syslog module is the next step after the loader, because it is the
simplest sink that is not a file and the error log already has a syslog path
for `ALERT`.
