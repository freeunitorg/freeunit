
/*
 * Copyright (C) FreeUnit
 */

#ifndef _NXT_PUNYCODE_H_INCLUDED_
#define _NXT_PUNYCODE_H_INCLUDED_


/* A DNS label is at most 63 octets, the "xn--" prefix included. */
#define NXT_PUNYCODE_MAX_LABEL  63


NXT_EXPORT ssize_t nxt_punycode_encode(u_char *dst, size_t size,
    const u_char *src, size_t len);
NXT_EXPORT nxt_int_t nxt_punycode_host(nxt_mp_t *mp, nxt_str_t *host,
    const char **error);


#endif /* _NXT_PUNYCODE_H_INCLUDED_ */
