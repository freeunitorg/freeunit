
/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include "nxt_tests.h"


/*
 * nxt_utf8_sanitize() is what keeps a JSON access log record readable when
 * the value came from the request: a byte that begins no valid sequence has
 * no JSON representation at all (RFC 8259 Sect. 8.1 makes JSON text UTF-8),
 * so it is replaced with U+FFFD rather than escaped or copied.
 *
 * The strings below are the ones where a sizing pass and a copying pass can
 * drift apart -- bytes kept, bytes replaced, and sequences that decode to
 * nothing -- because the function walks the input twice and the second walk
 * fills a buffer the first one sized.
 */

/*
 * A strict UTF-8 test written from the definition rather than from
 * src/nxt_utf8.c, which is the decoder the escaper itself uses.  Asserting
 * with that decoder would only prove the escaper agrees with itself: its
 * final gate is "overlong < u && u < 0x110000", so it accepts the surrogate
 * range, and a surrogate copied into the output would satisfy an assertion
 * built on it while a real consumer rejected the record.  Unicode 15.0
 * Sect. 3.9, table 3-7.
 */

static nxt_bool_t
nxt_utf8_sanitize_test_utf8(const u_char *p, size_t len)
{
    uint32_t      u, min;
    nxt_uint_t    n;
    const u_char  *end;

    end = p + len;

    while (p < end) {

        if (*p < 0x80) {
            p++;
            continue;
        }

        if (*p >= 0xF0) {
            if (*p > 0xF4) {
                return 0;
            }

            u = *p & 0x07;
            n = 3;
            min = 0x10000;

        } else if (*p >= 0xE0) {
            u = *p & 0x0F;
            n = 2;
            min = 0x800;

        } else if (*p >= 0xC2) {
            u = *p & 0x1F;
            n = 1;
            min = 0x80;

        } else {
            /* A continuation byte on its own, or an overlong C0/C1 lead. */
            return 0;
        }

        if (p + n >= end) {
            return 0;
        }

        p++;

        while (n != 0) {
            if ((*p & 0xC0) != 0x80) {
                return 0;
            }

            u = (u << 6) | (*p & 0x3F);
            p++;
            n--;
        }

        /* Shortest form, in range, and not a surrogate. */

        if (u < min || u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) {
            return 0;
        }
    }

    return 1;
}


static const struct {
    const char  *name;
    const char  *data;
    size_t      length;
} nxt_utf8_sanitize_cases[] = {
    { "plain",              "hello",                    5 },
    { "quote",              "a\"b",                     3 },
    { "backslash",          "a\\b",                     3 },
    { "named control",      "\n\r\t\b\f",               5 },
    { "numeric control",    "\x01\x02\x1F",             3 },
    { "nul",                "\x00",                     1 },
    { "two-byte utf8",      "\xC3\xA9",                 2 },
    { "three-byte utf8",    "\xE2\x82\xAC",             3 },
    { "four-byte utf8",     "\xF0\x9F\x92\xA9",         4 },
    { "lone 0xFF",          "\xFF",                     1 },
    { "lone continuation",  "\x80",                     1 },
    { "truncated two-byte", "\xC3",                     1 },
    { "truncated four-byte","\xF0\x9F",                 2 },
    { "overlong",           "\xC0\xAF",                 2 },
    { "surrogate",          "\xED\xA0\x80",             3 },
    { "above 0x10FFFF",     "\xF5\x80\x80\x80",         4 },
    { "invalid then quote", "\xFF\"",                   2 },
    { "invalid run",        "\xFF\xFE\xFD",             3 },
    { "mixed",              "a\xC3\xA9\xFF\n\"z",       7 },
};


nxt_int_t
nxt_utf8_sanitize_test(nxt_thread_t *thr)
{
    nxt_mp_t    *mp;
    nxt_str_t   src, dst, alias;
    nxt_int_t   ret;
    nxt_uint_t  i;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "utf8 sanitize test started");

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        nxt_log_alert(thr->log, "utf8 sanitize test: nxt_mp_create() failed");
        return NXT_ERROR;
    }

    for (i = 0; i < nxt_nitems(nxt_utf8_sanitize_cases); i++) {

        src.start = (u_char *) nxt_utf8_sanitize_cases[i].data;
        src.length = nxt_utf8_sanitize_cases[i].length;

        dst.start = NULL;
        dst.length = 0;

        ret = nxt_utf8_sanitize(mp, &dst, &src);
        if (ret != NXT_OK) {
            nxt_log_alert(thr->log, "utf8 sanitize test: \"%s\" failed",
                          nxt_utf8_sanitize_cases[i].name);
            goto fail;
        }

        if (!nxt_utf8_sanitize_test_utf8(dst.start, dst.length)) {
            nxt_log_alert(thr->log, "utf8 sanitize test: \"%s\" produced "
                          "invalid UTF-8: \"%*s\"",
                          nxt_utf8_sanitize_cases[i].name,
                          (size_t) dst.length, dst.start);
            goto fail;
        }

        /*
         * The access log sanitizes in place -- nxt_utf8_sanitize(mp, &s, &s)
         * -- so the aliased call is the one that matters, and it is the one
         * a two-pass implementation gets wrong by writing "dst" before the
         * second pass has read "src".
         */

        alias = src;

        ret = nxt_utf8_sanitize(mp, &alias, &alias);
        if (ret != NXT_OK) {
            nxt_log_alert(thr->log, "utf8 sanitize test: \"%s\" failed "
                          "in place", nxt_utf8_sanitize_cases[i].name);
            goto fail;
        }

        if (alias.length != dst.length
            || memcmp(alias.start, dst.start, dst.length) != 0)
        {
            nxt_log_alert(thr->log, "utf8 sanitize test: \"%s\" in place "
                          "gave \"%*s\", not the \"%*s\" of the separate "
                          "call", nxt_utf8_sanitize_cases[i].name,
                          (size_t) alias.length, alias.start,
                          (size_t) dst.length, dst.start);
            goto fail;
        }

        /*
         * A string that needs nothing done to it must come back as itself,
         * not as a copy: the ordinary request logs no invalid bytes, and
         * allocating per value per request would be paid on every one.
         */

        if (nxt_utf8_sanitize_test_utf8(src.start, src.length)
            && dst.start != src.start)
        {
            nxt_log_alert(thr->log, "utf8 sanitize test: \"%s\" copied a "
                          "string that was already valid",
                          nxt_utf8_sanitize_cases[i].name);
            goto fail;
        }
    }

    nxt_mp_destroy(mp);

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "utf8 sanitize test passed");

    return NXT_OK;

fail:

    nxt_mp_destroy(mp);

    return NXT_ERROR;
}
