
/*
 * Copyright (C) FreeUnit Community
 */

/*
 * nxt_memcase_eq_lit(), nxt_memcase_eq() and nxt_memcase_eq_fold() must give
 * the result of nxt_memcasecmp() == 0 for every input.  nxt_casecmp_test()
 * checks nxt_memcasecmp() against a loop that folds every byte.
 *
 * The compares run in a child.  Each input is placed twice: it ends where a
 * PROT_NONE page starts, and it starts where one ends.  The pattern and the
 * folded pattern are placed the same way.  So a load outside the length
 * kills the child.
 */

#include <nxt_main.h>
#include "nxt_tests.h"


#define NXT_MEMCASE_EQ_TEST_MAX       255
#define NXT_MEMCASE_EQ_TEST_VARIANTS  16384
#define NXT_MEMCASE_EQ_TEST_ROUNDS    2000000

#define nxt_memcase_eq_test_letter(c)                                         \
    ((u_char) (((c) | 0x20) - 'a') < 26)


/*
 * Each literal is a constant at the call site of its own function, as at a
 * real call site, so the compiler folds its masks.  The list has 26 header
 * names, a literal of every length from 0 to 40, and literals with NUL,
 * with the bytes next to the letters, and with bytes from 0x80 up.
 */

#define NXT_MEMCASE_EQ_TEST_LITERALS(X)                                       \
    X(0, "Host")                                                              \
    X(1, "Date")                                                              \
    X(2, "ETag")                                                              \
    X(3, "Vary")                                                              \
    X(4, "Range")                                                             \
    X(5, "Cookie")                                                            \
    X(6, "Expect")                                                            \
    X(7, "Server")                                                            \
    X(8, "Accept")                                                            \
    X(9, "Upgrade")                                                           \
    X(10, "Referer")                                                          \
    X(11, "If-Match")                                                         \
    X(12, "If-Range")                                                         \
    X(13, "Connection")                                                       \
    X(14, "User-Agent")                                                       \
    X(15, "Content-Type")                                                     \
    X(16, "Content-Length")                                                   \
    X(17, "Authorization")                                                    \
    X(18, "Accept-Encoding")                                                  \
    X(19, "Content-Encoding")                                                 \
    X(20, "Transfer-Encoding")                                                \
    X(21, "If-Modified-Since")                                                \
    X(22, "Sec-WebSocket-Key")                                                \
    X(23, "If-Unmodified-Since")                                              \
    X(24, "Sec-WebSocket-Version")                                            \
    X(25, "Upgrade-Insecure-Requests")                                        \
    X(26, "")                                                                 \
    X(27, "a")                                                                \
    X(28, "aB")                                                               \
    X(29, "aB3")                                                              \
    X(30, "aB3-")                                                             \
    X(31, "aB3-c")                                                            \
    X(32, "aB3-cD")                                                           \
    X(33, "aB3-cD9")                                                          \
    X(34, "aB3-cD9_")                                                         \
    X(35, "aB3-cD9_e")                                                        \
    X(36, "aB3-cD9_eF")                                                       \
    X(37, "aB3-cD9_eF0")                                                      \
    X(38, "aB3-cD9_eF0~")                                                     \
    X(39, "aB3-cD9_eF0~g")                                                    \
    X(40, "aB3-cD9_eF0~gH")                                                   \
    X(41, "aB3-cD9_eF0~gH1")                                                  \
    X(42, "aB3-cD9_eF0~gH1.")                                                 \
    X(43, "aB3-cD9_eF0~gH1.i")                                                \
    X(44, "aB3-cD9_eF0~gH1.iJ")                                               \
    X(45, "aB3-cD9_eF0~gH1.iJ2")                                              \
    X(46, "aB3-cD9_eF0~gH1.iJ2!")                                             \
    X(47, "aB3-cD9_eF0~gH1.iJ2!k")                                            \
    X(48, "aB3-cD9_eF0~gH1.iJ2!kL")                                           \
    X(49, "aB3-cD9_eF0~gH1.iJ2!kL4")                                          \
    X(50, "aB3-cD9_eF0~gH1.iJ2!kL4#")                                         \
    X(51, "aB3-cD9_eF0~gH1.iJ2!kL4#m")                                        \
    X(52, "aB3-cD9_eF0~gH1.iJ2!kL4#mN")                                       \
    X(53, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5")                                      \
    X(54, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$")                                     \
    X(55, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$o")                                    \
    X(56, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP")                                   \
    X(57, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP6")                                  \
    X(58, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP6%")                                 \
    X(59, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP6%q")                                \
    X(60, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP6%qR")                               \
    X(61, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP6%qR7")                              \
    X(62, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP6%qR7&")                             \
    X(63, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP6%qR7&s")                            \
    X(64, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP6%qR7&sT")                           \
    X(65, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP6%qR7&sT8")                          \
    X(66, "aB3-cD9_eF0~gH1.iJ2!kL4#mN5$oP6%qR7&sT8'")                         \
    X(67, "@[`{")                                                             \
    X(68, "A@Z[a`z{")                                                         \
    X(69, "-0129@AZ[`az{~")                                                   \
    X(70, "a\0b")                                                             \
    X(71, "\0\0\0\0\0\0\0\0\0")                                               \
    X(72, "\xc1\xe1Zz\xda\xfa")                                               \
    X(73, "x\x80y\xffz\x7f_^")                                                \
    X(74, "\x20\x0d-\x0d")


#define NXT_MEMCASE_EQ_TEST_FN(n, s)                                          \
                                                                              \
    static nxt_bool_t                                                         \
    nxt_memcase_eq_test_lit##n(const u_char *p)                               \
    {                                                                         \
        return nxt_memcase_eq_lit(p, s);                                      \
    }

NXT_MEMCASE_EQ_TEST_LITERALS(NXT_MEMCASE_EQ_TEST_FN)


typedef nxt_bool_t (*nxt_memcase_eq_test_fn_t)(const u_char *p);


typedef struct {
    nxt_str_t                 s;
    nxt_memcase_eq_test_fn_t  fn;
} nxt_memcase_eq_test_lit_t;


#define NXT_MEMCASE_EQ_TEST_ENTRY(n, s)                                       \
    { nxt_string(s), nxt_memcase_eq_test_lit##n },

static const nxt_memcase_eq_test_lit_t  nxt_memcase_eq_test_lits[] = {
    NXT_MEMCASE_EQ_TEST_LITERALS(NXT_MEMCASE_EQ_TEST_ENTRY)
};


/*
 * Three readable pages, each between two pages that cannot be read: for the
 * input, for the pattern and for the folded pattern.  Layout 0 puts a string
 * at the end of its page, layout 1 at the start.
 */

typedef struct {
    nxt_thread_t              *thr;
    u_char                    *start[3];
    u_char                    *end[3];
    u_char                    *s[2];
    u_char                    *low[2];
    size_t                    length;
    nxt_uint_t                checks;
    uint32_t                  seed;
} nxt_memcase_eq_test_ctx_t;


/* Bytes next to the letters, and high bytes that differ only in bit 5. */

static const u_char  nxt_memcase_eq_test_edge[] = {
    0x00, 0x01, 0x0d, 0x20, '-', '0', '9', '?', '@', 'A', 'M', 'Z', '[',
    '\\', ']', '^', '_', '`', 'a', 'm', 'z', '{', '|', '}', '~', 0x7f, 0x80,
    0x81, 0xa0, 0xc0, 0xc1, 0xda, 0xde, 0xdf, 0xe0, 0xe1, 0xfa, 0xfe, 0xff,
};


static uint32_t
nxt_memcase_eq_test_random(nxt_memcase_eq_test_ctx_t *ctx)
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
nxt_memcase_eq_test_byte(nxt_memcase_eq_test_ctx_t *ctx)
{
    uint32_t  r;

    r = nxt_memcase_eq_test_random(ctx);

    switch (r & 3) {

    case 0:
    case 1:
        return 'A' + (r >> 8) % 26 + ((r >> 16) & 0x20);

    case 2:
        return nxt_memcase_eq_test_edge[(r >> 8)
                                        % sizeof(nxt_memcase_eq_test_edge)];

    default:
        return (u_char) (r >> 8);
    }
}


static void
nxt_memcase_eq_test_hex(char *dst, const u_char *s, size_t length)
{
    static const char  hex[] = "0123456789abcdef";

    while (length-- != 0) {
        *dst++ = hex[*s >> 4];
        *dst++ = hex[*s++ & 0xf];
    }

    *dst = '\0';
}


/* Copies the pattern and its folded form to both layouts. */

static void
nxt_memcase_eq_test_pattern(nxt_memcase_eq_test_ctx_t *ctx, const u_char *s,
    size_t length)
{
    ctx->length = length;

    ctx->s[0] = ctx->end[1] - length;
    ctx->s[1] = ctx->start[1];
    ctx->low[0] = ctx->end[2] - 2 * length;
    ctx->low[1] = ctx->start[2];

    nxt_memcpy(ctx->s[0], s, length);
    nxt_memcpy(ctx->s[1], s, length);

    nxt_casefold_set(ctx->low[0], s, length);
    nxt_casefold_set(ctx->low[1], s, length);
}


/*
 * Compares the input with the pattern in both layouts: as the literal when
 * "fn" is set, as a pattern in memory, and as the folded pattern.
 */

static int
nxt_memcase_eq_test_check(nxt_memcase_eq_test_ctx_t *ctx, const u_char *in,
    nxt_memcase_eq_test_fn_t fn)
{
    int         lit;
    char        h1[2 * NXT_MEMCASE_EQ_TEST_MAX + 1];
    char        h2[2 * NXT_MEMCASE_EQ_TEST_MAX + 1];
    u_char      *p;
    size_t      length;
    nxt_bool_t  ref, eq, fold;
    nxt_uint_t  layout;

    length = ctx->length;

    for (layout = 0; layout < 2; layout++) {
        p = (layout == 0) ? ctx->end[0] - length : ctx->start[0];
        nxt_memcpy(p, in, length);

        ref = (nxt_memcasecmp(p, ctx->s[layout], length) == 0);

        lit = (fn != NULL) ? (int) fn(p) : -1;
        eq = nxt_memcase_eq(p, ctx->s[layout], length);
        fold = nxt_memcase_eq_fold(p, ctx->low[layout], length);

        ctx->checks += (fn != NULL) ? 3 : 2;

        if ((fn != NULL && lit != (int) ref) || eq != ref || fold != ref) {
            nxt_memcase_eq_test_hex(h1, p, length);
            nxt_memcase_eq_test_hex(h2, ctx->s[layout], length);

            nxt_log_alert(ctx->thr->log, "memcase eq test: layout %d, "
                          "length %d: nxt_memcasecmp() equal %d, "
                          "nxt_memcase_eq_lit() %d, nxt_memcase_eq() %d, "
                          "nxt_memcase_eq_fold() %d, input %s, pattern %s",
                          (int) layout, (int) length, (int) ref, lit,
                          (int) eq, (int) fold, h1, h2);
            return 1;
        }
    }

    return 0;
}


/*
 * The pattern in every case variant of its letters: 2^n variants for up to
 * 14 letters, else 16384 random variants.  Then the pattern as it is, in
 * lower case and in upper case, with every byte value at every position.
 */

static int
nxt_memcase_eq_test_variants(nxt_memcase_eq_test_ctx_t *ctx,
    const u_char *s, size_t length, nxt_memcase_eq_test_fn_t fn)
{
    u_char      in[NXT_MEMCASE_EQ_TEST_MAX], base[NXT_MEMCASE_EQ_TEST_MAX];
    size_t      i, pos;
    uint32_t    bits;
    nxt_uint_t  b, k, n, v, nv, form;

    nxt_memcase_eq_test_pattern(ctx, s, length);

    n = 0;

    for (i = 0; i < length; i++) {
        n += nxt_memcase_eq_test_letter(s[i]);
    }

    nv = (n <= 14) ? (nxt_uint_t) 1 << n : NXT_MEMCASE_EQ_TEST_VARIANTS;

    for (v = 0; v < nv; v++) {
        bits = (n <= 14) ? v : nxt_memcase_eq_test_random(ctx);

        for (i = 0, k = 0; i < length; i++) {
            in[i] = s[i];

            if (nxt_memcase_eq_test_letter(s[i])) {
                if (bits & ((uint32_t) 1 << (k & 31))) {
                    in[i] ^= 0x20;
                }

                k++;
            }
        }

        if (nxt_memcase_eq_test_check(ctx, in, fn) != 0) {
            return 1;
        }
    }

    for (form = 0; form < 3; form++) {
        for (i = 0; i < length; i++) {
            base[i] = (form == 0) ? s[i]
                      : (form == 1) ? nxt_lowcase(s[i]) : nxt_upcase(s[i]);
        }

        for (pos = 0; pos < length; pos++) {
            for (b = 0; b < 256; b++) {
                nxt_memcpy(in, base, length);
                in[pos] = (u_char) b;

                if (nxt_memcase_eq_test_check(ctx, in, fn) != 0) {
                    return 1;
                }
            }
        }
    }

    return 0;
}


static int
nxt_memcase_eq_test_literals(nxt_memcase_eq_test_ctx_t *ctx)
{
    nxt_uint_t                       i;
    const nxt_memcase_eq_test_lit_t  *lit;

    for (i = 0; i < nxt_nitems(nxt_memcase_eq_test_lits); i++) {
        lit = &nxt_memcase_eq_test_lits[i];

        if (nxt_memcase_eq_test_variants(ctx, lit->s.start, lit->s.length,
                                         lit->fn)
            != 0)
        {
            return 1;
        }
    }

    return 0;
}


/*
 * Random patterns of every length from 0 to 40.  The input first differs at
 * each position: by another byte (kinds 2 and 3), or not at all (kinds 0
 * and 1).  In kinds 1 and 3, the letters up to the position also change
 * case, at random.
 */

static int
nxt_memcase_eq_test_lengths(nxt_memcase_eq_test_ctx_t *ctx)
{
    u_char      s[40], in[40];
    size_t      i, pos, length;
    nxt_uint_t  rep, kind;

    for (length = 0; length <= 40; length++) {
        for (pos = 0; pos <= length; pos++) {
            for (kind = 0; kind < 4; kind++) {
                for (rep = 0; rep < 16; rep++) {

                    for (i = 0; i < length; i++) {
                        s[i] = nxt_memcase_eq_test_byte(ctx);
                        in[i] = s[i];

                        if ((kind & 1) && i <= pos
                            && nxt_memcase_eq_test_letter(in[i])
                            && (nxt_memcase_eq_test_random(ctx) & 1))
                        {
                            in[i] ^= 0x20;
                        }
                    }

                    /* Bytes that differ in bit 0 are never equal. */

                    if (kind >= 2 && pos < length) {
                        in[pos] = s[pos] ^ 1;
                    }

                    nxt_memcase_eq_test_pattern(ctx, s, length);

                    if (nxt_memcase_eq_test_check(ctx, in, NULL) != 0) {
                        return 1;
                    }
                }
            }
        }
    }

    return 0;
}


/*
 * Random pairs with biased case flips.  Half of the patterns are the
 * literals, compared in all three forms.  The other half are random bytes
 * of 0 to 40 bytes, and one in 16 of them is up to 255 bytes long, the range
 * of a field name length.
 */

static int
nxt_memcase_eq_test_random_run(nxt_memcase_eq_test_ctx_t *ctx)
{
    u_char                           c, s[NXT_MEMCASE_EQ_TEST_MAX];
    u_char                           in[NXT_MEMCASE_EQ_TEST_MAX];
    size_t                           i, length;
    uint32_t                         r;
    nxt_uint_t                       round;
    const u_char                     *src;
    nxt_memcase_eq_test_fn_t         fn;
    const nxt_memcase_eq_test_lit_t  *lit;

    for (round = 0; round < NXT_MEMCASE_EQ_TEST_ROUNDS; round++) {
        r = nxt_memcase_eq_test_random(ctx);

        if (r & 1) {
            i = (r >> 1) % nxt_nitems(nxt_memcase_eq_test_lits);
            lit = &nxt_memcase_eq_test_lits[i];

            src = lit->s.start;
            length = lit->s.length;
            fn = lit->fn;

        } else {
            length = ((r >> 1) % 16 == 0) ? (r >> 8) % 256 : (r >> 8) % 41;

            for (i = 0; i < length; i++) {
                s[i] = nxt_memcase_eq_test_byte(ctx);
            }

            src = s;
            fn = NULL;
        }

        for (i = 0; i < length; i++) {
            r = nxt_memcase_eq_test_random(ctx);
            c = src[i];

            switch (r % 8) {

            case 0:
            case 1:
            case 2:
                if (nxt_memcase_eq_test_letter(c)) {
                    c ^= 0x20;
                }

                break;

            case 3:
                if ((r >> 8) % 4 == 0) {
                    c = nxt_memcase_eq_test_byte(ctx);
                }

                break;

            default:
                break;
            }

            in[i] = c;
        }

        nxt_memcase_eq_test_pattern(ctx, src, length);

        if (nxt_memcase_eq_test_check(ctx, in, fn) != 0) {
            return 1;
        }
    }

    return 0;
}


static int
nxt_memcase_eq_test_child(void *data)
{
    u_char                     *p;
    nxt_uint_t                 i;
    nxt_memcase_eq_test_ctx_t  *ctx;

    ctx = data;

    p = mmap(NULL, 7 * nxt_pagesize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | NXT_MEM_MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        return 2;
    }

    for (i = 0; i < 4; i++) {
        if (mprotect(p + 2 * i * nxt_pagesize, nxt_pagesize, PROT_NONE) != 0) {
            return 2;
        }
    }

    for (i = 0; i < 3; i++) {
        ctx->start[i] = p + (2 * i + 1) * nxt_pagesize;
        ctx->end[i] = ctx->start[i] + nxt_pagesize;
    }

    if (nxt_memcase_eq_test_literals(ctx) != 0
        || nxt_memcase_eq_test_lengths(ctx) != 0
        || nxt_memcase_eq_test_random_run(ctx) != 0)
    {
        return 1;
    }

    nxt_log_error(NXT_LOG_NOTICE, ctx->thr->log,
                  "memcase eq test: %d checks", (int) ctx->checks);

    return 0;
}


nxt_int_t
nxt_memcase_eq_test(nxt_thread_t *thr)
{
    int                        rc;
    nxt_memcase_eq_test_ctx_t  ctx;

    nxt_thread_time_update(thr);

    nxt_memzero(&ctx, sizeof(nxt_memcase_eq_test_ctx_t));

    ctx.thr = thr;
    ctx.seed = 0x9e3779b9;

    rc = nxt_test_in_child(thr, "memcase eq test", nxt_memcase_eq_test_child,
                           &ctx);

    NXT_TEST_CHECK(thr->log, rc == 0,
                   "memcase eq test failed (child status %d)", rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "memcase eq test passed");

    return NXT_OK;
}
