/*
 * Copyright (C) FreeUnit contributors.
 */

#include <nxt_main.h>
#include "nxt_tests.h"


/* An empty nxt_str_t has a NULL start: memcpy() must not be given it. */

nxt_int_t
nxt_buf_test(nxt_thread_t *thr)
{
    nxt_int_t         ret;
    nxt_mp_t          *mp;
    nxt_buf_t         *b;
    static nxt_str_t  empty = nxt_null_string;

    mp = nxt_mp_create(1024, 128, 512, 16);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    ret = NXT_ERROR;

    b = nxt_buf_mem_alloc(mp, 1, 0);

    if (b != NULL) {
        nxt_buf_cpystr(b, &empty);

        if (nxt_buf_mem_used_size(&b->mem) == 0) {
            ret = NXT_OK;
        }
    }

    nxt_mp_destroy(mp);

    if (ret != NXT_OK) {
        nxt_log_alert(thr->log, "nxt_buf test failed");
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_buf test passed");

    return NXT_OK;
}
