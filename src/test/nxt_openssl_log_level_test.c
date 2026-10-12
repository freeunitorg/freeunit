
/*
 * Copyright (C) FreeUnit Community
 */

/*
 * Pins the bounds of the alert range in nxt_openssl_log_error_level()
 * (src/nxt_openssl.c).
 *
 * An SSL error whose reason is in no case label takes the default branch.
 * There a reason from SSL_AD_REASON_OFFSET to SSL_AD_REASON_OFFSET + 255 is
 * an alert from the peer and logs at info.  Any other reason logs at alert.
 *
 * A peer can send only alert codes 0 to 255, so the alert tests in
 * test/test_tls.py reach the range only from the inside.  Here
 * the NXT_TESTS hook nxt_openssl_test_log_level() puts an SSL error with the
 * given reason in the OpenSSL error queue and returns the level.
 *
 * Reason 1000 (close_notify) is a case label that logs at info, so it
 * never reaches the range check.  The other four reasons are in no case
 * label and take the default branch: 999 and 1256 just outside the range,
 * 1001 and 1255 at its two inside edges.
 */

#include <nxt_main.h>
#include "nxt_tests.h"

#include <openssl/ssl.h>


typedef struct {
    int         reason;
    nxt_uint_t  level;
} nxt_openssl_log_level_case_t;


static const nxt_openssl_log_level_case_t  nxt_openssl_log_level_cases[] = {
    { SSL_AD_REASON_OFFSET - 1,    NXT_LOG_ALERT },
    { SSL_AD_REASON_OFFSET,        NXT_LOG_INFO },
    { SSL_AD_REASON_OFFSET + 1,    NXT_LOG_INFO },
    { SSL_AD_REASON_OFFSET + 255,  NXT_LOG_INFO },
    { SSL_AD_REASON_OFFSET + 256,  NXT_LOG_ALERT },
};


nxt_int_t
nxt_openssl_log_level_test(nxt_thread_t *thr)
{
    nxt_uint_t                          i, level;
    const nxt_openssl_log_level_case_t  *c;

    nxt_thread_time_update(thr);

    for (i = 0; i < nxt_nitems(nxt_openssl_log_level_cases); i++) {
        c = &nxt_openssl_log_level_cases[i];

        level = nxt_openssl_test_log_level(c->reason);

        NXT_TEST_CHECK(thr->log, level == c->level,
                       "openssl log level test failed: reason %d "
                       "gave level %ui, expected %ui (0 alert, 4 info)",
                       c->reason, level, c->level);
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "openssl log level test passed");

    return NXT_OK;
}
