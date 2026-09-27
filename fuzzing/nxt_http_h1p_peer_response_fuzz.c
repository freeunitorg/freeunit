/*
 * Copyright (C) FreeUnit contributors.
 */

#include <nxt_main.h>

/* DO NOT TRY THIS AT HOME! */
#include "nxt_h1proto.c"


/*
 * This target fuzzes how the proxy reads an upstream response header: the
 * status line and the header fields that nxt_h1p_peer_header_parse() takes
 * from the upstream socket, and the peer field handlers that run on them.
 * fuzz_http_h1p_peer gets to those handlers through the request parser and
 * in one call.  This target gets to them in the same way as the proxy.
 *
 * The proxy reads the header into one buffer.  After each read that
 * returned NXT_AGAIN, it parses again, and the parser continues from its
 * saved state.  Thus the target parses each input two times: one time
 * complete, and one time a few bytes at a time.  The two runs must give the
 * same result, the same status, the same end of the header and the same
 * fields.  A difference causes an abort.
 *
 * Input layout: the first byte seeds the read sizes, the other bytes are
 * the response.
 */


#define KMININPUTLENGTH 2
#define KMAXINPUTLENGTH 4096


typedef struct {
    nxt_int_t         ret;
    nxt_int_t         status;
    size_t            header_size;
    nxt_uint_t        nfields;
    nxt_http_field_t  fields[64];
} nxt_peer_fuzz_result_t;


extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static nxt_int_t nxt_peer_fuzz_run(nxt_mp_t *mp, u_char *start,
    const u_char *src, size_t size, uint32_t seed, nxt_peer_fuzz_result_t *res);
static nxt_int_t nxt_peer_fuzz_result(nxt_int_t ret);
static nxt_bool_t nxt_peer_fuzz_field_eq(nxt_http_field_t *f1, u_char *base1,
    nxt_http_field_t *f2, u_char *base2);
static uint32_t nxt_peer_fuzz_rand(uint32_t *state);


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

    ret = nxt_http_fields_hash(&nxt_h1p_peer_fields_hash,
                                nxt_h1p_peer_fields,
                                nxt_nitems(nxt_h1p_peer_fields));
    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    return 0;
}


int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    u_char                  *whole_buf, *split_buf;
    nxt_mp_t                *mp;
    nxt_uint_t              i;
    nxt_peer_fuzz_result_t  *whole, *split;

    if (size < KMININPUTLENGTH || size > KMAXINPUTLENGTH) {
        return 0;
    }

    size--;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return 0;
    }

    /*
     * Each run gets its own heap buffer of the exact size.  Thus ASan catches
     * a read after the received bytes at the end of the input at least, and
     * the addresses of the fields show which run they came from.
     */
    whole_buf = nxt_malloc(size);
    split_buf = nxt_malloc(size);
    whole = nxt_mp_zget(mp, sizeof(nxt_peer_fuzz_result_t));
    split = nxt_mp_zget(mp, sizeof(nxt_peer_fuzz_result_t));

    if (whole_buf == NULL || split_buf == NULL
        || whole == NULL || split == NULL)
    {
        goto done;
    }

    /* A seed of 0 gives the full response in one pass: the reference run. */

    if (nxt_peer_fuzz_run(mp, whole_buf, data + 1, size, 0, whole) != NXT_OK
        || nxt_peer_fuzz_run(mp, split_buf, data + 1, size,
                             (uint32_t) data[0] + 1, split)
           != NXT_OK)
    {
        goto done;
    }

    if (whole->ret != split->ret) {
        goto differs;
    }

    if (whole->ret != NXT_DONE) {
        goto done;
    }

    if (whole->status != split->status
        || whole->header_size != split->header_size
        || whole->nfields != split->nfields)
    {
        goto differs;
    }

    for (i = 0; i < whole->nfields; i++) {
        if (!nxt_peer_fuzz_field_eq(&whole->fields[i], whole_buf,
                                    &split->fields[i], split_buf))
        {
            goto differs;
        }
    }

done:

    nxt_free(whole_buf);
    nxt_free(split_buf);

    nxt_mp_destroy(mp);

    return 0;

differs:

    nxt_thread_log_alert("peer response fuzz: split run differs: "
                         "ret %i/%i status %i/%i header %uz/%uz "
                         "fields %ui/%ui",
                         whole->ret, split->ret,
                         whole->status, split->status,
                         whole->header_size, split->header_size,
                         whole->nfields, split->nfields);
    abort();
}


static nxt_int_t
nxt_peer_fuzz_run(nxt_mp_t *mp, u_char *start, const u_char *src, size_t size,
    uint32_t seed, nxt_peer_fuzz_result_t *res)
{
    size_t              len, cut, done;
    nxt_int_t           ret;
    nxt_buf_mem_t       bm;
    nxt_h1proto_t       *h1p;
    nxt_http_field_t    *f;
    nxt_http_peer_t     *peer;
    nxt_http_request_t  *r;

    r = nxt_mp_zget(mp, sizeof(nxt_http_request_t));
    peer = nxt_mp_zget(mp, sizeof(nxt_http_peer_t));
    h1p = nxt_mp_zget(mp, sizeof(nxt_h1proto_t));

    if (r == NULL || peer == NULL || h1p == NULL) {
        return NXT_ERROR;
    }

    /*
     * A field handler can log, and nxt_log() reads task->log.  The request
     * is zeroed memory here.  Without this line, a handler that logs causes
     * a null dereference in the harness, which is not a finding.
     */
    r->task.log = &nxt_main_log;
    r->mem_pool = mp;
    r->peer = peer;
    r->resp.content_length_n = -1;

    peer->request = r;
    peer->proto.h1 = h1p;

    /* The same values that nxt_h1p_peer_header_send() leaves. */
    peer->status = NXT_HTTP_UNSET;

    ret = nxt_http_parse_request_init(&h1p->parser, mp);
    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    bm.start = start;
    bm.pos = start;
    bm.free = start;
    bm.end = start + size;

    /*
     * Each pass copies the next bytes of the input to bm.free, in the same
     * way as a read from the upstream.  After a 1xx, the parser can move the
     * unread bytes to bm.start and move bm.free back.  Thus bm.free is not a
     * cursor into the input.  "done" is the cursor.
     */
    done = 0;

    for ( ;; ) {
        len = size - done;

        if (seed != 0) {
            cut = nxt_peer_fuzz_rand(&seed) % 32 + 1;
            len = nxt_min(len, cut);
        }

        nxt_memcpy(bm.free, src + done, len);
        bm.free += len;
        done += len;

        ret = nxt_h1p_peer_header_parse(peer, &bm);

        if (ret != NXT_AGAIN || done == size) {
            break;
        }
    }

    if (bm.pos < start || bm.pos > bm.free) {
        nxt_thread_log_alert("peer response fuzz: pos out of the buffer");
        abort();
    }

    res->ret = nxt_peer_fuzz_result(ret);

    if (ret != NXT_DONE) {
        return NXT_OK;
    }

    res->status = peer->status;
    res->header_size = bm.pos - start;

    /* The same steps that nxt_h1p_peer_header_read_done() does. */

    peer->num_inline_fields = h1p->parser.num_inline_fields;
    if (peer->num_inline_fields > 0) {
        nxt_memcpy(peer->inline_fields, h1p->parser.inline_fields,
                   sizeof(nxt_http_field_t) * peer->num_inline_fields);
    }
    peer->fields = h1p->parser.fields;

    nxt_http_fields_each(f, peer->inline_fields, peer->num_inline_fields,
                         peer->fields)
    {
        if (f->name < start || f->name + f->name_length > bm.pos
            || f->value < start || f->value + f->value_length > bm.pos)
        {
            nxt_thread_log_alert("peer response fuzz: field out of header");
            abort();
        }

        if (res->nfields < nxt_nitems(res->fields)) {
            res->fields[res->nfields++] = *f;
        }

    } nxt_http_fields_loop;

    ret = nxt_http_fields_process(peer->inline_fields, peer->num_inline_fields,
                                  peer->fields, &nxt_h1p_peer_fields_hash, r);
    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    /*
     * The framing decision of nxt_h1p_peer_header_read_done(), in the same
     * order: a response without a body ends at the empty line, whatever the
     * fields say.  After that, a Transfer-Encoding the proxy cannot decode,
     * and chunked together with Content-Length, each end in 502.
     */

    h1p->chunked = (h1p->transfer_encoding == NXT_HTTP_TE_CHUNKED);

    if (nxt_http_request_is_bodyless_final(r, peer->status)) {
        h1p->chunked = 0;

    } else if (h1p->transfer_encoding == NXT_HTTP_TE_UNSUPPORTED
               || (h1p->chunked && r->resp.content_length != NULL))
    {
        res->status = NXT_HTTP_BAD_GATEWAY;
    }

    return NXT_OK;
}


/*
 * The error that a bad header ends in can depend on where it was cut.  A
 * field name that is longer than its limit is "too large" when the parser
 * stops in it, but "invalid" when the bad byte after it is already there.
 * Only the outcome must be the same.
 */

static nxt_int_t
nxt_peer_fuzz_result(nxt_int_t ret)
{
    switch (ret) {
    case NXT_DONE:
    case NXT_AGAIN:
        return ret;
    default:
        return NXT_ERROR;
    }
}


static nxt_bool_t
nxt_peer_fuzz_field_eq(nxt_http_field_t *f1, u_char *base1,
    nxt_http_field_t *f2, u_char *base2)
{
    return f1->hash == f2->hash
           && f1->name_length == f2->name_length
           && f1->value_length == f2->value_length
           && f1->name - base1 == f2->name - base2
           && f1->value - base1 == f2->value - base2;
}


/* xorshift32: fast, and the same sequence for the same input. */

static uint32_t
nxt_peer_fuzz_rand(uint32_t *state)
{
    uint32_t  x;

    x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    *state = x;

    return x;
}
