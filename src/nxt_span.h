
/*
 * Copyright (C) FreeUnit Community
 */

#ifndef _NXT_SPAN_H_INCLUDED_
#define _NXT_SPAN_H_INCLUDED_

#include <nxt_checked.h>


/*
 * nxt_span_t is a bounded read cursor over a buffer.  "pos" is the next
 * byte to consume.  "end" is one past the last valid byte.  It is for
 * parsers that walk untrusted, length-prefixed input, such as shared
 * memory port messages and libunit request buffers.  Such a parser must
 * check each advance against the remaining bytes.  It must not trust a
 * length field that comes from the peer.
 *
 * A short read means that not enough bytes are left.  A partial tail means
 * that the span ends in the middle of a whole record.  Both are reported
 * as a failure.  nxt_span_take() never returns a pointer that reads past
 * "end".
 */

typedef struct {
    const u_char  *pos;
    const u_char  *end;
} nxt_span_t;


nxt_inline void
nxt_span_init(nxt_span_t *span, const u_char *start, const u_char *end)
{
    span->pos = start;
    span->end = end;
}


nxt_inline size_t
nxt_span_len(const nxt_span_t *span)
{
    return (size_t) (span->end - span->pos);
}


/*
 * Takes "size" bytes off the front of the span.  Returns a pointer to them
 * in "out".  Returns 1 when fewer than "size" bytes remain.  This covers a
 * short buffer.  It also covers a partial tail that is left after whole
 * records were taken.  On failure, "span" and "out" do not change.
 */

nxt_inline int
nxt_span_take(nxt_span_t *span, size_t size, const u_char **out)
{
    size_t  avail;

    avail = nxt_span_len(span);

    if (nxt_slow_path(size > avail)) {
        return 1;
    }

    *out = span->pos;
    span->pos += size;

    return 0;
}


/*
 * Copies "size" bytes off the front of the span into "dst".  It fails in
 * the same way as nxt_span_take().  On a short or partial tail, the span
 * and "dst" do not change.
 */

nxt_inline int
nxt_span_copy(nxt_span_t *span, void *dst, size_t size)
{
    const u_char  *src;

    if (nxt_span_take(span, size, &src) != 0) {
        return 1;
    }

    nxt_memcpy(dst, src, size);

    return 0;
}


#endif /* _NXT_SPAN_H_INCLUDED_ */
