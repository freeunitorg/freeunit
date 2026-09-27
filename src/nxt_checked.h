/*
 * Copyright (C) FreeUnit Community
 */

#ifndef _NXT_CHECKED_H_INCLUDED_
#define _NXT_CHECKED_H_INCLUDED_


/*
 * Checked size_t arithmetic.  Each function returns 0 and stores the
 * result in "out".  When the operation overflows, the function returns 1.
 * Then the value of "out" is not defined.  Callers must not use it.
 *
 * With GCC and Clang, nxt_size_add() and nxt_size_mul() use the compiler's
 * __builtin_*_overflow() (probed in auto/clang).  Other compilers get the
 * portable functions.  The portable functions are always compiled, so
 * that the test suite covers them under every compiler.
 */

nxt_inline int
nxt_size_add_portable(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a) {
        return 1;
    }

    *out = a + b;

    return 0;
}


nxt_inline int
nxt_size_mul_portable(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a) {
        return 1;
    }

    *out = a * b;

    return 0;
}


#if (NXT_HAVE_BUILTIN_OVERFLOW)

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

#else

nxt_inline int
nxt_size_add(size_t a, size_t b, size_t *out)
{
    return nxt_size_add_portable(a, b, out);
}


nxt_inline int
nxt_size_mul(size_t a, size_t b, size_t *out)
{
    return nxt_size_mul_portable(a, b, out);
}

#endif


#endif /* _NXT_CHECKED_H_INCLUDED_ */
