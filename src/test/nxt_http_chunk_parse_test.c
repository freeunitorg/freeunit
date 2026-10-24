
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include "nxt_tests.h"


static void
nxt_http_chunk_test_buf_completion(nxt_task_t *task, void *obj, void *data)
{
    nxt_uint_t  *completed = data;

    (*completed)++;
}


nxt_int_t
nxt_http_chunk_parse_test(nxt_thread_t *thr)
{
    nxt_uint_t              i;
    nxt_http_chunk_parse_t  hcp;
    nxt_buf_t               b;

    /*
     * No input carries a nonzero data chunk, so no data buffer is ever
     * allocated from mem_pool -- a bare stack nxt_buf_t and the test task
     * suffice. Error / terminal inputs resolve inside the parser's inner loop
     * (goto chunk_error / return), but the incomplete-terminal and trailer
     * inputs run the buffer to exhaustion, which reaches the end-of-buffer
     * completion-enqueue (nxt_work_queue_add) that the engine-less test task
     * cannot service; setting b.retain nonzero suppresses that enqueue without
     * affecting the state machine under test. The chunk-size overflow cases
     * guard nxt_size_is_sufficient() against a wrapped size (hardening).
     */
    static const struct {
        nxt_str_t  input;
        uint8_t    chunk_error;
        uint8_t    last;      /* message body fully consumed */
    } tests[] = {
        { nxt_string("0\r\n\r\n"),             0, 1 },  /* clean last-chunk */
        { nxt_string("z\r\n"),                 1, 0 },  /* non-hex size */
        { nxt_string("g0\r\n"),                1, 0 },  /* non-hex size 2 */
        { nxt_string("1ffffffffffffffff\r\n"), 1, 0 },  /* size overflow */
        { nxt_string("10000000000000000\r\n"), 1, 0 },  /* size overflow */

        /* Trailer section (RFC 9112 7.1.2) after the terminal 0-chunk. */
        { nxt_string("0\r\nX-Trailer-Test: trailed\r\n\r\n"),
                                               0, 1 },  /* single trailer */
        { nxt_string("0\r\nA: 1\r\nB: 2\r\n\r\n"),
                                               0, 1 },  /* multiple trailers */
        { nxt_string("0\r\nX:\tv\r\n\r\n"),    0, 1 },  /* HTAB in value */
        { nxt_string("0\r\nX-T:v\r\n\r\n"),    0, 1 },  /* no OWS after colon */
        { nxt_string("0\r\nX-T:\r\n\r\n"),     0, 1 },  /* empty value */
        { nxt_string("0\r\nX: \x80\r\n\r\n"),  0, 1 },  /* obs-text in value */

        /*
         * Field-line grammar: a trailer line must be "field-name ':' value"
         * with a tchar name.  A line that never presents a colon is the
         * smuggling case -- printable, CTL-free, and read as the start of the
         * next request by any peer that stops at the terminal CRLF.
         */
        { nxt_string("0\r\nGET /admin HTTP/1.1\r\n\r\n"),
                                               1, 0 },  /* colonless: request */
        { nxt_string("0\r\nX-T\r\n\r\n"),      1, 0 },  /* colonless: name only */
        { nxt_string("0\r\n: v\r\n\r\n"),      1, 0 },  /* empty field-name */
        { nxt_string("0\r\nX-T : v\r\n\r\n"),  1, 0 },  /* SP before colon */
        { nxt_string("0\r\nX@T: v\r\n\r\n"),   1, 0 },  /* non-tchar in name */
        { nxt_string("0\r\n X-T: v\r\n\r\n"),  1, 0 },  /* obs-fold (SP) */
        { nxt_string("0\r\n\tX-T: v\r\n\r\n"), 1, 0 },  /* obs-fold (HTAB) */

        { nxt_string("0\r\nX-T: v\r\n"),       0, 0 },  /* trailer, no final CRLF */
        { nxt_string("0\r\n"),                 0, 0 },  /* incomplete terminal */
        { nxt_string("0\r\n\r"),               0, 0 },  /* incomplete terminal 2 */
    };

    nxt_thread_time_update(thr);

    for (i = 0; i < nxt_nitems(tests); i++) {
        nxt_memzero(&hcp, sizeof(nxt_http_chunk_parse_t));
        nxt_memzero(&b, sizeof(nxt_buf_t));

        b.mem.pos = tests[i].input.start;
        b.mem.free = tests[i].input.start + tests[i].input.length;
        b.retain = 1;

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

    /*
     * Oversized trailer: an unbounded trailer section must be rejected once
     * more than 4096 trailer bytes have been consumed.  Built at runtime
     * because nxt_string() is for literals only.  The oversized run sits in the
     * field *value*, past a well-formed "X: " prefix, so the cap is what
     * rejects it -- a bare run of 'X' would now be refused by the field-line
     * grammar at the CR that ends a name with no colon, and this case would
     * silently stop testing the cap.
     */
    {
        static u_char  buf[8192];
        u_char         *p;

        p = nxt_cpymem(buf, "0\r\nX: ", 6);
        nxt_memset(p, 'v', 5000);
        p += 5000;
        p = nxt_cpymem(p, "\r\n\r\n", 4);

        nxt_memzero(&hcp, sizeof(nxt_http_chunk_parse_t));
        nxt_memzero(&b, sizeof(nxt_buf_t));

        b.mem.pos = buf;
        b.mem.free = p;
        b.retain = 1;

        (void) nxt_http_chunk_parse(thr->task, &hcp, &b);

        if (hcp.chunk_error != 1 || hcp.last != 0) {
            nxt_log_alert(thr->log,
                          "nxt_http_chunk_parse() oversized-trailer test "
                          "failed: chunk_error=%d/1 last=%d/0 (got/expected)",
                          (int) hcp.chunk_error, (int) hcp.last);
            return NXT_ERROR;
        }
    }

    /*
     * Split buffer: a trailer straddling two buffers must resume via the saved
     * hcp->state and only report last==1 once the final CRLF has arrived.
     */
    {
        nxt_buf_t  b1, b2;
        nxt_str_t  part1 = nxt_string("0\r\nX: y");
        nxt_str_t  part2 = nxt_string("\r\n\r\n");

        nxt_memzero(&hcp, sizeof(nxt_http_chunk_parse_t));

        nxt_memzero(&b1, sizeof(nxt_buf_t));
        b1.mem.pos = part1.start;
        b1.mem.free = part1.start + part1.length;
        b1.retain = 1;

        (void) nxt_http_chunk_parse(thr->task, &hcp, &b1);

        if (hcp.chunk_error != 0 || hcp.last != 0) {
            nxt_log_alert(thr->log,
                          "nxt_http_chunk_parse() split-trailer test failed "
                          "after first buffer: chunk_error=%d/0 last=%d/0 "
                          "(got/expected)",
                          (int) hcp.chunk_error, (int) hcp.last);
            return NXT_ERROR;
        }

        nxt_memzero(&b2, sizeof(nxt_buf_t));
        b2.mem.pos = part2.start;
        b2.mem.free = part2.start + part2.length;
        b2.retain = 1;

        (void) nxt_http_chunk_parse(thr->task, &hcp, &b2);

        if (hcp.chunk_error != 0 || hcp.last != 1) {
            nxt_log_alert(thr->log,
                          "nxt_http_chunk_parse() split-trailer test failed "
                          "after second buffer: chunk_error=%d/0 last=%d/1 "
                          "(got/expected)",
                          (int) hcp.chunk_error, (int) hcp.last);
            return NXT_ERROR;
        }
    }

    /*
     * Control-byte rejection across the whole trailer section.  RFC 9110
     * (sec 5.5) forbids CTL bytes -- %x00-1F and %x7F (DEL) -- other than
     * HTAB in a field value; Unit rejects them wherever they appear in a
     * trailer line rather than skipping them.  Sweep every such byte in both
     * positions the parser checks -- mid-value (sw_trailer_value) and as the
     * first byte of a trailer field line (consumed in sw_chunk_end_newline) --
     * asserting every CTL, including NUL and a bare LF, errors.  Built at
     * runtime because a C-string literal cannot carry an embedded NUL.  CR is
     * skipped: it is the structural line terminator, not a field byte.
     *
     * HTAB is the one CTL that differs by position: legal inside a field value,
     * but at the start of a line it is an obs-fold continuation with no
     * field-name, which the grammar check rejects along with SP.
     */
    {
        static u_char  buf[16];
        u_char         *p;
        nxt_uint_t     c;
        uint8_t        expect_err, expect_last;

        for (c = 0; c <= 0xff; c++) {
            if (!(c <= 0x1f || c == 0x7f) || c == '\r') {
                continue;
            }

            expect_err = (c == '\t') ? 0 : 1;
            expect_last = (c == '\t') ? 1 : 0;

            /* Position 1: mid-value -- "0\r\nX: <c>\r\n\r\n". */
            p = nxt_cpymem(buf, "0\r\nX: ", 6);
            *p++ = (u_char) c;
            p = nxt_cpymem(p, "\r\n\r\n", 4);

            nxt_memzero(&hcp, sizeof(nxt_http_chunk_parse_t));
            nxt_memzero(&b, sizeof(nxt_buf_t));
            b.mem.pos = buf;
            b.mem.free = p;
            b.retain = 1;

            (void) nxt_http_chunk_parse(thr->task, &hcp, &b);

            if (hcp.chunk_error != expect_err || hcp.last != expect_last) {
                nxt_log_alert(thr->log,
                              "nxt_http_chunk_parse() ctl-value test failed "
                              "for 0x%02Xd: chunk_error=%d/%d last=%d/%d",
                              (int) c, (int) hcp.chunk_error, (int) expect_err,
                              (int) hcp.last, (int) expect_last);
                return NXT_ERROR;
            }

            /*
             * Position 2: first byte of the line -- "0\r\n<c>: v\r\n\r\n".
             * No CTL opens a field-name, HTAB included: there it is an obs-fold
             * continuation, so unlike position 1 it must error.
             */
            expect_err = 1;
            expect_last = 0;

            p = nxt_cpymem(buf, "0\r\n", 3);
            *p++ = (u_char) c;
            p = nxt_cpymem(p, ": v\r\n\r\n", 7);

            nxt_memzero(&hcp, sizeof(nxt_http_chunk_parse_t));
            nxt_memzero(&b, sizeof(nxt_buf_t));
            b.mem.pos = buf;
            b.mem.free = p;
            b.retain = 1;

            (void) nxt_http_chunk_parse(thr->task, &hcp, &b);

            if (hcp.chunk_error != expect_err || hcp.last != expect_last) {
                nxt_log_alert(thr->log,
                              "nxt_http_chunk_parse() ctl-line-start test "
                              "failed for 0x%02Xd: chunk_error=%d/%d last=%d/%d",
                              (int) c, (int) hcp.chunk_error, (int) expect_err,
                              (int) hcp.last, (int) expect_last);
                return NXT_ERROR;
            }
        }
    }

    /*
     * retain_buffers == 1 (request body path): non-empty chunk slices are
     * consumed synchronously by the caller without running completion
     * handlers, so nxt_http_chunk_buffer() must not retain hcp->mem_pool or
     * increment b.retain.
     */
    {
        nxt_mp_t   *mp;
        nxt_buf_t  *out;
        nxt_str_t  body = nxt_string("5\r\nhello\r\n3\r\nbye\r\n0\r\n\r\n");

        mp = nxt_mp_create(1024, 128, 256, 32);
        if (mp == NULL) {
            return NXT_ERROR;
        }

        nxt_memzero(&hcp, sizeof(nxt_http_chunk_parse_t));
        hcp.mem_pool = mp;
        hcp.retain_buffers = 1;

        nxt_memzero(&b, sizeof(nxt_buf_t));
        b.mem.pos = body.start;
        b.mem.free = body.start + body.length;

        out = nxt_http_chunk_parse(thr->task, &hcp, &b);

        if (out == NULL || out->next == NULL || out->next->next != NULL
            || out->parent != &b || out->next->parent != &b
            || nxt_buf_mem_used_size(&out->mem) != 5
            || memcmp(out->mem.pos, "hello", 5) != 0
            || nxt_buf_mem_used_size(&out->next->mem) != 3
            || memcmp(out->next->mem.pos, "bye", 3) != 0
            || hcp.chunk_error != 0 || hcp.error != 0 || hcp.last != 1
            || b.retain != 0 || nxt_mp_test_retain_count(mp) != 1)
        {
            nxt_log_alert(thr->log,
                          "nxt_http_chunk_parse() retain_buffers=1 test "
                          "failed: retain=%uD mp_retain=%uD",
                          b.retain, nxt_mp_test_retain_count(mp));
            nxt_mp_destroy(mp);
            return NXT_ERROR;
        }

        nxt_mp_release(mp);
    }

    /*
     * retain_buffers == 0 (upstream proxy path): an input buffer that produces
     * no data slices (e.g., a separate terminal "0\r\n\r\n" buffer or a
     * malformed chunk) must still have its completion_handler queued on
     * fast_work_queue so the upstream buffer's pool retention is released.
     */
    {
        void                *obj, *data;
        nxt_mp_t            *mp;
        nxt_buf_t           *out;
        nxt_task_t          *task;
        nxt_uint_t          completed;
        nxt_thread_t        *saved_thread;
        nxt_event_engine_t  engine, *saved_engine;
        nxt_work_handler_t  handler;
        nxt_str_t           last_chunk = nxt_string("0\r\n\r\n");
        nxt_str_t           bad_chunk = nxt_string("5\r\nhello\r\nz\r\n");

        nxt_memzero(&engine, sizeof(nxt_event_engine_t));
        nxt_work_queue_cache_create(&engine.work_queue_cache, 16);
        engine.fast_work_queue.cache = &engine.work_queue_cache;
        nxt_work_queue_name(&engine.fast_work_queue, "test");

        saved_thread = thr->task->thread;
        saved_engine = thr->engine;
        thr->task->thread = thr;
        thr->engine = &engine;

        completed = 0;

        nxt_memzero(&hcp, sizeof(nxt_http_chunk_parse_t));
        nxt_memzero(&b, sizeof(nxt_buf_t));
        b.mem.pos = last_chunk.start;
        b.mem.free = last_chunk.start + last_chunk.length;
        b.completion_handler = nxt_http_chunk_test_buf_completion;
        b.parent = &completed;

        (void) nxt_http_chunk_parse(thr->task, &hcp, &b);

        while (engine.fast_work_queue.head != NULL) {
            handler = nxt_work_queue_pop(&engine.fast_work_queue, &task, &obj,
                                         &data);
            handler(task, obj, data);
        }

        if (hcp.last != 1 || hcp.chunk_error != 0 || completed != 1) {
            thr->engine = saved_engine;
            thr->task->thread = saved_thread;
            nxt_work_queue_cache_destroy(&engine.work_queue_cache);
            nxt_log_alert(thr->log,
                          "nxt_http_chunk_parse() terminal completion test "
                          "failed: last=%d chunk_error=%d completed=%ui",
                          (int) hcp.last, (int) hcp.chunk_error, completed);
            return NXT_ERROR;
        }

        mp = nxt_mp_create(1024, 128, 256, 32);
        if (mp == NULL) {
            thr->engine = saved_engine;
            thr->task->thread = saved_thread;
            nxt_work_queue_cache_destroy(&engine.work_queue_cache);
            return NXT_ERROR;
        }

        completed = 0;

        nxt_memzero(&hcp, sizeof(nxt_http_chunk_parse_t));
        hcp.mem_pool = mp;

        nxt_memzero(&b, sizeof(nxt_buf_t));
        b.mem.pos = bad_chunk.start;
        b.mem.free = bad_chunk.start + bad_chunk.length;
        b.completion_handler = nxt_http_chunk_test_buf_completion;
        b.parent = &completed;

        out = nxt_http_chunk_parse(thr->task, &hcp, &b);

        if (out == NULL || hcp.chunk_error != 1
            || b.retain != 1 || nxt_mp_test_retain_count(mp) != 2)
        {
            thr->engine = saved_engine;
            thr->task->thread = saved_thread;
            nxt_work_queue_cache_destroy(&engine.work_queue_cache);
            nxt_log_alert(thr->log,
                          "nxt_http_chunk_parse() error slice test failed: "
                          "chunk_error=%d retain=%uD mp_retain=%uD",
                          (int) hcp.chunk_error, b.retain,
                          nxt_mp_test_retain_count(mp));
            nxt_mp_destroy(mp);
            return NXT_ERROR;
        }

        out->completion_handler(thr->task, out, out->parent);

        while (engine.fast_work_queue.head != NULL) {
            handler = nxt_work_queue_pop(&engine.fast_work_queue, &task, &obj,
                                         &data);
            handler(task, obj, data);
        }

        thr->engine = saved_engine;
        thr->task->thread = saved_thread;
        nxt_work_queue_cache_destroy(&engine.work_queue_cache);

        if (b.retain != 0 || completed != 1
            || nxt_mp_test_retain_count(mp) != 1)
        {
            nxt_log_alert(thr->log,
                          "nxt_http_chunk_parse() error completion test "
                          "failed: retain=%uD completed=%ui mp_retain=%uD",
                          b.retain, completed, nxt_mp_test_retain_count(mp));
            nxt_mp_destroy(mp);
            return NXT_ERROR;
        }

        nxt_mp_release(mp);
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "nxt_http_chunk_parse() test passed");

    return NXT_OK;
}
