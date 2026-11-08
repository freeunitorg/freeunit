
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_UNIT_RESPONSE_H_INCLUDED_
#define _NXT_UNIT_RESPONSE_H_INCLUDED_


#include <inttypes.h>

#include "nxt_unit_sptr.h"
#include "nxt_unit_field.h"

struct nxt_unit_response_s {
    uint64_t              content_length;
    uint32_t              fields_count;
    uint32_t              piggyback_content_length;
    uint16_t              status;

    nxt_unit_sptr_t       piggyback_content;

    nxt_unit_field_t      fields[];
};


/*
 * The response is shared memory: the router parses it where libunit wrote
 * it.  status leaves two bytes of padding before piggyback_content.
 */
nxt_unit_layout_assert(response_status,
                       offsetof(nxt_unit_response_t, status) == 16,
                       "nxt_unit_response_t: status moved");
nxt_unit_layout_assert(response_piggyback_content,
                       offsetof(nxt_unit_response_t, piggyback_content) == 20,
                       "nxt_unit_response_t: piggyback_content moved");
nxt_unit_layout_assert(response_size, sizeof(nxt_unit_response_t) == 24,
                       "nxt_unit_response_t: size changed");


#endif /* _NXT_UNIT_RESPONSE_H_INCLUDED_ */
