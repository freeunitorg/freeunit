/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_UTF8_H_INCLUDED_
#define _NXT_UTF8_H_INCLUDED_


NXT_EXPORT u_char *nxt_utf8_encode(u_char *p, uint32_t u);
NXT_EXPORT nxt_int_t nxt_utf8_sanitize(nxt_mp_t *mp, nxt_str_t *dst,
    const nxt_str_t *src);
NXT_EXPORT uint32_t nxt_utf8_decode(const u_char **start, const u_char *end);
NXT_EXPORT ssize_t nxt_utf8_length(const u_char *p, size_t len);
NXT_EXPORT nxt_bool_t nxt_utf8_is_valid(const u_char *p, size_t len);


#endif /* _NXT_UTF8_H_INCLUDED_ */
