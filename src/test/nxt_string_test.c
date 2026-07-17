
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include "nxt_tests.h"


nxt_int_t
nxt_string_test(nxt_thread_t *thr)
{
    u_char      *p;
    nxt_uint_t  i;
    nxt_bool_t  enc;

    static const u_char  abc[] = "abc";

    /*
     * nxt_rmemstrn(): backward substring search. The length guard
     * (length == 0 || length > buffer) must reject out-of-range lookups
     * without an over-read / size_t underflow (hardening 740e9bd5).
     */
    static const struct {
        nxt_str_t  haystack;
        nxt_str_t  needle;
        nxt_int_t  offset;   /* expected match offset, or -1 for NULL */
    } rmem[] = {
        { nxt_string("abcXYZdef"), nxt_string("XYZ"),  3 },
        { nxt_string("abcXYZdef"), nxt_string("def"),  6 },
        { nxt_string("aXYbXYc"),   nxt_string("XY"),   4 },  /* last match */
        { nxt_string("abcdef"),    nxt_string("QQ"),  -1 },  /* not found */
        { nxt_string("ab"),        nxt_string("XYZ"), -1 },  /* len > buffer */
    };

    /*
     * nxt_is_complex_uri_encoded(): a trailing '%' with fewer than two hex
     * digits must be rejected before reading src+1/src+2 (off-by-one fix
     * 740e9bd5 / V13: "end - src < 3").
     */
    static const struct {
        nxt_str_t   uri;
        nxt_bool_t  encoded;
    } cplx[] = {
        { nxt_string("/some/path"), 1 },
        { nxt_string("%2F"),        1 },
        { nxt_string("a%20b"),      1 },
        { nxt_string("%2"),         0 },   /* off-by-one guard: 2 bytes */
        { nxt_string("%"),          0 },   /* 1 byte */
        { nxt_string("%ZZ"),        0 },   /* invalid hex */
        { nxt_string(" "),          0 },   /* space must be escaped */
    };

    nxt_thread_time_update(thr);

    for (i = 0; i < nxt_nitems(rmem); i++) {
        p = nxt_rmemstrn(rmem[i].haystack.start,
                         rmem[i].haystack.start + rmem[i].haystack.length,
                         (char *) rmem[i].needle.start, rmem[i].needle.length);

        if (rmem[i].offset < 0) {
            if (p != NULL) {
                nxt_log_alert(thr->log, "nxt_rmemstrn(\"%V\", \"%V\") failed: "
                              "expected NULL", &rmem[i].haystack,
                              &rmem[i].needle);
                return NXT_ERROR;
            }

            continue;
        }

        if (p != rmem[i].haystack.start + rmem[i].offset) {
            nxt_log_alert(thr->log, "nxt_rmemstrn(\"%V\", \"%V\") failed: "
                          "wrong offset", &rmem[i].haystack, &rmem[i].needle);
            return NXT_ERROR;
        }
    }

    /* Zero-length needle must be rejected (no size_t underflow). */

    if (nxt_rmemstrn(abc, abc + 3, "", 0) != NULL) {
        nxt_log_alert(thr->log, "nxt_rmemstrn() zero-length test failed");
        return NXT_ERROR;
    }

    for (i = 0; i < nxt_nitems(cplx); i++) {
        enc = nxt_is_complex_uri_encoded(cplx[i].uri.start, cplx[i].uri.length);

        if (enc != cplx[i].encoded) {
            nxt_log_alert(thr->log, "nxt_is_complex_uri_encoded(\"%V\") failed: "
                          "got %d, expected %d", &cplx[i].uri, (int) enc,
                          (int) cplx[i].encoded);
            return NXT_ERROR;
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_string helpers test passed");

    return NXT_OK;
}
