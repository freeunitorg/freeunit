
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include "nxt_tests.h"


/*
 * nxt_punycode_encode() against samples from RFC 3492 section 7.1 and
 * a few Python "punycode" codec results, then nxt_punycode_host() for
 * the label rules of a host match pattern.  The source is UTF-8.
 */

#define NXT_PUNYCODE_TEST_10U  "üüüüüüüüüü"


nxt_int_t
nxt_punycode_test(nxt_thread_t *thr)
{
    u_char      buf[NXT_PUNYCODE_MAX_LABEL];
    ssize_t     len;
    nxt_mp_t    *mp;
    nxt_str_t   host, out;
    nxt_int_t   ret;
    nxt_uint_t  i;
    const char  *error;

    static const struct {
        nxt_str_t  input;
        nxt_str_t  output;
    } labels[] = {
        /* RFC 3492 (A) Arabic (Egyptian). */
        { nxt_string("ليهمابتكلموشعربي؟"),
          nxt_string("egbpdaj6bu4bxfgehfvwxn") },
        /* RFC 3492 (K) Japanese, mixed-case basic code points kept. */
        { nxt_string("Hello-Another-Way-それぞれの場所"),
          nxt_string("Hello-Another-Way--fc4qua05auwb3674vfr0b") },
        /* RFC 3492 (L) Japanese. */
        { nxt_string("3年B組金八先生"),
          nxt_string("3B-ww4c5e180e575a65lsy2b") },
        /* RFC 3492 (S) all basic: a trailing delimiter only. */
        { nxt_string("-> $1.00 <-"),  nxt_string("-> $1.00 <--") },
        { nxt_string("münchen"),      nxt_string("mnchen-3ya") },
        { nxt_string("München"),      nxt_string("Mnchen-3ya") },
        { nxt_string("bücher"),       nxt_string("bcher-kva") },
        { nxt_string("ü"),            nxt_string("tda") },
        { nxt_string("пример"),       nxt_string("e1afmkfd") },
        { nxt_string("ascii"),        nxt_string("ascii-") },
        { nxt_string("рф"),           nxt_string("p1ai") },
        { nxt_string("日本語"),       nxt_string("wgv71a119e") },
        /* A 4-byte UTF-8 character. */
        { nxt_string("😀"),           nxt_string("e28h") },
        /* A basic "-" before the delimiter. */
        { nxt_string("a-ü"),          nxt_string("a--yka") },
        { nxt_string(""),             nxt_string("") },
        /* Invalid UTF-8: a lone continuation byte, a truncated sequence. */
        { nxt_string("\xbf"),         nxt_null_string },
        { nxt_string("m\xc3"),        nxt_null_string },
        /* More code points than a label may hold. */
        { nxt_string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
          nxt_null_string },
    };

    static const struct {
        nxt_str_t  input;
        nxt_str_t  output;
    } hosts[] = {
        { nxt_string("example.com"),       nxt_string("example.com") },
        { nxt_string("münchen.example"),
          nxt_string("xn--mnchen-3ya.example") },
        /* Case is left to the caller; the delta does not depend on it. */
        { nxt_string("München.Example"),
          nxt_string("xn--Mnchen-3ya.Example") },
        { nxt_string("*.münchen.example"),
          nxt_string("*.xn--mnchen-3ya.example") },
        { nxt_string("münchen.*"),         nxt_string("xn--mnchen-3ya.*") },
        /* A label already in punycode passes through. */
        { nxt_string("xn--mnchen-3ya.example"),
          nxt_string("xn--mnchen-3ya.example") },
        { nxt_string("münchen.xn--e1afmkfd.example"),
          nxt_string("xn--mnchen-3ya.xn--e1afmkfd.example") },
        { nxt_string("ü"),                 nxt_string("xn--tda") },
        /* Empty labels and a trailing dot are kept as written. */
        { nxt_string("a..ü."),             nxt_string("a..xn--tda.") },
        { nxt_string(".ü"),                nxt_string(".xn--tda") },
        /* A wildcard inside a non-ASCII label. */
        { nxt_string("mün*chen.example"),  nxt_null_string },
        { nxt_string("*ünchen.example"),   nxt_null_string },
        { nxt_string("münchen*"),          nxt_null_string },
        { nxt_string("\xbf.example"),      nxt_null_string },
        /* 60 code points: 66 bytes with "xn--", over the 63 of a label. */
        { nxt_string(NXT_PUNYCODE_TEST_10U NXT_PUNYCODE_TEST_10U
                     NXT_PUNYCODE_TEST_10U NXT_PUNYCODE_TEST_10U
                     NXT_PUNYCODE_TEST_10U NXT_PUNYCODE_TEST_10U ".example"),
          nxt_null_string },
    };

    for (i = 0; i < nxt_nitems(labels); i++) {
        len = nxt_punycode_encode(buf, sizeof(buf), labels[i].input.start,
                                  labels[i].input.length);

        if (labels[i].output.start == NULL) {
            if (len != -1) {
                nxt_log_alert(thr->log, "nxt_punycode_encode(\"%V\") test "
                              "failed: %z expected -1", &labels[i].input, len);
                return NXT_ERROR;
            }

            continue;
        }

        if (len < 0 || (size_t) len != labels[i].output.length
            || memcmp(buf, labels[i].output.start, len) != 0)
        {
            out.start = buf;
            out.length = (len < 0) ? 0 : len;

            nxt_log_alert(thr->log, "nxt_punycode_encode(\"%V\") test "
                          "failed: \"%V\" expected \"%V\"",
                          &labels[i].input, &out, &labels[i].output);
            return NXT_ERROR;
        }
    }

    /* An output buffer too small for the label. */
    len = nxt_punycode_encode(buf, 5, (const u_char *) "münchen",
                              nxt_length("münchen"));
    if (len != -1) {
        nxt_log_alert(thr->log, "nxt_punycode_encode() size test failed: "
                      "%z expected -1", len);
        return NXT_ERROR;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    for (i = 0; i < nxt_nitems(hosts); i++) {
        host = hosts[i].input;
        error = NULL;

        ret = nxt_punycode_host(mp, &host, &error);

        if (hosts[i].output.start == NULL) {
            if (ret != NXT_ERROR || error == NULL) {
                nxt_log_alert(thr->log, "nxt_punycode_host(\"%V\") test "
                              "failed: %i expected an error",
                              &hosts[i].input, ret);
                goto fail;
            }

            continue;
        }

        if (ret != NXT_OK || !nxt_strstr_eq(&host, &hosts[i].output)) {
            nxt_log_alert(thr->log, "nxt_punycode_host(\"%V\") test "
                          "failed: \"%V\" expected \"%V\"",
                          &hosts[i].input, &host, &hosts[i].output);
            goto fail;
        }

        /* An ASCII pattern, which is returned as is, is not copied. */
        if (nxt_strstr_eq(&hosts[i].input, &hosts[i].output)
            && host.start != hosts[i].input.start)
        {
            nxt_log_alert(thr->log, "nxt_punycode_host(\"%V\") test "
                          "failed: an ASCII host was copied",
                          &hosts[i].input);
            goto fail;
        }
    }

    nxt_mp_destroy(mp);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "punycode test passed");

    return NXT_OK;

fail:

    nxt_mp_destroy(mp);

    return NXT_ERROR;
}
