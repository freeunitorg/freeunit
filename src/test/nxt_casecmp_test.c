
/*
 * Copyright (C) FreeUnit Community
 */

/*
 * nxt_memcasecmp() and nxt_strncasecmp() fold the case of two bytes only
 * when the bytes differ.  Their results must stay those of the loops that
 * folded every byte.  The reference functions below are those loops.  They
 * have their own copy of the fold: only 'A' to 'Z' fold, and '@', '[', '`',
 * '{' and the bytes 0x80 to 0xFF do not.
 *
 * The compares run in a child.  Each string ends where a PROT_NONE page
 * starts, so a read past the length kills the child.
 */

#include <nxt_main.h>
#include "nxt_tests.h"


#define NXT_CASECMP_TEST_MAX     64
#define NXT_CASECMP_TEST_ROUNDS  100000

#define nxt_casecmp_test_lowcase(c)                                           \
    (u_char) ((c >= 'A' && c <= 'Z') ? c | 0x20 : c)

#define nxt_casecmp_test_letter(c)                                            \
    ((u_char) (((c) | 0x20) - 'a') < 26)


typedef struct {
    nxt_thread_t  *thr;
    u_char        *end1;
    u_char        *end2;
    uint32_t      seed;
} nxt_casecmp_test_ctx_t;


/* The names in nxt_h1p_fields[] of src/nxt_h1proto.c. */

static const char  *nxt_casecmp_test_names[] = {
    "Connection", "Upgrade", "Sec-WebSocket-Key", "Sec-WebSocket-Version",
    "Transfer-Encoding", "Expect", "Host", "Cookie", "Referer", "User-Agent",
    "Content-Type", "Content-Length", "Authorization", "Traceparent",
    "Tracestate",
};


/* Bytes next to the letters, and high bytes that differ only in bit 5. */

static const u_char  nxt_casecmp_test_edge[] = {
    0x00, 0x20, '-', '0', '9', '@', 'A', 'Z', '[', '`', 'a', 'z', '{', 0x7f,
    0x80, 0xa0, 0xc0, 0xc1, 0xda, 0xde, 0xe0, 0xe1, 0xfa, 0xfe, 0xff,
};


static nxt_int_t
nxt_casecmp_test_mem_ref(const u_char *s1, const u_char *s2, size_t length)
{
    u_char     c1, c2;
    nxt_int_t  n;

    while (length-- != 0) {
        c1 = *s1++;
        c2 = *s2++;

        c1 = nxt_casecmp_test_lowcase(c1);
        c2 = nxt_casecmp_test_lowcase(c2);

        n = c1 - c2;

        if (n != 0) {
            return n;
        }
    }

    return 0;
}


static nxt_int_t
nxt_casecmp_test_strn_ref(const u_char *s1, const u_char *s2, size_t length)
{
    u_char     c1, c2;
    nxt_int_t  n;

    while (length-- != 0) {
        c1 = *s1++;
        c2 = *s2++;

        c1 = nxt_casecmp_test_lowcase(c1);
        c2 = nxt_casecmp_test_lowcase(c2);

        n = c1 - c2;

        if (n != 0) {
            return n;
        }

        if (c1 == 0) {
            return 0;
        }
    }

    return 0;
}


static u_char *
nxt_casecmp_test_hex(u_char *p, const u_char *s, size_t length)
{
    static const u_char  hex[] = "0123456789abcdef";

    while (length-- != 0) {
        *p++ = hex[*s >> 4];
        *p++ = hex[*s++ & 0xf];
    }

    return p;
}


/*
 * Copies the strings to the ends of their pages and compares them with
 * both functions.  The results must be the reference results, and zero when
 * "equal" is set.
 */

static int
nxt_casecmp_test_pair(nxt_casecmp_test_ctx_t *ctx, const u_char *s1,
    const u_char *s2, size_t length, nxt_bool_t equal)
{
    u_char      *p1, *p2, *e1, *e2;
    u_char      h1[2 * NXT_CASECMP_TEST_MAX], h2[2 * NXT_CASECMP_TEST_MAX];
    nxt_int_t   r, ref;
    const char  *fn;

    p1 = ctx->end1 - length;
    p2 = ctx->end2 - length;

    nxt_memcpy(p1, s1, length);
    nxt_memcpy(p2, s2, length);

    fn = "nxt_memcasecmp";
    r = nxt_memcasecmp(p1, p2, length);
    ref = nxt_casecmp_test_mem_ref(p1, p2, length);

    if (r == ref && (!equal || r == 0)) {
        fn = "nxt_strncasecmp";
        r = nxt_strncasecmp(p1, p2, length);
        ref = nxt_casecmp_test_strn_ref(p1, p2, length);

        if (r == ref && (!equal || r == 0)) {
            return 0;
        }
    }

    e1 = nxt_casecmp_test_hex(h1, p1, length);
    e2 = nxt_casecmp_test_hex(h2, p2, length);

    nxt_log_alert(ctx->thr->log, "casecmp test: %s(%*s, %*s, %uz) gave %i, "
                  "expected %i", fn, (size_t) (e1 - h1), h1,
                  (size_t) (e2 - h2), h2, length, r, equal ? 0 : ref);

    return 1;
}


static uint32_t
nxt_casecmp_test_random(nxt_casecmp_test_ctx_t *ctx)
{
    uint32_t  x;

    /* xorshift32: the same sequence on every run and platform. */

    x = ctx->seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    ctx->seed = x;

    return x;
}


static u_char
nxt_casecmp_test_byte(nxt_casecmp_test_ctx_t *ctx)
{
    uint32_t  r;

    r = nxt_casecmp_test_random(ctx);

    switch (r & 3) {

    case 0:
    case 1:
        return 'A' + (r >> 8) % 26 + ((r >> 16) & 0x20);

    case 2:
        return nxt_casecmp_test_edge[(r >> 8)
                                     % sizeof(nxt_casecmp_test_edge)];

    default:
        return (u_char) (r >> 8);
    }
}


/* Every pair of bytes, as 1-byte strings. */

static int
nxt_casecmp_test_bytes(nxt_casecmp_test_ctx_t *ctx)
{
    u_char      c1, c2;
    nxt_uint_t  a, b;

    for (a = 0; a < 256; a++) {
        for (b = 0; b < 256; b++) {
            c1 = (u_char) a;
            c2 = (u_char) b;

            if (nxt_casecmp_test_pair(ctx, &c1, &c2, 1, 0) != 0) {
                return 1;
            }
        }
    }

    return 0;
}


/*
 * nxt_strncasecmp() stops at a NUL that both strings have, and the bytes
 * after it do not count.  nxt_memcasecmp() does not stop.
 */

static int
nxt_casecmp_test_nul(nxt_casecmp_test_ctx_t *ctx)
{
    nxt_uint_t  i;

    static const struct {
        u_char  s1[5];
        u_char  s2[5];
        size_t  length;
    } cases[] = {
        { { 'a', 'B', 0, 'c', 'D' }, { 'A', 'b', 0, 'x', 'y' }, 5 },
        { { 0, 'a' },                { 0, 'b' },                2 },
        { { 'a', 0, 'c' },           { 'A', 'b', 'c' },         3 },
        { { 'a', 'b', 0 },           { 'A', 'B', 0 },           3 },
    };

    for (i = 0; i < nxt_nitems(cases); i++) {
        if (nxt_casecmp_test_pair(ctx, cases[i].s1, cases[i].s2,
                                  cases[i].length, 0) != 0)
        {
            return 1;
        }
    }

    return 0;
}


/*
 * Each name in its own case, in lower case, in upper case and in a mixed
 * case must be equal to the name.  Names of the same length are compared
 * too.
 */

static int
nxt_casecmp_test_names_run(nxt_casecmp_test_ctx_t *ctx)
{
    u_char        v[4][NXT_CASECMP_TEST_MAX];
    size_t        length;
    nxt_uint_t    i, j, k;
    const u_char  *name, *other;

    for (i = 0; i < nxt_nitems(nxt_casecmp_test_names); i++) {
        name = (const u_char *) nxt_casecmp_test_names[i];
        length = nxt_strlen(name);

        for (k = 0; k < length; k++) {
            v[0][k] = name[k];
            v[1][k] = nxt_lowcase(name[k]);
            v[2][k] = nxt_upcase(name[k]);
            v[3][k] = (k & 1) ? v[1][k] : v[2][k];
        }

        for (k = 0; k < 4; k++) {
            if (nxt_casecmp_test_pair(ctx, v[k], name, length, 1) != 0
                || nxt_casecmp_test_pair(ctx, name, v[k], length, 1) != 0)
            {
                return 1;
            }
        }

        for (j = 0; j < nxt_nitems(nxt_casecmp_test_names); j++) {
            other = (const u_char *) nxt_casecmp_test_names[j];

            if (j == i || nxt_strlen(other) != length) {
                continue;
            }

            for (k = 0; k < 4; k++) {
                if (nxt_casecmp_test_pair(ctx, v[k], other, length, 0) != 0) {
                    return 1;
                }
            }
        }
    }

    return 0;
}


/*
 * Random strings of 0 to 64 bytes.  The second string flips the case of
 * about half of the letters and, rarely, bit 5 of another byte.  A quarter
 * of the pairs also get one random byte.
 */

static int
nxt_casecmp_test_random_run(nxt_casecmp_test_ctx_t *ctx)
{
    u_char      c, s1[NXT_CASECMP_TEST_MAX], s2[NXT_CASECMP_TEST_MAX];
    size_t      length;
    uint32_t    r;
    nxt_uint_t  i, k;

    for (i = 0; i < NXT_CASECMP_TEST_ROUNDS; i++) {
        length = nxt_casecmp_test_random(ctx) % (NXT_CASECMP_TEST_MAX + 1);

        for (k = 0; k < length; k++) {
            c = nxt_casecmp_test_byte(ctx);
            s1[k] = c;

            r = nxt_casecmp_test_random(ctx);

            if (nxt_casecmp_test_letter(c) ? (r & 1) : (r % 64 == 0)) {
                c ^= 0x20;
            }

            s2[k] = c;
        }

        r = nxt_casecmp_test_random(ctx);

        if (length != 0 && (r & 3) == 0) {
            s2[(r >> 8) % length] = (u_char) (r >> 16);
        }

        if (nxt_casecmp_test_pair(ctx, s1, s2, length, 0) != 0
            || nxt_casecmp_test_pair(ctx, s2, s1, length, 0) != 0)
        {
            return 1;
        }
    }

    return 0;
}


static int
nxt_casecmp_test_child(void *data)
{
    u_char                  *p;
    nxt_casecmp_test_ctx_t  *ctx;

    ctx = data;

    /* Two buffers, each one followed by a page that cannot be read. */

    p = mmap(NULL, 4 * nxt_pagesize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | NXT_MEM_MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        return 2;
    }

    if (mprotect(p + nxt_pagesize, nxt_pagesize, PROT_NONE) != 0
        || mprotect(p + 3 * nxt_pagesize, nxt_pagesize, PROT_NONE) != 0)
    {
        return 2;
    }

    ctx->end1 = p + nxt_pagesize;
    ctx->end2 = p + 3 * nxt_pagesize;

    if (nxt_casecmp_test_bytes(ctx) != 0
        || nxt_casecmp_test_nul(ctx) != 0
        || nxt_casecmp_test_names_run(ctx) != 0
        || nxt_casecmp_test_random_run(ctx) != 0)
    {
        return 1;
    }

    return 0;
}


nxt_int_t
nxt_casecmp_test(nxt_thread_t *thr)
{
    int                     rc;
    nxt_casecmp_test_ctx_t  ctx;

    nxt_thread_time_update(thr);

    ctx.thr = thr;
    ctx.end1 = NULL;
    ctx.end2 = NULL;
    ctx.seed = 0x9e3779b9;

    rc = nxt_test_in_child(thr, "casecmp test", nxt_casecmp_test_child, &ctx);

    NXT_TEST_CHECK(thr->log, rc == 0, "casecmp test failed (child status %d)",
                   rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "casecmp test passed");

    return NXT_OK;
}
