/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>

/* DO NOT TRY THIS AT HOME! */
#include "nxt_h1proto.c"


#define KMININPUTLENGTH 2
#define KMAXINPUTLENGTH 1024


extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);


extern char  **environ;


int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
    nxt_int_t  ret;

    if (nxt_lib_start("fuzzing", NULL, &environ) != NXT_OK) {
        return NXT_ERROR;
    }

    /* Keep a fuzzing run quiet: nothing below alert is worth printing. */
    nxt_main_log.level = NXT_LOG_ALERT;

    ret = nxt_http_fields_hash(&nxt_h1p_fields_hash,
                               nxt_h1p_fields, nxt_nitems(nxt_h1p_fields));
    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    return 0;
}


int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    nxt_mp_t                  *mp;
    nxt_int_t                 rc;
    nxt_buf_mem_t             buf;
    nxt_http_request_t        *req;
    nxt_http_request_parse_t  rp;

    if (size < KMININPUTLENGTH || size > KMAXINPUTLENGTH) {
        return 0;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return 0;
    }

    req = nxt_mp_zget(mp, sizeof(nxt_http_request_t));
    if (req == NULL) {
        goto failed;
    }

    req->proto.h1 = nxt_mp_zget(mp, sizeof(nxt_h1proto_t));
    if (req->proto.h1 == NULL) {
        goto failed;
    }

    req->conf = nxt_mp_zget(mp, sizeof(nxt_socket_conf_joint_t));
    if (req->conf == NULL) {
        goto failed;
    }

    req->conf->socket_conf = nxt_mp_zget(mp, sizeof(nxt_socket_conf_t));
    if (req->conf->socket_conf == NULL) {
        goto failed;
    }

    /*
     * A field handler may log, and nxt_log() dereferences task->log.  The
     * request is zeroed memory here, so without this any handler that logs
     * is a null dereference in the harness rather than a finding.
     */
    req->task.log = &nxt_main_log;

    buf.start = (u_char *)data;
    buf.end = (u_char *)data + size;
    buf.pos = buf.start;
    buf.free = buf.end;

    req->mem_pool = mp;
    req->conf->socket_conf->max_body_size = 8 * 1024 * 1024;

    nxt_memzero(&rp, sizeof(nxt_http_request_parse_t));

    rc = nxt_http_parse_request_init(&rp, mp);
    if (rc != NXT_OK) {
        goto failed;
    }

    rc = nxt_http_parse_request(&rp, &buf);
    if (rc != NXT_DONE) {
        goto failed;
    }

    nxt_http_fields_process(rp.inline_fields, rp.num_inline_fields, rp.fields,
                            &nxt_h1p_fields_hash, req);

failed:

    nxt_mp_destroy(mp);

    return 0;
}
