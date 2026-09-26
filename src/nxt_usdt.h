/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_USDT_H_INCLUDED_
#define _NXT_USDT_H_INCLUDED_


/*
 * Static USDT probes, provider "freeunit" (docs/observability/usdt.md).
 *
 *     NXT_USDT(port__send, stream, type);    probe "freeunit:port__send"
 *
 * With --usdt a probe is a nop until traced; without, it is nothing.
 * Arguments are evaluated whenever built with --usdt, so they must be cheap:
 * a local or one dereference, never a call or the firing pid.  Cast a
 * bit-field to a plain integer first.
 */

#if (NXT_HAVE_USDT)

#include <sys/sdt.h>

#ifndef DTRACE_PROBE0
#define DTRACE_PROBE0(provider, probe)  DTRACE_PROBE(provider, probe)
#endif

#define nxt_usdt_nargs_(_0, _1, _2, _3, _4, _5, _6, N, ...)  N
#define nxt_usdt_nargs(...)                                                 \
    nxt_usdt_nargs_(_, ##__VA_ARGS__, 6, 5, 4, 3, 2, 1, 0)

#define nxt_usdt_cat_(a, b)  a##b
#define nxt_usdt_cat(a, b)   nxt_usdt_cat_(a, b)

#define NXT_USDT(name, ...)                                                 \
    nxt_usdt_cat(DTRACE_PROBE, nxt_usdt_nargs(__VA_ARGS__))                  \
        (freeunit, name, ##__VA_ARGS__)

#else

#define NXT_USDT(name, ...)

#endif


#endif /* _NXT_USDT_H_INCLUDED_ */
