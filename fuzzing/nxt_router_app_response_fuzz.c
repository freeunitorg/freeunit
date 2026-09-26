/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * libFuzzer target for the router's response-header parsing: the one place
 * the router decodes what an untrusted application handed back as an
 * nxt_unit_response_t -- src/nxt_unit_response.h, src/nxt_unit_field.h --
 * through nxt_router_response_header_parse() in src/nxt_router.c.  That
 * function is a straight extraction of the header-parsing block that used
 * to live inline in nxt_router_response_ready_handler(): the fields_count
 * bound check, the per-field nxt_unit_sptr_get() resolution and the
 * piggybacked body's sptr, exactly as the router runs them, without the
 * port message, the RPC bookkeeping or a live application connection.
 *
 * DO NOT TRY THIS AT HOME! -- #include the router source directly, the way
 * fuzzing/nxt_http_h1p_fuzz.c does for nxt_h1proto.c, to reach the static
 * nxt_router_response_header_parse() and nxt_response_fields_hash.
 *
 * Input layout: none.  The fuzz data *is* the response buffer, byte for
 * byte -- an nxt_unit_response_t header, then its nxt_unit_field_t array,
 * then whatever name/value/piggyback bytes the sptr offsets point at.
 * Every length, offset and count the parser trusts is data the fuzzer
 * controls directly; see the comment on nxt_router_response_header_parse()
 * for the exact list.
 */

#include <nxt_main.h>

/* DO NOT TRY THIS AT HOME! */
#include "nxt_router.c"


#define KMININPUTLENGTH  sizeof(nxt_unit_response_t)
#define KMAXINPUTLENGTH  (64 * 1024)


extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

extern char  **environ;


/*
 * Keeps a fuzzing run quiet.  The parser answers a bad response with an
 * alert, and nxt_alert() calls the handler whatever the log level is.
 */
static void nxt_cdecl
nxt_fuzz_log_handler(nxt_uint_t level, nxt_log_t *log, const char *fmt, ...)
{
}


int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
    if (nxt_lib_start("fuzzing", NULL, &environ) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_main_log.handler = nxt_fuzz_log_handler;

    if (nxt_http_response_hash_init(NULL) != NXT_OK) {
        return NXT_ERROR;
    }

    return 0;
}


int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    nxt_mp_t             *mp;
    u_char               *buf;
    nxt_buf_t            b;
    nxt_http_request_t   r;

    if (size < KMININPUTLENGTH || size > KMAXINPUTLENGTH) {
        return 0;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return 0;
    }

    /*
     * The response buffer itself: a plain heap allocation holding exactly
     * the fuzzer's bytes, so ASan's redzones are what catch a sptr that
     * resolves outside it.
     */
    buf = nxt_mp_alloc(mp, size);
    if (buf == NULL) {
        goto failed;
    }

    memcpy(buf, data, size);

    nxt_memzero(&b, sizeof(nxt_buf_t));
    b.mem.start = buf;
    b.mem.pos = buf;
    b.mem.free = buf + size;
    b.mem.end = buf + size;

    nxt_memzero(&r, sizeof(nxt_http_request_t));
    r.mem_pool = mp;
    r.task.log = &nxt_main_log;

    (void) nxt_router_response_header_parse(&r.task, &r, &b);

failed:

    nxt_mp_destroy(mp);

    return 0;
}
