/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_main.h>

/* DO NOT TRY THIS AT HOME! */
#include "nxt_http_parse.c"


/*
 * This target compares the field value scan of nxt_http_parse.c with the
 * code that it replaced: a byte loop that stopped at the first byte below
 * 0x20, and a memchr() for DEL (0x7F) over the value after the loop found
 * the end of the line.  The scan now tests 8 bytes at a time and stops at
 * DEL too.  Two checks run on each input.  A difference causes an abort.
 *
 * - nxt_http_lookup_field_end() on each range that starts at one of the
 *   first 9 bytes and ends at one of the last 9 bytes must return the
 *   pointer that the reference scan returns.  libFuzzer gives the input in
 *   a heap buffer of exactly "size" bytes, so AddressSanitizer reports a
 *   read past the end.
 *
 * - nxt_http_parse_field_value() on the whole input must give the result
 *   of a copy of the old function: the same return code, value, position
 *   and stored field.  One difference is allowed.  When the line does not
 *   end in the input, the old function asks for more data, and the new one
 *   rejects a value that has a DEL.  Then the old function must reject the
 *   value when the line ends, which is checked with a CR LF after a copy of
 *   the input.
 *
 * The whole input is the field value, from the byte after the colon.
 */


#define KMININPUTLENGTH  1
#define KMAXINPUTLENGTH  1024

#define NXT_FIELD_FUZZ_EDGE  8


extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static u_char *nxt_field_fuzz_old_scan(u_char *p, const u_char *end);
static u_char *nxt_field_fuzz_ref_scan(u_char *p, const u_char *end);
static nxt_int_t nxt_field_fuzz_old_value(nxt_http_request_parse_t *rp,
    u_char **pos, const u_char *end);
static void nxt_field_fuzz_init(nxt_http_request_parse_t *rp);
static void nxt_field_fuzz_compare(nxt_http_request_parse_t *rp, u_char *pos,
    nxt_http_request_parse_t *ref, u_char *ref_pos);


extern char  **environ;


int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
    if (nxt_lib_start("fuzzing", NULL, &environ) != NXT_OK) {
        return NXT_ERROR;
    }

    /* Keep a fuzzing run quiet: nothing below alert is worth printing. */
    nxt_main_log.level = NXT_LOG_ALERT;

    return 0;
}


int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    u_char                    *p, *end, *pos, *ref_pos, *copy;
    size_t                    i, j, edge;
    nxt_int_t                 rc, ref_rc;
    nxt_http_request_parse_t  rp, ref;

    if (size < KMININPUTLENGTH || size > KMAXINPUTLENGTH) {
        return 0;
    }

    p = (u_char *) data;
    end = p + size;

    edge = nxt_min(size, NXT_FIELD_FUZZ_EDGE);

    for (i = 0; i <= edge; i++) {
        for (j = 0; j <= edge && i + j <= size; j++) {
            if (nxt_http_lookup_field_end(p + i, end - j)
                != nxt_field_fuzz_ref_scan(p + i, end - j))
            {
                abort();
            }
        }
    }

    nxt_field_fuzz_init(&rp);
    nxt_field_fuzz_init(&ref);

    pos = p;
    ref_pos = p;

    rc = nxt_http_parse_field_value(&rp, &pos, end);
    ref_rc = nxt_field_fuzz_old_value(&ref, &ref_pos, end);

    if (rc == ref_rc) {
        nxt_field_fuzz_compare(&rp, pos, &ref, ref_pos);
        return 0;
    }

    /*
     * The old function returned NXT_AGAIN with "ref_pos" at the start of the
     * value, and the new one found a DEL in the bytes it was given.
     */

    if (rc != NXT_HTTP_PARSE_INVALID
        || ref_rc != NXT_AGAIN
        || memchr(ref_pos, 0x7F, end - ref_pos) == NULL)
    {
        abort();
    }

    copy = malloc(size + 2);
    if (copy == NULL) {
        return 0;
    }

    nxt_memcpy(copy, data, size);
    copy[size] = '\r';
    copy[size + 1] = '\n';

    nxt_field_fuzz_init(&ref);
    ref_pos = copy;

    ref_rc = nxt_field_fuzz_old_value(&ref, &ref_pos, copy + size + 2);

    free(copy);

    if (ref_rc != NXT_HTTP_PARSE_INVALID) {
        abort();
    }

    return 0;
}


/* The scan of origin/master a5352d223, without its unrolling. */

static u_char *
nxt_field_fuzz_old_scan(u_char *p, const u_char *end)
{
    while (p != end && *p >= 0x20) {
        p++;
    }

    return p;
}


/* The old scan and the DEL check: the first byte below 0x20 or DEL. */

static u_char *
nxt_field_fuzz_ref_scan(u_char *p, const u_char *end)
{
    u_char  *q, *del;

    q = nxt_field_fuzz_old_scan(p, end);

    del = memchr(p, 0x7F, q - p);

    return (del != NULL) ? del : q;
}


/* nxt_http_parse_field_value() of origin/master a5352d223. */

static nxt_int_t
nxt_field_fuzz_old_value(nxt_http_request_parse_t *rp, u_char **pos,
    const u_char *end)
{
    u_char  *p, *start, ch;
    size_t  len;

    p = *pos;

    for ( ;; ) {
        if (nxt_slow_path(p == end)) {
            *pos = p;
            rp->handler = &nxt_http_parse_field_value;
            return NXT_AGAIN;
        }

        ch = *p;

        if (ch != ' ' && ch != '\t') {
            break;
        }

        p++;
    }

    start = p;

    p += rp->field_value.length;

    for ( ;; ) {
        p = nxt_field_fuzz_old_scan(p, end);

        if (nxt_slow_path(p == end)) {
            *pos = start;

            len = p - start;

            if (nxt_slow_path(len > NXT_HTTP_MAX_FIELD_VALUE)) {
                return NXT_HTTP_PARSE_TOO_LARGE_FIELD;
            }

            rp->field_value.length = len;
            rp->handler = &nxt_http_parse_field_value;
            return NXT_AGAIN;
        }

        ch = *p;

        if (nxt_fast_path(ch == '\r' || ch == '\n')) {
            break;
        }

        if (ch != '\t') {
            return NXT_HTTP_PARSE_INVALID;
        }

        p++;
    }

    if (nxt_slow_path(memchr(start, 0x7F, p - start) != NULL)) {
        return NXT_HTTP_PARSE_INVALID;
    }

    *pos = p;

    if (nxt_fast_path(p != start)) {

        while (p[-1] == ' ' || p[-1] == '\t') {
            p--;
        }
    }

    len = p - start;

    if (nxt_slow_path(len > NXT_HTTP_MAX_FIELD_VALUE)) {
        return NXT_HTTP_PARSE_TOO_LARGE_FIELD;
    }

    rp->field_value.length = len;
    rp->field_value.start = start;

    return nxt_http_parse_field_end(rp, pos, end);
}


/* The state of the parser after "X:", so a complete line stores a field. */

static void
nxt_field_fuzz_init(nxt_http_request_parse_t *rp)
{
    static u_char  name[] = "X";

    nxt_memzero(rp, sizeof(nxt_http_request_parse_t));

    rp->field_hash = NXT_HTTP_FIELD_HASH_INIT;
    rp->field_name.start = name;
    rp->field_name.length = 1;
}


static void
nxt_field_fuzz_compare(nxt_http_request_parse_t *rp, u_char *pos,
    nxt_http_request_parse_t *ref, u_char *ref_pos)
{
    nxt_http_field_t  *f, *rf;

    if (pos != ref_pos
        || rp->handler != ref->handler
        || rp->field_value.start != ref->field_value.start
        || rp->field_value.length != ref->field_value.length
        || rp->num_inline_fields != ref->num_inline_fields)
    {
        abort();
    }

    if (rp->num_inline_fields != 0) {
        f = &rp->inline_fields[0];
        rf = &ref->inline_fields[0];

        if (f->value != rf->value
            || f->value_length != rf->value_length
            || f->hash != rf->hash)
        {
            abort();
        }
    }
}
