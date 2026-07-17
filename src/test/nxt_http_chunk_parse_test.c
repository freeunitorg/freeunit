
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include "nxt_tests.h"


nxt_int_t
nxt_http_chunk_parse_test(nxt_thread_t *thr)
{
    nxt_uint_t              i;
    nxt_http_chunk_parse_t  hcp;
    nxt_buf_t               b;

    /*
     * Only error / terminal inputs are exercised: they are resolved inside the
     * parser's inner loop (goto chunk_error / return), before any data buffer
     * is allocated from mem_pool or the work queue is touched -- so a bare
     * stack nxt_buf_t and the test task suffice. The chunk-size overflow cases
     * guard nxt_size_is_sufficient() against a wrapped size (hardening).
     */
    static const struct {
        nxt_str_t  input;
        uint8_t    chunk_error;
        uint8_t    last;      /* terminal 0-chunk seen */
    } tests[] = {
        { nxt_string("0\r\n\r\n"),             0, 1 },  /* clean last-chunk */
        { nxt_string("z\r\n"),                 1, 0 },  /* non-hex size */
        { nxt_string("g0\r\n"),                1, 0 },  /* non-hex size 2 */
        { nxt_string("1ffffffffffffffff\r\n"), 1, 0 },  /* size overflow */
        { nxt_string("10000000000000000\r\n"), 1, 0 },  /* size overflow */
    };

    nxt_thread_time_update(thr);

    for (i = 0; i < nxt_nitems(tests); i++) {
        nxt_memzero(&hcp, sizeof(nxt_http_chunk_parse_t));
        nxt_memzero(&b, sizeof(nxt_buf_t));

        b.mem.pos = tests[i].input.start;
        b.mem.free = tests[i].input.start + tests[i].input.length;

        (void) nxt_http_chunk_parse(thr->task, &hcp, &b);

        if (hcp.chunk_error != tests[i].chunk_error
            || hcp.last != tests[i].last)
        {
            nxt_log_alert(thr->log,
                          "nxt_http_chunk_parse(\"%V\") test failed: "
                          "chunk_error=%d/%d last=%d/%d (got/expected)",
                          &tests[i].input, (int) hcp.chunk_error,
                          (int) tests[i].chunk_error, (int) hcp.last,
                          (int) tests[i].last);
            return NXT_ERROR;
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "nxt_http_chunk_parse() test passed");

    return NXT_OK;
}
