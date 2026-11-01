/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_http_comp_select_compressor() (src/nxt_http_compression.c) skips the
 * Accept-Encoding parse when compression is off and the value has no ';'.
 * The skip must give the same result as the parse.  This test runs both on
 * a generated set of values, with compression off and on, and compares the
 * chosen coding and the identity refusal.  The static handler serves no
 * range when identity is refused, so the refusal flag covers that path too.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_http_compression.h>
#include "nxt_tests.h"


#define NXT_HCS_MAX      256
#define NXT_HCS_RANDOM   100000
#define NXT_HCS_PER_MP   4096


typedef struct {
    nxt_thread_t        *thr;
    nxt_http_request_t  *r;
    nxt_uint_t          count;
    nxt_uint_t          skipped;
} nxt_hcs_ctx_t;


static const char  *nxt_hcs_tokens[] = {
    "identity", "IDENTITY", "Identity", "identit", "identityx", "*", "*foo",
    "gzip", "GZIP", "x-gzip", "deflate", "br", "zstd", "compress", "",
};


static const char  *nxt_hcs_weights[] = {
    "", ";q=0", ";Q=0", ";q=0.0", ";q=0.000", ";q=0.5", ";q=1", ";q=1.0",
    ";q=1.001", ";q=", ";q=abc", ";q=0x10", ";q=nan", ";level=9",
    ";q=0;level=1", ";level=1;q=0", ";", ";;q=0", "; q=0", ";\tq=0",
    " ;q=0", "q=0",
};


static const char  *nxt_hcs_spaces[] = { "", " ", "\t" };


static const char  *nxt_hcs_seps[] = { ",", ", ", " ,", ",\t", ",," };


static nxt_int_t
nxt_hcs_compare(nxt_hcs_ctx_t *ctx, const u_char *p, size_t len)
{
    bool        ref_refused, new_refused;
    nxt_int_t   ref, res;
    nxt_str_t   value;
    nxt_uint_t  mode;

    static const struct {
        nxt_bool_t  compression;
        nxt_off_t   min_len;
        nxt_off_t   clen;
    } modes[] = {
        { 0, -1, -1 },
        { 0, -1, 100 },
        { 1, -1, -1 },
        { 1, 1000, 100 },
    };

    if (ctx->count % NXT_HCS_PER_MP == 0) {
        if (ctx->r->mem_pool != NULL) {
            nxt_mp_destroy(ctx->r->mem_pool);
        }

        ctx->r->mem_pool = nxt_mp_create(1024, 128, 256, 32);
        if (nxt_slow_path(ctx->r->mem_pool == NULL)) {
            return NXT_ERROR;
        }
    }

    ctx->count++;

    value.start = (u_char *) p;
    value.length = len;

    for (mode = 0; mode < nxt_nitems(modes); mode++) {
        ctx->r->resp.content_length_n = modes[mode].clen;

        ref = nxt_http_comp_test_select(ctx->r, &value,
                                        modes[mode].compression,
                                        modes[mode].min_len, 1,
                                        &ref_refused);

        res = nxt_http_comp_test_select(ctx->r, &value,
                                        modes[mode].compression,
                                        modes[mode].min_len, 0,
                                        &new_refused);

        NXT_TEST_CHECK(ctx->thr->log,
                       ref == res && ref_refused == new_refused,
                       "http comp select test failed: mode %ui value "
                       "\"%*s\" gave %i/%d, the parse gives %i/%d",
                       mode, len, p, res, (int) new_refused, ref,
                       (int) ref_refused);
    }

    if (memchr(p, ';', len) == NULL) {
        ctx->skipped++;
    }

    return NXT_OK;
}


static size_t
nxt_hcs_append(u_char *buf, size_t len, const char *s)
{
    size_t  n;

    n = nxt_strlen(s);

    if (len + n > NXT_HCS_MAX) {
        return len;
    }

    nxt_memcpy(buf + len, s, n);

    return len + n;
}


static size_t
nxt_hcs_element(u_char *buf, size_t len, nxt_uint_t t, nxt_uint_t w)
{
    len = nxt_hcs_append(buf, len, nxt_hcs_tokens[t]);

    return nxt_hcs_append(buf, len, nxt_hcs_weights[w]);
}


/* A fixed sequence, so a failure can be reproduced. */

static uint32_t
nxt_hcs_random(uint32_t *state)
{
    *state = *state * 1103515245 + 12345;

    return *state >> 8;
}


static nxt_int_t
nxt_hcs_expect(nxt_hcs_ctx_t *ctx)
{
    bool        refused;
    nxt_int_t   res;
    nxt_str_t   value;
    nxt_uint_t  i;

    static const struct {
        const char  *value;
        nxt_int_t   idx;
        bool        refused;
    } cases[] = {
        { "gzip, deflate, br, zstd", 0, false },
        { "identity", 0, false },
        { "*", 0, false },
        { "identity;q=0", -1, true },
        { "identity;Q=0", -1, true },
        { "identity ;\tq=0", -1, true },
        { "*;q=0", -1, true },
        { "gzip;q=1, identity;q=0", -1, true },
        { "identity;q=0, identity", 0, false },
        { "*;q=0, identity", 0, false },
    };

    ctx->r->resp.content_length_n = -1;

    for (i = 0; i < nxt_nitems(cases); i++) {
        value.start = (u_char *) cases[i].value;
        value.length = nxt_strlen(cases[i].value);

        res = nxt_http_comp_test_select(ctx->r, &value, 0, -1, 0, &refused);

        NXT_TEST_CHECK(ctx->thr->log,
                       res == cases[i].idx && refused == cases[i].refused,
                       "http comp select test failed: \"%s\" gave %i/%d, "
                       "not %i/%d", cases[i].value, res, (int) refused,
                       cases[i].idx, (int) cases[i].refused);
    }

#if (NXT_HAVE_ZLIB)
    /* With compression on, a value without ';' still selects a coding. */

    value.start = (u_char *) "gzip";
    value.length = nxt_length("gzip");

    res = nxt_http_comp_test_select(ctx->r, &value, 1, -1, 0, &refused);

    NXT_TEST_CHECK(ctx->thr->log, res > 0 && !refused,
                   "http comp select test failed: \"gzip\" with "
                   "compression on gave %i/%d", res, (int) refused);

    {
        nxt_http_field_t  cl_field;

        nxt_memzero(&cl_field, sizeof(cl_field));
        ctx->r->resp.content_length = &cl_field;

        /*
         * An application's Content-Length is read over value_length, not to
         * a NUL: "100" followed by "999" in memory is 100.  That is below
         * min_len 1000, so gzip is not selected, and the refused identity
         * gives -1.  strtol() read 100999 and selected gzip.
         */
        cl_field.value = (u_char *) "100999";
        cl_field.value_length = 3;

        value.start = (u_char *) "gzip, identity;q=0";
        value.length = nxt_length("gzip, identity;q=0");

        res = nxt_http_comp_test_select(ctx->r, &value, 1, 1000, 0, &refused);

        NXT_TEST_CHECK(ctx->thr->log, res == -1 && refused,
                       "http comp select bounded clen test failed: %i/%d",
                       res, (int) refused);

        /*
         * Whitespace around the digits is trimmed, as the response parser
         * accepted it: " 100\t" is 100, below min_len, so identity (0) is
         * selected and not refused.  strtol() gave the same; this pins the
         * trim in front of nxt_off_t_parse(), which takes digits only.
         */
        value.start = (u_char *) "gzip";
        value.length = nxt_length("gzip");

        cl_field.value = (u_char *) " 100\t";
        cl_field.value_length = 5;

        res = nxt_http_comp_test_select(ctx->r, &value, 1, 1000, 0, &refused);

        NXT_TEST_CHECK(ctx->thr->log, res == 0 && !refused,
                       "http comp select trimmed clen test failed: %i/%d",
                       res, (int) refused);

        ctx->r->resp.content_length = NULL;
    }

    {
        ssize_t                         n;
        uint8_t                         out[4];
        nxt_http_comp_compressor_ctx_t  cctx;

        static const uint8_t  str[] = "a last call with no room left";

        /*
         * A last call whose output cannot hold the whole stream leaves
         * Z_FINISH unfinished: deflate() returns Z_OK, not Z_STREAM_END.
         * nxt_zlib_deflate() must report that as an error.  Returning the
         * bytes written would send a truncated body as a success.
         */
        nxt_memzero(&cctx, sizeof(cctx));
        cctx.level = 6;

        NXT_TEST_CHECK(ctx->thr->log,
                       nxt_http_comp_gzip_ops.init(&cctx) == 0,
                       "gzip init failed");

        n = nxt_http_comp_gzip_ops.deflate(&cctx, str, sizeof(str) - 1,
                                           out, sizeof(out), true);
        nxt_http_comp_gzip_ops.free(&cctx);

        NXT_TEST_CHECK(ctx->thr->log, n == -1,
                       "gzip of %uz bytes into %uz gave %z, not -1",
                       sizeof(str) - 1, sizeof(out), n);
    }

#if (NXT_64BIT) && defined(MAP_NORESERVE)
    {
        ssize_t                         n, exact;
        uint8_t                         out[1024], *map;
        nxt_http_comp_compressor_ctx_t  cctx;

        static const uint8_t  str[] = "the gzip output length is clamped";

        /* The low 32 bits of this length are zero. */
        static const size_t  map_len = (size_t) UINT_MAX + 1;

        /*
         * nxt_zlib_deflate() clamps an output length above UINT_MAX to
         * UINT_MAX, the most that avail_out can hold.  The plain conversion
         * kept the low 32 bits, zero here, so deflate() had no room and the
         * call gave -1.  The mapping is reserved, not backed: only the pages
         * written cost memory.  A platform that refuses it skips the check.
         */
        nxt_memzero(&cctx, sizeof(cctx));
        cctx.level = 6;

        NXT_TEST_CHECK(ctx->thr->log,
                       nxt_http_comp_gzip_ops.init(&cctx) == 0,
                       "gzip init failed");

        exact = nxt_http_comp_gzip_ops.deflate(&cctx, str, sizeof(str) - 1,
                                               out, sizeof(out), true);
        nxt_http_comp_gzip_ops.free(&cctx);

        NXT_TEST_CHECK(ctx->thr->log, exact > 0,
                       "gzip of %uz bytes gave %z", sizeof(str) - 1, exact);

        map = nxt_mem_mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | NXT_MEM_MAP_ANON | MAP_NORESERVE,
                           -1, 0);

        if (map == MAP_FAILED) {
            nxt_log_error(NXT_LOG_NOTICE, ctx->thr->log,
                          "gzip clamp check skipped: mmap(%uz) failed %E",
                          map_len, nxt_errno);

        } else {
            nxt_memzero(&cctx, sizeof(cctx));
            cctx.level = 6;

            NXT_TEST_CHECK(ctx->thr->log,
                           nxt_http_comp_gzip_ops.init(&cctx) == 0,
                           "gzip init failed");

            n = nxt_http_comp_gzip_ops.deflate(&cctx, str, sizeof(str) - 1,
                                               map, map_len, true);
            nxt_http_comp_gzip_ops.free(&cctx);
            nxt_mem_munmap(map, map_len);

            NXT_TEST_CHECK(ctx->thr->log, n == exact,
                           "gzip with out_len %uz gave %z, not %z",
                           map_len, n, exact);
        }
    }
#endif
#endif

    return NXT_OK;
}


nxt_int_t
nxt_http_comp_select_test(nxt_thread_t *thr)
{
    size_t              len;
    u_char              buf[NXT_HCS_MAX];
    uint32_t            state;
    nxt_int_t           ret;
    nxt_uint_t          t1, w1, s, a, b, i, n, k;
    nxt_hcs_ctx_t       ctx;
    nxt_http_request_t  *r;

    static const struct {
        const char  *value;
        size_t      length;
    } specials[] = {
        { "", 0 },
        { " ", 1 },
        { "\t", 1 },
        { ",", 1 },
        { ";", 1 },
        { "identity\0;q=0", 13 },
        { "*\0;q=0", 6 },
        { "gzip\0", 5 },
        { "\0", 1 },
    };

    r = nxt_zalloc(sizeof(nxt_http_request_t));
    if (nxt_slow_path(r == NULL)) {
        return NXT_ERROR;
    }

    ctx.thr = thr;
    ctx.r = r;
    ctx.count = 0;
    ctx.skipped = 0;

    ret = NXT_OK;

    for (i = 0; ret == NXT_OK && i < nxt_nitems(specials); i++) {
        ret = nxt_hcs_compare(&ctx, (u_char *) specials[i].value,
                              specials[i].length);
    }

    /* One element, with blanks before and after it. */

    for (t1 = 0; ret == NXT_OK && t1 < nxt_nitems(nxt_hcs_tokens); t1++) {
        for (w1 = 0; ret == NXT_OK && w1 < nxt_nitems(nxt_hcs_weights); w1++) {
            for (a = 0; ret == NXT_OK && a < nxt_nitems(nxt_hcs_spaces); a++) {
                for (b = 0; ret == NXT_OK && b < nxt_nitems(nxt_hcs_spaces);
                     b++)
                {
                    len = nxt_hcs_append(buf, 0, nxt_hcs_spaces[a]);
                    len = nxt_hcs_element(buf, len, t1, w1);
                    len = nxt_hcs_append(buf, len, nxt_hcs_spaces[b]);

                    ret = nxt_hcs_compare(&ctx, buf, len);
                }
            }
        }
    }

    /* Each pair of elements, with each of the first two separators. */

    n = nxt_nitems(nxt_hcs_tokens) * nxt_nitems(nxt_hcs_weights);

    for (i = 0; ret == NXT_OK && i < n * n * 2; i++) {
        a = i / 2 / n;
        b = i / 2 % n;
        s = i % 2;

        len = nxt_hcs_element(buf, 0, a / nxt_nitems(nxt_hcs_weights),
                              a % nxt_nitems(nxt_hcs_weights));
        len = nxt_hcs_append(buf, len, nxt_hcs_seps[s]);
        len = nxt_hcs_element(buf, len, b / nxt_nitems(nxt_hcs_weights),
                              b % nxt_nitems(nxt_hcs_weights));

        ret = nxt_hcs_compare(&ctx, buf, len);
    }

    /*
     * Each sequence of one to four tokens without a weight: the values that
     * the shortcut takes with compression off.  The separators rotate.
     */

    for (n = 1; n <= 4; n++) {
        b = 1;

        for (k = 0; k < n; k++) {
            b *= nxt_nitems(nxt_hcs_tokens);
        }

        for (i = 0; ret == NXT_OK && i < b; i++) {
            len = 0;
            a = i;

            for (k = 0; k < n; k++) {
                if (k != 0) {
                    s = (i + k) % nxt_nitems(nxt_hcs_seps);
                    len = nxt_hcs_append(buf, len, nxt_hcs_seps[s]);
                }

                len = nxt_hcs_element(buf, len,
                                      a % nxt_nitems(nxt_hcs_tokens), 0);
                a /= nxt_nitems(nxt_hcs_tokens);
            }

            ret = nxt_hcs_compare(&ctx, buf, len);
        }
    }

    /* Random values of three to six elements, with blanks and separators. */

    state = 2026;

    for (i = 0; ret == NXT_OK && i < NXT_HCS_RANDOM; i++) {
        n = 3 + nxt_hcs_random(&state) % 4;
        len = 0;

        for (k = 0; k < n; k++) {
            if (k != 0) {
                len = nxt_hcs_append(buf, len,
                                     nxt_hcs_seps[nxt_hcs_random(&state)
                                                  % nxt_nitems(nxt_hcs_seps)]);
            }

            len = nxt_hcs_append(buf, len,
                                 nxt_hcs_spaces[nxt_hcs_random(&state)
                                                % nxt_nitems(nxt_hcs_spaces)]);

            t1 = nxt_hcs_random(&state) % nxt_nitems(nxt_hcs_tokens);

            /* Half of the elements have no weight. */

            w1 = nxt_hcs_random(&state) % (2 * nxt_nitems(nxt_hcs_weights));
            w1 = (w1 < nxt_nitems(nxt_hcs_weights)) ? w1 : 0;

            len = nxt_hcs_element(buf, len, t1, w1);

            len = nxt_hcs_append(buf, len,
                                 nxt_hcs_spaces[nxt_hcs_random(&state)
                                                % nxt_nitems(nxt_hcs_spaces)]);
        }

        ret = nxt_hcs_compare(&ctx, buf, len);
    }

    if (ret == NXT_OK) {
        ret = nxt_hcs_expect(&ctx);
    }

    if (r->mem_pool != NULL) {
        nxt_mp_destroy(r->mem_pool);
    }

    nxt_free(r);

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "http comp select test passed: %ui values, "
                      "%ui without ';'",
                      ctx.count, ctx.skipped);
    }

    return ret;
}
