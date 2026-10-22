/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_USDT_H_INCLUDED_
#define _NXT_USDT_H_INCLUDED_


/*
 * USDT probes of the provider "freeunit", see docs/usdt.md.  With --usdt
 * a probe is a nop until a tracer attaches, and its arguments are always
 * evaluated: pass a local or one dereference, never a call.  Without
 * --usdt the macro is empty.  STAP_PROBEV() needs SDT_USE_VARIADIC.
 */

#if (NXT_HAVE_USDT)

#define SDT_USE_VARIADIC
#include <sys/sdt.h>

#define NXT_USDT(name, ...)  STAP_PROBEV(freeunit, name, ##__VA_ARGS__)

#else

#define NXT_USDT(name, ...)

#endif


#endif /* _NXT_USDT_H_INCLUDED_ */
