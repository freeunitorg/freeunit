
/*
 * Copyright (C) FreeUnit Community
 */

/*
 * nxt_http_lookup_field_end() tests 8 bytes at a time.  Its result must be
 * that of the byte loop it replaced together with the DEL check that
 * nxt_http_parse_field_value() did after that loop: the first byte below
 * 0x20 or equal to 0x7F, else "end".  The reference function below is that
 * loop, without its unrolling, and that memchr() call.
 *
 * The scans run in a child.  Each value ends where a PROT_NONE page starts,
 * except in the random test, which also puts values inside the page.  A read
 * past "end" at the page end kills the child.
 */

#include <nxt_main.h>
#include "nxt_tests.h"


#define NXT_HTTP_FIELD_END_TEST_VALUE   31
#define NXT_HTTP_FIELD_END_TEST_MAX     64
#define NXT_HTTP_FIELD_END_TEST_PAGE    40
#define NXT_HTTP_FIELD_END_TEST_ROUNDS  10000000


typedef struct {
    nxt_thread_t  *thr;
    u_char        *start;
    u_char        *end;
    uint32_t      seed;
} nxt_http_field_end_test_ctx_t;


/* The bytes that end the scan: 0x00 to 0x1F and 0x7F, 33 of 256. */

#define nxt_http_field_end_test_stops(c)                                      \
    ((c) <= 0x1F || (c) == 0x7F)


/* Bytes that are not stops, and in a word can get a borrow or a carry. */

static const u_char  nxt_http_field_end_test_fill[] = {
    'a', ' ', '~', 0x80, 0xFE, 0xFF,
};


static const u_char  nxt_http_field_end_test_edge[] = {
    0x00, 0x09, 0x0A, 0x0D, 0x1F, 0x20, 0x21, 0x7E, 0x7F, 0x80, 0x9F, 0xA0,
    0xDF, 0xE0, 0xFE, 0xFF,
};


static u_char *
nxt_http_field_end_test_ref(u_char *p, const u_char *end)
{
    u_char  *q, *del;

    for (q = p; q != end; q++) {
        if (*q < 0x20) {
            break;
        }
    }

    del = memchr(p, 0x7F, q - p);

    return (del != NULL) ? del : q;
}


static u_char *
nxt_http_field_end_test_hex(u_char *dst, const u_char *src, size_t length)
{
    static const u_char  hex[] = "0123456789abcdef";

    while (length-- != 0) {
        *dst++ = hex[*src >> 4];
        *dst++ = hex[*src++ & 0xf];
    }

    return dst;
}


/*
 * Scans "length" bytes at "p" with both functions.  The result must be the
 * reference result, and "expect" bytes from "p" when "expect" is not -1.
 */

static int
nxt_http_field_end_test_one(nxt_http_field_end_test_ctx_t *ctx, u_char *p,
    size_t length, ssize_t expect, const char *name)
{
    u_char  *end, *r, *ref, *e;
    u_char  hex[2 * NXT_HTTP_FIELD_END_TEST_MAX];

    end = p + length;

    r = nxt_http_test_lookup_field_end(p, end);
    ref = nxt_http_field_end_test_ref(p, end);

    if (r == ref && (expect == -1 || r == p + expect)) {
        return 0;
    }

    e = nxt_http_field_end_test_hex(hex, p,
                                    nxt_min(length,
                                            NXT_HTTP_FIELD_END_TEST_MAX));

    nxt_log_alert(ctx->thr->log, "http field end test: %s: \"%*s\" "
                  "(%uz bytes) gave %uz, reference %uz, expected %z",
                  name, (size_t) (e - hex), hex, length, (size_t) (r - p),
                  (size_t) (ref - p), expect);

    return 1;
}


static uint32_t
nxt_http_field_end_test_random(nxt_http_field_end_test_ctx_t *ctx)
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


/*
 * Every byte value at every position of a 24-byte value and of a 31-byte
 * value, in each of the fill bytes.  The 24 bytes are three words; the 31
 * bytes also reach the 4-byte and the 1-byte tails.  Exactly 33 byte values
 * stop the scan of a 1-byte value.
 */

static int
nxt_http_field_end_test_bytes(nxt_http_field_end_test_ctx_t *ctx)
{
    u_char      *p;
    size_t      length;
    ssize_t     expect;
    nxt_uint_t  b, f, i, n, stops;

    static const size_t  lengths[] = { 24, NXT_HTTP_FIELD_END_TEST_VALUE };

    for (n = 0; n < nxt_nitems(lengths); n++) {
        length = lengths[n];
        p = ctx->end - length;

        for (f = 0; f < sizeof(nxt_http_field_end_test_fill); f++) {
            for (b = 0; b < 256; b++) {
                for (i = 0; i < length; i++) {
                    nxt_memset(p, nxt_http_field_end_test_fill[f], length);
                    p[i] = (u_char) b;

                    if (nxt_http_field_end_test_stops(b)) {
                        expect = i;

                    } else {
                        expect = length;
                    }

                    if (nxt_http_field_end_test_one(ctx, p, length, expect,
                                                    "byte")
                        != 0)
                    {
                        return 1;
                    }
                }
            }
        }
    }

    p = ctx->end - 1;
    stops = 0;

    for (b = 0; b < 256; b++) {
        *p = (u_char) b;

        if (nxt_http_test_lookup_field_end(p, ctx->end) == p) {
            stops++;
        }
    }

    if (stops != 33) {
        nxt_log_alert(ctx->thr->log, "http field end test: %ui byte values "
                      "stop the scan, not 33", stops);
        return 1;
    }

    return 0;
}


/*
 * Values of 0 to 40 bytes that end at the PROT_NONE page: with no stop,
 * with a CR as the last byte, with bytes that carry into a 0x7E, and with
 * random bytes.
 */

static int
nxt_http_field_end_test_page_end(nxt_http_field_end_test_ctx_t *ctx)
{
    u_char      *p;
    size_t      length;
    nxt_uint_t  i, k;

    for (length = 0; length <= NXT_HTTP_FIELD_END_TEST_PAGE; length++) {
        p = ctx->end - length;

        nxt_memset(p, 'a', length);

        if (nxt_http_field_end_test_one(ctx, p, length, length, "page end")
            != 0)
        {
            return 1;
        }

        if (length != 0) {
            p[length - 1] = '\r';

            if (nxt_http_field_end_test_one(ctx, p, length, length - 1,
                                            "page end CR")
                != 0)
            {
                return 1;
            }
        }

        for (i = 0; i < length; i++) {
            p[i] = (i & 1) ? 0x7E : 0xFF;
        }

        if (nxt_http_field_end_test_one(ctx, p, length, length,
                                        "page end carry")
            != 0)
        {
            return 1;
        }

        for (k = 0; k < 100; k++) {
            for (i = 0; i < length; i++) {
                p[i] = nxt_http_field_end_test_edge[
                           nxt_http_field_end_test_random(ctx)
                           % sizeof(nxt_http_field_end_test_edge)];
            }

            if (nxt_http_field_end_test_one(ctx, p, length, -1,
                                            "page end random")
                != 0)
            {
                return 1;
            }
        }
    }

    return 0;
}


/* Two stops in a 16-byte value: the scan returns the first one. */

static int
nxt_http_field_end_test_pairs(nxt_http_field_end_test_ctx_t *ctx)
{
    u_char      *p;
    nxt_uint_t  a, b, f, i, j;

    static const u_char  stops[] = { 0x00, '\t', '\n', '\r', 0x1F, 0x7F };

    p = ctx->end - 16;

    for (f = 0; f < sizeof(nxt_http_field_end_test_fill); f++) {
        for (i = 0; i < 16; i++) {
            for (j = i + 1; j < 16; j++) {
                for (a = 0; a < sizeof(stops); a++) {
                    for (b = 0; b < sizeof(stops); b++) {
                        nxt_memset(p, nxt_http_field_end_test_fill[f], 16);
                        p[i] = stops[a];
                        p[j] = stops[b];

                        if (nxt_http_field_end_test_one(ctx, p, 16, i, "pair")
                            != 0)
                        {
                            return 1;
                        }
                    }
                }
            }
        }
    }

    return 0;
}


/*
 * Random values of 0 to 64 bytes, compared with the reference.  A round
 * picks how often a stop byte appears, from never to every other byte.
 * Other bytes are printable, the edge bytes or any byte.  A quarter of the
 * values end at the PROT_NONE page; the rest start anywhere in the page.
 */

static int
nxt_http_field_end_test_random_run(nxt_http_field_end_test_ctx_t *ctx)
{
    u_char      *p, c;
    size_t      length, size;
    uint32_t    r, density;
    nxt_uint_t  n, i;

    size = ctx->end - ctx->start;

    for (n = 0; n < NXT_HTTP_FIELD_END_TEST_ROUNDS; n++) {
        r = nxt_http_field_end_test_random(ctx);

        length = r % (NXT_HTTP_FIELD_END_TEST_MAX + 1);
        density = (r >> 8) & 3;

        r = nxt_http_field_end_test_random(ctx);

        if ((r & 3) == 0) {
            p = ctx->end - length;

        } else {
            p = ctx->start + (r >> 2) % (size - length + 1);
        }

        for (i = 0; i < length; i++) {
            r = nxt_http_field_end_test_random(ctx);

            if (density != 0 && (r & 0x3F) < (1u << (2 * density - 1))) {
                c = (r >> 8) % 33;
                p[i] = (c == 32) ? 0x7F : c;
                continue;
            }

            switch ((r >> 16) & 3) {

            case 0:
                p[i] = nxt_http_field_end_test_edge[(r >> 18)
                                       % sizeof(nxt_http_field_end_test_edge)];
                break;

            case 1:
                p[i] = (u_char) (r >> 24);
                break;

            default:
                p[i] = 0x21 + (r >> 18) % 94;
                break;
            }
        }

        if (nxt_http_field_end_test_one(ctx, p, length, -1, "random") != 0) {
            return 1;
        }
    }

    return 0;
}


static int
nxt_http_field_end_test_child(void *data)
{
    u_char                         *p;
    nxt_http_field_end_test_ctx_t  *ctx;

    ctx = data;

    /* A readable page followed by a page that cannot be read. */

    p = mmap(NULL, 2 * nxt_pagesize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | NXT_MEM_MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        return 2;
    }

    if (mprotect(p + nxt_pagesize, nxt_pagesize, PROT_NONE) != 0) {
        return 2;
    }

    ctx->start = p;
    ctx->end = p + nxt_pagesize;

    if (nxt_http_field_end_test_bytes(ctx) != 0
        || nxt_http_field_end_test_page_end(ctx) != 0
        || nxt_http_field_end_test_pairs(ctx) != 0
        || nxt_http_field_end_test_random_run(ctx) != 0)
    {
        return 1;
    }

    return 0;
}


nxt_int_t
nxt_http_field_end_test(nxt_thread_t *thr)
{
    int                            rc;
    nxt_http_field_end_test_ctx_t  ctx;

    nxt_thread_time_update(thr);

    ctx.thr = thr;
    ctx.start = NULL;
    ctx.end = NULL;
    ctx.seed = 0x9e3779b9;

    rc = nxt_test_in_child(thr, "http field end test",
                           nxt_http_field_end_test_child, &ctx);

    NXT_TEST_CHECK(thr->log, rc == 0,
                   "http field end test failed (child status %d)", rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "http field end test passed");

    return NXT_OK;
}
