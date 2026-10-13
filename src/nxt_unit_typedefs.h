
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_UNIT_TYPEDEFS_H_INCLUDED_
#define _NXT_UNIT_TYPEDEFS_H_INCLUDED_


typedef struct nxt_unit_s                  nxt_unit_t;
typedef struct nxt_unit_ctx_s              nxt_unit_ctx_t;
typedef struct nxt_unit_port_id_s          nxt_unit_port_id_t;
typedef struct nxt_unit_port_s             nxt_unit_port_t;
typedef struct nxt_unit_buf_s              nxt_unit_buf_t;
typedef struct nxt_unit_request_info_s     nxt_unit_request_info_t;
typedef struct nxt_unit_callbacks_s        nxt_unit_callbacks_t;
typedef struct nxt_unit_init_s             nxt_unit_init_t;
typedef union nxt_unit_sptr_u              nxt_unit_sptr_t;
typedef struct nxt_unit_field_s            nxt_unit_field_t;
typedef struct nxt_unit_request_s          nxt_unit_request_t;
typedef struct nxt_unit_response_s         nxt_unit_response_t;
typedef struct nxt_unit_read_info_s        nxt_unit_read_info_t;
typedef struct nxt_unit_websocket_frame_s  nxt_unit_websocket_frame_t;


/*
 * nxt_unit_layout_assert(id, cond, msg) fails the compilation when a
 * structure that the router and libunit share through memory does not have
 * the layout they agree on.  These headers are installed, so a language
 * module or a C++ binding can compile them with a compiler other than the
 * one that built libunit.  The check turns a different bitfield order or a
 * moved member into a build error rather than a protocol error.
 *
 * nxt_clang.h has a similar fallback for nxt_static_assert(), but it is not
 * installed.  A compiler without _Static_assert() or static_assert()
 * declares an array whose size is negative when the condition does not
 * hold, and the message survives only in the type name.  That name is made
 * from id, which has to be unique in the translation unit.  A line number
 * would repeat when two headers assert on the same line.
 */

#if (defined(__cplusplus) && __cplusplus >= 201103L)

#define nxt_unit_layout_assert(id, cond, msg)                                 \
    static_assert(cond, msg)

#elif (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)

#define nxt_unit_layout_assert(id, cond, msg)                                 \
    _Static_assert(cond, msg)

#else

#define nxt_unit_layout_assert(id, cond, msg)                                 \
    typedef char nxt_unit_layout_failed_##id[(cond) ? 1 : -1]

#endif


#endif /* _NXT_UNIT_TYPEDEFS_H_INCLUDED_ */
