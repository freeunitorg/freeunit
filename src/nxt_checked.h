
/*
 * Copyright (C) FreeUnit Community
 */

#ifndef _NXT_CHECKED_H_INCLUDED_
#define _NXT_CHECKED_H_INCLUDED_


/*
 * Checked arithmetic on the compiler's __builtin_*_overflow() (GCC and
 * Clang).  Each function returns 0 and stores the result in "out".  When
 * the operation overflows, the function returns 1.  Then "out" holds the
 * wrapped result.  Callers must not use that value.
 */

nxt_inline int
nxt_size_add(size_t a, size_t b, size_t *out)
{
    return __builtin_add_overflow(a, b, out);
}


nxt_inline int
nxt_size_mul(size_t a, size_t b, size_t *out)
{
    return __builtin_mul_overflow(a, b, out);
}


#endif /* _NXT_CHECKED_H_INCLUDED_ */
