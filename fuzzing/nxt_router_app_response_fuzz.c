/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * libFuzzer target for nxt_router_response_header_parse() in
 * src/nxt_router.c: the router decoding the nxt_unit_response_t that an
 * untrusted application handed back.  The fuzz input is the response buffer,
 * byte for byte: the header, the field array and whatever the sptr offsets
 * point at.
 */

#include <nxt_main.h>

/* DO NOT TRY THIS AT HOME! */
#include "nxt_router.c"


#define KMININPUTLENGTH  sizeof(nxt_unit_response_t)
#define KMAXINPUTLENGTH  (64 * 1024)


extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

extern char  **environ;


/* A bad response is answered with nxt_alert(), which no log level silences. */
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

    /* Exactly the fuzzer's bytes, so ASan catches a sptr resolved outside. */
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
