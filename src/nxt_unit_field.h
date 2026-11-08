
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_UNIT_FIELD_H_INCLUDED_
#define _NXT_UNIT_FIELD_H_INCLUDED_


#include <inttypes.h>

#include "nxt_unit_sptr.h"

enum {
    NXT_UNIT_HASH_CONTENT_LENGTH = 0x1EA0,
    NXT_UNIT_HASH_CONTENT_TYPE   = 0x5F7D,
    NXT_UNIT_HASH_COOKIE         = 0x23F2,
    NXT_UNIT_HASH_AUTHORIZATION  = 0xB29A,
};


/* Name and Value field aka HTTP header. */
struct nxt_unit_field_s {
    uint16_t              hash;
    uint8_t               skip:1;
    uint8_t               hopbyhop:1;
    uint8_t               name_length;
    uint32_t              value_length;

    nxt_unit_sptr_t       name;
    nxt_unit_sptr_t       value;
};


/*
 * The fields are shared memory: the router reads them where libunit wrote
 * them.  The offsets are pinned so that a compiler which lays out the skip
 * and hopbyhop bits differently fails the build.  The order of the two bits
 * inside their byte is not part of the check.
 */
nxt_unit_layout_assert(field_name_length,
                       offsetof(nxt_unit_field_t, name_length) == 3,
                       "nxt_unit_field_t: name_length moved");
nxt_unit_layout_assert(field_value_length,
                       offsetof(nxt_unit_field_t, value_length) == 4,
                       "nxt_unit_field_t: value_length moved");
nxt_unit_layout_assert(field_size, sizeof(nxt_unit_field_t) == 16,
                       "nxt_unit_field_t: size changed");


#endif /* _NXT_UNIT_FIELD_H_INCLUDED_ */
