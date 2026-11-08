/*
 * Copyright (C) FreeUnit contributors.
 */

#include <nxt_main.h>
#include "nxt_tests.h"


/*
 * nxt_var_escape() tests eight bytes at a time.  The test compares it and
 * nxt_var_escape_length() with a byte loop, for every byte value at every
 * position of a word, for every pair of neighbouring byte values, and for
 * random strings, each length at every alignment.
 */

static u_char *
nxt_var_escape_test_ref(u_char *dst, const u_char *src, size_t length)
{
    u_char  c;
    size_t  i;

    static const u_char  hex[] = "0123456789ABCDEF";

    for (i = 0; i < length; i++) {
        c = src[i];

        if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') {
            *dst++ = c;
            continue;
        }

        *dst++ = '\\';
        *dst++ = 'x';
        *dst++ = hex[c >> 4];
        *dst++ = hex[c & 0xf];
    }

    return dst;
}


static nxt_int_t
nxt_var_escape_test_one(nxt_thread_t *thr, const u_char *src, size_t length)
{
    u_char  *p, *q;
    size_t  size;
    u_char  got[4 * 64], want[4 * 64];

    q = nxt_var_escape_test_ref(want, src, length);

    size = nxt_var_escape_length(src, length);

    if (size != (size_t) (q - want)) {
        nxt_log_alert(thr->log, "nxt_var_escape_length() test failed: "
                      "%uz, not %uz", size, (size_t) (q - want));
        return NXT_ERROR;
    }

    p = nxt_var_escape(got, src, length);

    if (p - got != q - want || memcmp(got, want, q - want) != 0) {
        nxt_log_alert(thr->log, "nxt_var_escape() test failed");
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * Each string is copied to the end of a buffer of exactly its size, so that
 * ASan reports a read past the end of a value.
 */

static nxt_int_t
nxt_var_escape_test_at(nxt_thread_t *thr, const u_char *src, size_t length)
{
    u_char     *p;
    nxt_int_t  ret;

    p = nxt_malloc(length + 1);
    if (nxt_slow_path(p == NULL)) {
        return NXT_ERROR;
    }

    nxt_memcpy(p, src, length);
    ret = nxt_var_escape_test_one(thr, p, length);

    nxt_free(p);

    return ret;
}


nxt_int_t
nxt_var_escape_test(nxt_thread_t *thr)
{
    u_char      buf[64 + 8];
    uint32_t    seed;
    nxt_uint_t  c, d, i, n, offset, length;

    /* An absent value. */

    if (nxt_var_escape_test_one(thr, NULL, 0) != NXT_OK) {
        return NXT_ERROR;
    }

    /* Every byte value at every position of a 24-byte string. */

    for (c = 0; c < 256; c++) {
        for (i = 0; i < 24; i++) {
            nxt_memset(buf, 'a', 24);
            buf[i] = (u_char) c;

            if (nxt_var_escape_test_at(thr, buf, 24) != NXT_OK) {
                return NXT_ERROR;
            }
        }
    }

    /* Every pair of neighbouring byte values, across a word boundary too. */

    for (c = 0; c < 256; c++) {
        for (d = 0; d < 256; d++) {
            for (i = 6; i < 9; i++) {
                nxt_memset(buf, 'a', 16);
                buf[i] = (u_char) c;
                buf[i + 1] = (u_char) d;

                if (nxt_var_escape_test_at(thr, buf, 16) != NXT_OK) {
                    return NXT_ERROR;
                }
            }
        }
    }

    /* Random strings of 0 to 63 bytes, each length at every alignment. */

    seed = 1;

    for (n = 0; n < 20000; n++) {
        offset = (n / 64) % 8;
        length = n % 64;

        for (i = 0; i < length; i++) {
            seed = seed * 1103515245 + 12345;

            /* Mostly printable bytes, as in a header value. */
            c = (seed >> 16) & 0xff;
            buf[offset + i] = ((seed >> 8) & 7) ? (u_char) (0x20 + c % 0x5f)
                                                : (u_char) c;
        }

        if (nxt_var_escape_test_one(thr, buf + offset, length) != NXT_OK
            || nxt_var_escape_test_at(thr, buf + offset, length) != NXT_OK)
        {
            return NXT_ERROR;
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_var_escape() test passed");

    return NXT_OK;
}
