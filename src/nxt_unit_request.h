
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_UNIT_REQUEST_H_INCLUDED_
#define _NXT_UNIT_REQUEST_H_INCLUDED_


#include <inttypes.h>

#include "nxt_unit_sptr.h"
#include "nxt_unit_field.h"

#define NXT_UNIT_NONE_FIELD  0xFFFFFFFFU

struct nxt_unit_request_s {
    uint8_t               method_length;
    uint8_t               version_length;
    uint8_t               remote_length;
    uint8_t               local_addr_length;
    uint8_t               local_port_length;
    uint8_t               tls;
    uint8_t               websocket_handshake;
    uint8_t               app_target;
    uint32_t              server_name_length;
    uint32_t              target_length;
    uint32_t              path_length;
    uint32_t              query_length;
    uint32_t              fields_count;

    uint32_t              content_length_field;
    uint32_t              content_type_field;
    uint32_t              cookie_field;
    uint32_t              authorization_field;

    uint64_t              content_length;

    nxt_unit_sptr_t       method;
    nxt_unit_sptr_t       version;
    nxt_unit_sptr_t       remote;
    nxt_unit_sptr_t       local_addr;
    nxt_unit_sptr_t       local_port;
    nxt_unit_sptr_t       server_name;
    nxt_unit_sptr_t       target;
    nxt_unit_sptr_t       path;
    nxt_unit_sptr_t       query;
    nxt_unit_sptr_t       preread_content;

    nxt_unit_field_t      fields[];
};


/*
 * The request is shared memory: the router parses it where libunit wrote it.
 * The 44 bytes before content_length are four-byte aligned on every target.
 * The offset of content_length then depends on the alignment of uint64_t in
 * a structure: four bytes on i386 and eight bytes on the other targets.  The
 * numbers are absolute, so a consumer that is built with another alignment
 * rule, for example gcc -malign-double on i386, fails the build.  A target
 * with another rule needs its own numbers here.
 */
nxt_unit_layout_assert(request_authorization_field,
                       offsetof(nxt_unit_request_t, authorization_field) == 40,
                       "nxt_unit_request_t: authorization_field moved");

#if defined(__i386__)

nxt_unit_layout_assert(request_content_length,
                       offsetof(nxt_unit_request_t, content_length) == 44,
                       "nxt_unit_request_t: content_length moved");
nxt_unit_layout_assert(request_size, sizeof(nxt_unit_request_t) == 92,
                       "nxt_unit_request_t: an sptr or the size moved");

#else

nxt_unit_layout_assert(request_content_length,
                       offsetof(nxt_unit_request_t, content_length) == 48,
                       "nxt_unit_request_t: content_length moved");
nxt_unit_layout_assert(request_size, sizeof(nxt_unit_request_t) == 96,
                       "nxt_unit_request_t: an sptr or the size moved");

#endif

nxt_unit_layout_assert(request_method,
                       offsetof(nxt_unit_request_t, method)
                       == offsetof(nxt_unit_request_t, content_length)
                          + sizeof(uint64_t),
                       "nxt_unit_request_t: method moved");


#endif /* _NXT_UNIT_REQUEST_H_INCLUDED_ */

