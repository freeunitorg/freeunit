"""Self-identifying request bodies for the shared-memory teardown tests.

Why not ``'x' * SIZE``: test_process_teardown_churn.py and
test_process_abrupt_teardown.py exist to notice the router serving freed or
recycled shared memory, and the most likely shape of that is a 16 KiB chunk
(PORT_MMAP_CHUNK_SIZE, src/nxt_port_memory_int.h) belonging to a *different*
concurrent request turning up in this one.  Against a uniform body such a
chunk is byte-identical to the bytes that belong there, so the integrity
check cannot see it at all: it only ever catches garbage that happens not to
be 'x'.  Both tests run several request threads on purpose, so a cross-request
chunk is exactly the corruption they are best placed to catch and exactly the
one a uniform body hides.

The body is a run of fixed-width records, each stamped with the request's own
tag and with its own byte offset:

    | tag: 8 hex digits | offset: 8 hex digits | 48 filler letters |

so every 64 bytes of the payload say both *which* request the bytes came from
and *where in it* they belong.  A chunk swapped in from another request
mismatches on the tag; a chunk from another offset of the same request
mismatches on the offset field; a shift of less than one record mismatches on
the filler letter, which advances with the record index.  Byte-wise, that
means any splice at a chunk boundary is wrong within at most 64 bytes of the
splice, and the record header at the first wrong byte names the culprit.

Cost matters - these tests send 64 to 100 bodies of 1 MiB each and are meant
to stay at a few seconds - so the offset skeleton is built once per size at
import time and a per-request body is a single str.replace() of the tag
placeholder: one C-speed pass over 1 MiB instead of a Python loop.
"""

import itertools

# One record per 64 bytes: short enough that a corrupt region is localised to
# within 64 bytes of where it starts, long enough that a 1 MiB body is 16384
# records rather than ten times that.
RECORD_SIZE = 64

TAG_LEN = 8
OFFSET_LEN = 8

_FILLER_LEN = RECORD_SIZE - TAG_LEN - OFFSET_LEN

_LETTERS = 'abcdefghijklmnopqrstuvwxyz'

# Tags are hex digits, so '~' cannot occur in a finished body and replacing
# the placeholder cannot corrupt payload that happens to look like it.
_PLACEHOLDER = '~' * TAG_LEN

_counter = itertools.count(1)

_skeletons = {}


def _skeleton(size):
    if size % RECORD_SIZE:
        raise ValueError(f'size must be a multiple of {RECORD_SIZE}')

    if size not in _skeletons:
        _skeletons[size] = ''.join(
            _PLACEHOLDER
            + f'{offset:0{OFFSET_LEN}X}'
            + _LETTERS[(offset // RECORD_SIZE) % len(_LETTERS)] * _FILLER_LEN
            for offset in range(0, size, RECORD_SIZE)
        )

    return _skeletons[size]


def make_body(size):
    """A body of `size` bytes, distinguishable from every other body this
    process has handed out.

    next() on an itertools.count is a single bytecode under the GIL, so
    request threads can call this without a lock, and consecutive tags always
    differ in at least one hex digit - which is all the byte-wise comparison
    in the tests needs to tell two in-flight requests apart.
    """

    return _skeleton(size).replace(_PLACEHOLDER, f'{next(_counter):08X}')


def describe_mismatch(body, expected):
    """One line saying how `body` differs from `expected`, for a failure
    message.

    Reports the first wrong byte and the record header found there against
    the header that belongs there, which is what separates "a chunk from
    another request" (tag differs) from "a chunk from another offset of this
    request" (offset differs) from plain garbage (neither parses).
    """

    # next() needs a default.  zip() stops at the shorter of the two, so a
    # body that is a correct prefix but LONGER than the request - which still
    # fails the caller's `body != expected[:len(body)]` test, since that
    # slice clamps to `expected` - has no mismatching byte to find here.  A
    # bare next() would raise StopIteration out of the caller's request loop
    # and kill the thread instead of recording the corruption: the detector
    # would fail open on one of the shapes it exists to catch.
    at = next(
        (i for i, (c, e) in enumerate(zip(body, expected)) if c != e), None
    )

    if at is None:
        return f'correct prefix but longer than {len(expected)} bytes'

    return (
        f'first mismatch at {at} ({body[at]!r} != {expected[at]!r}): '
        f'got {_record_header(body, at)}, '
        f'want {_record_header(expected, at)}'
    )


def _record_header(text, at):
    """The tag/offset stamp of the record containing byte `at`."""

    start = at - at % RECORD_SIZE
    head = text[start : start + TAG_LEN + OFFSET_LEN]

    if len(head) < TAG_LEN + OFFSET_LEN:
        return f'<truncated record at {start}>'

    return f'tag={head[:TAG_LEN]} offset={head[TAG_LEN:]}'
