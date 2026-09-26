
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_UNIT_SPTR_H_INCLUDED_
#define _NXT_UNIT_SPTR_H_INCLUDED_


#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "nxt_unit_typedefs.h"


/* Serialized pointer. */
union nxt_unit_sptr_u {
    uint8_t   base[1];
    uint32_t  offset;
};


static inline void
nxt_unit_sptr_set(nxt_unit_sptr_t *sptr, void *ptr)
{
    sptr->offset = (uint8_t *) ptr - sptr->base;
}


static inline void *
nxt_unit_sptr_get(nxt_unit_sptr_t *sptr)
{
    return sptr->base + sptr->offset;
}


/*
 * Resolves an sptr field of a peer-supplied buffer, returning the pointer
 * when the [length]-byte range it addresses is wholly inside the buffer and
 * NULL otherwise.  Shared by both directions of the trust boundary: libunit
 * vets every sptr of an nxt_unit_request_t before the application sees it
 * (src/nxt_unit.c), and the router vets an application's nxt_unit_response_t
 * (src/nxt_router.c).
 *
 * sptr->base aliases the address of the sptr itself (the union encodes an
 * offset relative to that location), so this also implicitly checks that
 * the sptr is inside the buffer.  The offset is read once: the buffer may be
 * shared memory the peer keeps writing, so it is read through volatile and
 * the caller uses the returned pointer rather than resolving the sptr again.
 */
static inline void *
nxt_unit_sptr_in_buf(nxt_unit_sptr_t *sptr, uint32_t length,
    void *buf_start, uint32_t buf_size)
{
    size_t    sptr_off;
    uint32_t  offset;

    if ((uint8_t *) sptr < (uint8_t *) buf_start) {
        return NULL;
    }

    sptr_off = (uint8_t *) sptr - (uint8_t *) buf_start;

    /*
     * The sptr itself must fit inside the buffer before its offset is read;
     * the first test keeps the subtraction from underflowing.
     */
    if (buf_size < sizeof(nxt_unit_sptr_t)
        || sptr_off > buf_size - sizeof(nxt_unit_sptr_t))
    {
        return NULL;
    }

    offset = *(volatile uint32_t *) &sptr->offset;

    if (offset > buf_size - sptr_off
        || length > buf_size - sptr_off - offset)
    {
        return NULL;
    }

    return sptr->base + offset;
}


#endif /* _NXT_UNIT_SPTR_H_INCLUDED_ */
