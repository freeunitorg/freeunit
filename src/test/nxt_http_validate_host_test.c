
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include <nxt_http.h>
#include "nxt_tests.h"


/*
 * nxt_http_validate_host() checks and normalises the value of a Host
 * field.  It is shared with other frontends, which validate the
 * :authority pseudo-header the same way, so the table pins the contract:
 * the port is cut off, a trailing dot is dropped, upper case is lowered
 * into a pool copy, and an empty label or a slash is a 400.
 */

nxt_int_t
nxt_http_validate_host_test(nxt_thread_t *thr)
{
    nxt_mp_t    *mp;
    nxt_str_t   host;
    nxt_int_t   ret;
    nxt_uint_t  i;

    static const struct {
        nxt_str_t  input;
        nxt_str_t  output;
        nxt_int_t  ret;
    } tests[] = {
        { nxt_string("example.com"),        nxt_string("example.com"), NXT_OK },
        { nxt_string("Example.COM"),        nxt_string("example.com"), NXT_OK },
        { nxt_string("example.com."),       nxt_string("example.com"), NXT_OK },
        { nxt_string("example.com:8080"),   nxt_string("example.com"), NXT_OK },
        { nxt_string("example.com.:8080"),  nxt_string("example.com"), NXT_OK },
        { nxt_string("[::1]:8080"),         nxt_string("[::1]"),       NXT_OK },
        { nxt_string("[::1]"),              nxt_string("[::1]"),       NXT_OK },
        { nxt_string(""),                   nxt_string(""),            NXT_OK },
        { nxt_string("a..b"),               nxt_null_string,
                                                     NXT_HTTP_BAD_REQUEST },
        { nxt_string("example.com/path"),   nxt_null_string,
                                                     NXT_HTTP_BAD_REQUEST },
        { nxt_string("[::1]/"),             nxt_null_string,
                                                     NXT_HTTP_BAD_REQUEST },
    };

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    for (i = 0; i < nxt_nitems(tests); i++) {
        host = tests[i].input;

        ret = nxt_http_validate_host(&host, mp);

        if (ret != tests[i].ret) {
            nxt_log_alert(thr->log, "nxt_http_validate_host(\"%V\") test "
                          "failed: %i expected %i",
                          &tests[i].input, ret, tests[i].ret);
            goto fail;
        }

        if (ret != NXT_OK) {
            continue;
        }

        if (!nxt_strstr_eq(&host, &tests[i].output)) {
            nxt_log_alert(thr->log, "nxt_http_validate_host(\"%V\") test "
                          "failed: \"%V\" expected \"%V\"",
                          &tests[i].input, &host, &tests[i].output);
            goto fail;
        }
    }

    nxt_mp_destroy(mp);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "http validate host test passed");

    return NXT_OK;

fail:

    nxt_mp_destroy(mp);

    return NXT_ERROR;
}
