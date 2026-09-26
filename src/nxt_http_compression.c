/*
 * Copyright (C) Andrew Clayton
 * Copyright (C) F5, Inc.
 */

#include <nxt_auto_config.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_tstr.h>
#include <nxt_conf.h>
#include <nxt_http_compression.h>


#define NXT_COMP_LEVEL_UNSET               INT8_MIN

/* Headroom for the flush marker a compressor emits per call. */
#define NXT_HTTP_COMP_FLUSH_SLACK          64


typedef enum nxt_http_comp_scheme_e        nxt_http_comp_scheme_t;
typedef struct nxt_http_comp_type_s        nxt_http_comp_type_t;
typedef struct nxt_http_comp_opts_s        nxt_http_comp_opts_t;
typedef struct nxt_http_comp_compressor_s  nxt_http_comp_compressor_t;
typedef struct nxt_http_comp_ctx_s         nxt_http_comp_ctx_t;

enum nxt_http_comp_scheme_e {
    NXT_HTTP_COMP_SCHEME_IDENTITY = 0,
#if NXT_HAVE_ZLIB
    NXT_HTTP_COMP_SCHEME_DEFLATE,
    NXT_HTTP_COMP_SCHEME_GZIP,
#endif
#if NXT_HAVE_ZSTD
    NXT_HTTP_COMP_SCHEME_ZSTD,
#endif
#if NXT_HAVE_BROTLI
    NXT_HTTP_COMP_SCHEME_BROTLI,
#endif

    /* keep last */
    NXT_HTTP_COMP_SCHEME_UNKNOWN
};
#define NXT_NR_COMPRESSORS  NXT_HTTP_COMP_SCHEME_UNKNOWN

struct nxt_http_comp_type_s {
    nxt_str_t                         token;
    nxt_http_comp_scheme_t            scheme;
    int8_t                            def_compr;
    int8_t                            comp_min;
    int8_t                            comp_max;

    const nxt_http_comp_operations_t  *cops;
};

struct nxt_http_comp_opts_s {
    int8_t                      level;
    nxt_off_t                   min_len;
};

struct nxt_http_comp_compressor_s {
    const nxt_http_comp_type_t  *type;
    nxt_http_comp_opts_t        opts;
};

struct nxt_http_comp_ctx_s {
    nxt_uint_t                      idx;

    /*
     * The compressor's type table entry, copied out of the configuration
     * when the choice is applied.  The table is static and lives as long as
     * the process, so a body that is still being compressed when the
     * configuration is replaced keeps working from here instead of indexing
     * a per-configuration array that may already be freed.
     */
    const nxt_http_comp_type_t      *type;

    /*
     * The compressor nxt_http_comp_check_acceptable() chose, or -1 when the
     * request will not be compressed.  Kept apart from "idx", which is only
     * set once the choice has actually been applied.
     */
    nxt_int_t                       sel_idx;

    /*
     * The client sent "identity;q=0": it will not take the file's own bytes.
     * Recorded separately from sel_idx because a request that refuses
     * identity and accepts gzip selects gzip and is perfectly serveable --
     * until a Range enters, which is served as identity.
     */
    bool                            identity_refused;

    nxt_off_t                       resp_clen;
    nxt_off_t                       clen_sent;

    nxt_http_comp_compressor_ctx_t  ctx;
};


/*
 * Everything here is allocated from the router configuration's pools, so it
 * is held per configuration and reached through the request.  It used to be
 * four process-global pointers, which dangled as soon as the configuration
 * they came from was freed -- reconfiguring compression away killed the
 * router on the next request (#167).
 */
struct nxt_http_comp_conf_s {
    nxt_http_route_rule_t       *mime_types_rule;
    nxt_http_comp_compressor_t  *enabled;
    nxt_uint_t                  nr_enabled;
};

static nxt_thread_declare_data(nxt_http_comp_ctx_t,
                               nxt_http_comp_compressor_ctx);

#define nxt_http_comp_ctx()  nxt_thread_get_data(nxt_http_comp_compressor_ctx)

static const nxt_conf_map_t  nxt_http_comp_compressors_opts_map[] = {
    {
        nxt_string("level"),
        NXT_CONF_MAP_INT,
        offsetof(nxt_http_comp_opts_t, level),
    }, {
        nxt_string("min_length"),
        NXT_CONF_MAP_SIZE,
        offsetof(nxt_http_comp_opts_t, min_len),
    },
};

static const nxt_http_comp_type_t  nxt_http_comp_compressors[] = {
    /* Keep this first */
    {
        .token      = nxt_string("identity"),
        .scheme     = NXT_HTTP_COMP_SCHEME_IDENTITY,
#if NXT_HAVE_ZLIB
    }, {
        .token      = nxt_string("deflate"),
        .scheme     = NXT_HTTP_COMP_SCHEME_DEFLATE,
        .def_compr  = NXT_HTTP_COMP_ZLIB_DEFAULT_LEVEL,
        .comp_min   = NXT_HTTP_COMP_ZLIB_COMP_MIN,
        .comp_max   = NXT_HTTP_COMP_ZLIB_COMP_MAX,
        .cops       = &nxt_http_comp_deflate_ops,
    }, {
        .token      = nxt_string("gzip"),
        .scheme     = NXT_HTTP_COMP_SCHEME_GZIP,
        .def_compr  = NXT_HTTP_COMP_ZLIB_DEFAULT_LEVEL,
        .comp_min   = NXT_HTTP_COMP_ZLIB_COMP_MIN,
        .comp_max   = NXT_HTTP_COMP_ZLIB_COMP_MAX,
        .cops       = &nxt_http_comp_gzip_ops,
#endif
#if NXT_HAVE_ZSTD
    }, {
        .token      = nxt_string("zstd"),
        .scheme     = NXT_HTTP_COMP_SCHEME_ZSTD,
        .def_compr  = NXT_HTTP_COMP_ZSTD_DEFAULT_LEVEL,
        .comp_min   = NXT_HTTP_COMP_ZSTD_COMP_MIN,
        .comp_max   = NXT_HTTP_COMP_ZSTD_COMP_MAX,
        .cops       = &nxt_http_comp_zstd_ops,
#endif
#if NXT_HAVE_BROTLI
    }, {
        .token      = nxt_string("br"),
        .scheme     = NXT_HTTP_COMP_SCHEME_BROTLI,
        .def_compr  = NXT_HTTP_COMP_BROTLI_DEFAULT_LEVEL,
        .comp_min   = NXT_HTTP_COMP_BROTLI_COMP_MIN,
        .comp_max   = NXT_HTTP_COMP_BROTLI_COMP_MAX,
        .cops       = &nxt_http_comp_brotli_ops,
#endif
    },
};


nxt_inline nxt_http_comp_conf_t *
nxt_http_comp_request_conf(const nxt_http_request_t *r)
{
    return r->conf->socket_conf->router_conf->compression;
}


static ssize_t
nxt_http_comp_compress(uint8_t *dst, size_t dst_size, const uint8_t *src,
                       size_t src_size, bool last)
{
    nxt_http_comp_ctx_t               *ctx = nxt_http_comp_ctx();
    const nxt_http_comp_operations_t  *cops;

    cops = ctx->type->cops;

    return cops->deflate(&ctx->ctx, src, src_size, dst, dst_size, last);
}


static size_t
nxt_http_comp_bound(size_t size)
{
    nxt_http_comp_ctx_t               *ctx = nxt_http_comp_ctx();
    const nxt_http_comp_operations_t  *cops;

    cops = ctx->type->cops;

    return cops->bound(&ctx->ctx, size);
}


nxt_int_t
nxt_http_comp_compress_app_response(nxt_task_t *task, nxt_http_request_t *r,
                                    nxt_buf_t **b)
{
    bool                 last;
    size_t               buf_len;
    ssize_t              cbytes;
    nxt_buf_t            *in, *next, *buf, *out, **tail;
    nxt_off_t            in_len;
    nxt_http_comp_ctx_t  *ctx = nxt_http_comp_ctx();

    if (ctx->idx == NXT_HTTP_COMP_SCHEME_IDENTITY) {
        return NXT_OK;
    }

    /*
     * What arrives here is a chain, not a single buffer:
     * nxt_port_mmap_read() (src/nxt_port_memory.c) makes one nxt_buf_t per
     * nxt_port_mmap_msg_t, the call site links it into r->out with
     * nxt_buf_chain_add(), and a sync buffer may sit at the tail.  Walk it.
     *
     * The previous version compressed the head and then replaced the whole
     * chain with that one output buffer, so anything behind the head was
     * dropped from the response and never released, and ctx->clen_sent
     * counted only the head -- which is what decides whether the compressor
     * is told it is finishing the stream.
     */

    out = NULL;
    tail = &out;

    for (in = *b; in != NULL; in = next) {
        next = in->next;
        in->next = NULL;

        /*
         * Buffers that did not come through shared memory are passed
         * through untouched: the trailing sync/last buffer carries no data,
         * and a plain-mode response is not compressed at all (the same
         * condition the single-buffer version tested on the head).
         */
        if (!nxt_buf_is_port_mmap(in)) {
            *tail = in;
            tail = &in->next;
            continue;
        }

        in_len = in->mem.free - in->mem.pos;

        last = ctx->clen_sent + in_len == ctx->resp_clen;

        if (in_len == 0 && !last) {
            goto release;
        }

        /*
         * The per-call flush marker each compressor emits after the input
         * is not part of what bound() promises for the input alone, so the
         * output buffer gets a small fixed margin on top.
         */
        buf_len = nxt_http_comp_bound(in_len) + NXT_HTTP_COMP_FLUSH_SLACK;

        buf = nxt_buf_mem_ts_alloc(task, in->data, buf_len);
        if (nxt_slow_path(buf == NULL)) {
            goto fail;
        }

        cbytes = nxt_http_comp_compress(buf->mem.start, buf_len,
                                        in->mem.pos, in_len, last);
        if (nxt_slow_path(cbytes == -1)) {
            nxt_buf_free(buf->data, buf);
            goto fail;
        }

        buf->mem.free += cbytes;

        ctx->clen_sent += in_len;

        *tail = buf;
        tail = &buf->next;

    release:

        /*
         * The compressed bytes have been copied out, so the shared memory
         * chunk can go back to the application.  This has to run the
         * buffer's own completion handler -- nxt_buf_free() is a plain pool
         * free, and using it here (as the single-buffer version did) leaked
         * the chunk, the mmap_handler reference and the port mem_pool
         * reference on every compressed response.  in->next was cleared
         * above because nxt_port_mmap_buf_completion() walks the chain.
         */
        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           in->completion_handler, task, in, in->parent);
    }

    *b = out;

    return NXT_OK;

fail:

    /*
     * Keep whatever has been produced so far, and re-attach the input that
     * has not been consumed, so that the caller's error path owns the whole
     * chain and releases it.
     */
    in->next = next;
    *tail = in;
    *b = out;

    return NXT_ERROR;
}


nxt_int_t
nxt_http_comp_compress_static_response(nxt_task_t *task, nxt_http_request_t *r,
                                       nxt_file_t **f, nxt_file_info_t *fi,
                                       size_t static_buf_len, size_t *out_total)
{
    size_t         in_size, out_size, rest;
    char           *tmp_path, *p;
    uint8_t        *in, *out;
    nxt_int_t      ret;
    nxt_file_t     tfile;
    nxt_runtime_t  *rt = task->thread->runtime;

    static const char  *template = "unit-compr-XXXXXX";

    *out_total = 0;

    tmp_path = nxt_mp_nget(r->mem_pool,
                           strlen(rt->tmp) + 1 + strlen(template) + 1);
    if (nxt_slow_path(tmp_path == NULL)) {
        return NXT_ERROR;
    }

    p = tmp_path;
    p = nxt_cpymem(p, rt->tmp, strlen(rt->tmp));
    *p++ = '/';
    p = nxt_cpymem(p, template, strlen(template));
    *p = '\0';

    tfile.fd = mkstemp(tmp_path);
    if (nxt_slow_path(tfile.fd == -1)) {
        nxt_alert(task, "mkstemp(%s) failed %E", tmp_path, nxt_errno);
        return NXT_ERROR;
    }
    unlink(tmp_path);
    tfile.name = (nxt_file_name_t *)tmp_path;

    in_size = nxt_file_size(fi);
    out_size = nxt_http_comp_bound(in_size);

    ret = ftruncate(tfile.fd, out_size);
    if (nxt_slow_path(ret == -1)) {
        nxt_alert(task, "ftruncate(%d<%s>, %uz) failed %E",
                  tfile.fd, tmp_path, out_size, nxt_errno);
        nxt_file_close(task, &tfile);
        nxt_file_close(task, *f);
        *f = NULL;
        return NXT_ERROR;
    }

    in = nxt_mem_mmap(NULL, in_size, PROT_READ, MAP_SHARED, (*f)->fd, 0);
    if (nxt_slow_path(in == MAP_FAILED)) {
        nxt_alert(task, "mmap(%uz) of source failed %E", in_size, nxt_errno);
        nxt_file_close(task, &tfile);
        nxt_file_close(task, *f);
        *f = NULL;
        return NXT_ERROR;
    }

    out = nxt_mem_mmap(NULL, out_size, PROT_READ|PROT_WRITE, MAP_SHARED,
                       tfile.fd, 0);
    if (nxt_slow_path(out == MAP_FAILED)) {
        nxt_alert(task, "mmap(%uz) of temp failed %E", out_size, nxt_errno);
        nxt_mem_munmap(in, in_size);
        nxt_file_close(task, &tfile);
        nxt_file_close(task, *f);
        *f = NULL;
        return NXT_ERROR;
    }

    rest = in_size;

    do {
        bool     last;
        size_t   n;
        ssize_t  cbytes;

        n = nxt_min(rest, static_buf_len);

        last = n == rest;

        cbytes = nxt_http_comp_compress(out + *out_total, out_size - *out_total,
                                        in + in_size - rest, n, last);
        if (cbytes == -1) {
            nxt_file_close(task, &tfile);
            nxt_mem_munmap(in, in_size);
            nxt_mem_munmap(out, out_size);
            nxt_file_close(task, *f);
            *f = NULL;
            return NXT_ERROR;
        }

        *out_total += cbytes;
        rest -= n;
    } while (rest > 0);

    nxt_mem_munmap(in, in_size);
    msync(out, out_size, MS_ASYNC);
    nxt_mem_munmap(out, out_size);

    ret = ftruncate(tfile.fd, *out_total);
    if (nxt_slow_path(ret == -1)) {
        nxt_alert(task, "ftruncate(%d<%s>, %uz) failed %E",
                  tfile.fd, tmp_path, *out_total, nxt_errno);
        nxt_file_close(task, &tfile);
        nxt_file_close(task, *f);
        *f = NULL;
        return NXT_ERROR;
    }

    nxt_file_close(task, *f);

    **f = tfile;

    return NXT_OK;
}


bool
nxt_http_comp_wants_compression(void)
{
    nxt_http_comp_ctx_t  *ctx = nxt_http_comp_ctx();

    return ctx->idx;
}


bool
nxt_http_comp_identity_refused(void)
{
    nxt_http_comp_ctx_t  *ctx = nxt_http_comp_ctx();

    return ctx->identity_refused;
}


/*
 * Finds the ";q=" weight parameter in one Accept-Encoding element.
 *
 * RFC 9110 Sect. 12.4.2 spells the parameter "weight = OWS ';' OWS ('q' /
 * 'Q') '=' qvalue", and an ABNF literal is case-insensitive besides, so
 * "identity;Q=0" is as valid as "identity;q=0".  A plain strstr() for ";q="
 * misses it, and for identity that means missing a refusal.  Spaces are
 * already gone by the time this runs.
 */

static char *
nxt_http_comp_find_weight(char *tkn)
{
    for (char *p = tkn; (p = strchr(p, ';')) != NULL; p++) {
        if ((p[1] == 'q' || p[1] == 'Q') && p[2] == '=') {
            return p;
        }
    }

    return NULL;
}


/*
 * Reads the qvalue that follows a weight's "=".
 *
 * RFC 9110 Sect. 12.4.2 spells it "( '0' [ '.' 0*3DIGIT ] ) / ( '1' [ '.'
 * 0*3( '0' ) ] )": one leading digit, then at most a fraction.  strtod() on
 * its own is far looser and every way it is looser is a bug here.  It takes
 * no digits at all from "q=" and from "q=abc" and reports 0, which this
 * function's caller reads as a refusal; it reads "q=0x10" as hexadecimal;
 * and it turns "q=nan" into a NaN that compares false against both range
 * bounds, so the element is kept and then outranks every real weight.
 *
 * So check the shape first and only then convert.  The digit count in the
 * fraction is not enforced: rejecting "q=0.0000" would read a client's
 * refusal as an acceptance, which is the wrong way to be strict.
 *
 * An element whose weight does not parse is ignored, exactly like an element
 * naming a coding this build does not have.  Reading it as q=0 is the other
 * defensible answer -- nginx reads a bad quantity as zero -- but a zero is a
 * refusal here, so "identity;q=" would answer 406 to a client that refused
 * nothing.
 */

static bool
nxt_http_comp_parse_weight(const char *qvalue, double *qval)
{
    const char  *p = qvalue;

    if (*p != '0' && *p != '1') {
        return false;
    }

    p++;

    if (*p == '.') {
        p++;

        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }

    /* Anything may follow the weight, but only as a further parameter. */

    if (*p != '\0' && *p != ';') {
        return false;
    }

    *qval = strtod(qvalue, NULL);

    return *qval <= 1.0;
}


static nxt_uint_t
nxt_http_comp_compressor_lookup_enabled(const nxt_http_comp_conf_t *conf,
                                        const nxt_str_t *token)
{
    /*
     * RFC 9110 Sect. 8.4.1: a content coding is a token, and tokens are
     * compared case-insensitively.  "Identity;q=0" and "GZIP" are as valid
     * as the lowercase spellings, and a case-sensitive compare silently
     * ignores them -- which, for identity, means missing a refusal.
     */

    for (nxt_uint_t i = 0; i < conf->nr_enabled; i++) {
        if (nxt_strcasestr_eq(token, &conf->enabled[i].type->token)) {
            return i;
        }
    }

    return NXT_HTTP_COMP_SCHEME_UNKNOWN;
}


/*
 * We need to parse the 'Accept-Encoding` header as described by
 * <https://www.rfc-editor.org/rfc/rfc9110.html#field.accept-encoding>
 * which can take forms such as
 *
 *  Accept-Encoding: compress, gzip
 *  Accept-Encoding:
 *  Accept-Encoding: *
 *  Accept-Encoding: compress;q=0.5, gzip;q=1.0
 *  Accept-Encoding: gzip;q=1.0, identity;q=0.5, *;q=0
 *
 *  '*:q=0' means if the content being served has no 'Content-Coding'
 *  matching an 'Accept-Encoding' entry then don't send any response.
 *
 * 'identity;q=0' seems to basically mean the same thing...
 */
/*
 * Collects every Accept-Encoding field into one value.
 *
 * RFC 9110 Sect. 5.3: a field that may carry a comma-separated list can be
 * sent as several lines, and a recipient must treat them as one value joined
 * by commas.  A variable query answers with the first matching field only
 * (nxt_http_var_header()), so "Accept-Encoding: gzip" followed by
 * "Accept-Encoding: identity;q=0" lost the refusal and the request was served
 * the identity bytes it had declined -- and in the other order the gzip it
 * would have accepted was never seen, so it drew a 406.
 *
 * The single-field case, which is every ordinary request, points straight at
 * the field and copies nothing.
 */

static nxt_int_t
nxt_http_comp_accept_encoding(nxt_http_request_t *r, nxt_str_t *value)
{
    u_char                  *p;
    size_t                  len;
    nxt_uint_t              n;
    nxt_http_field_t        *f, *first;
    nxt_http_fields_iter_t  iter;

    static const nxt_str_t  accept_encoding = nxt_string("Accept-Encoding");

    n = 0;
    len = 0;
    first = NULL;

    for (f = nxt_http_fields_first(&iter, r->inline_fields,
                                   r->num_inline_fields, r->fields);
         f != NULL;
         f = nxt_http_fields_next(&iter))
    {
        if (f->skip || f->name_length != accept_encoding.length
            || nxt_strncasecmp(f->name, accept_encoding.start,
                               accept_encoding.length) != 0)
        {
            continue;
        }

        if (n == 0) {
            first = f;

        } else {
            len += nxt_length(", ");
        }

        len += f->value_length;
        n++;
    }

    if (n == 0) {
        nxt_str_null(value);
        return NXT_OK;
    }

    if (n == 1) {
        value->start = first->value;
        value->length = first->value_length;

        return NXT_OK;
    }

    p = nxt_mp_nget(r->mem_pool, len);
    if (nxt_slow_path(p == NULL)) {
        return NXT_ERROR;
    }

    value->start = p;
    value->length = len;

    n = 0;

    for (f = nxt_http_fields_first(&iter, r->inline_fields,
                                   r->num_inline_fields, r->fields);
         f != NULL;
         f = nxt_http_fields_next(&iter))
    {
        if (f->skip || f->name_length != accept_encoding.length
            || nxt_strncasecmp(f->name, accept_encoding.start,
                               accept_encoding.length) != 0)
        {
            continue;
        }

        if (n++ != 0) {
            p = nxt_cpymem(p, ", ", nxt_length(", "));
        }

        p = nxt_cpymem(p, f->value, f->value_length);
    }

    return NXT_OK;
}


/*
 * The length the compressor decision is made against, or -1 when the body's
 * length is not known yet.
 */

static nxt_off_t
nxt_http_comp_resp_length(const nxt_http_request_t *r)
{
    if (r->resp.content_length_n > -1) {
        return r->resp.content_length_n;
    }

    if (r->resp.content_length != NULL) {
        return strtol((char *) r->resp.content_length->value, NULL, 10);
    }

    return -1;
}


/*
 * Whether "min_length" declines this response.  A body of unknown length is
 * compressed, so it is never below the minimum.
 *
 * Both nxt_http_comp_select_compressor() and
 * nxt_http_comp_apply_compression() ask this, and they have to give the same
 * answer: the first decides which coding is chosen, the second whether the
 * chosen one is really applied.
 */

static bool
nxt_http_comp_below_min_len(nxt_off_t clen,
                            const nxt_http_comp_compressor_t *compressor)
{
    return clen > -1 && clen < compressor->opts.min_len;
}


static nxt_int_t
nxt_http_comp_select_compressor(const nxt_http_comp_conf_t *conf,
                                nxt_http_request_t *r, const nxt_str_t *token,
                                bool *identity_refused)
{
    /*
     * What the field said about identity: "identity_named" and
     * "identity_named_ok" carry an explicit "identity" token, the wildcard
     * pair below carries "*".  They are kept apart because the explicit one
     * wins, and "named" records the enabled codings the field listed, which
     * are the ones the wildcard does not stand for.
     */
    bool       identity_allowed = true;
    bool       identity_named = false;
    bool       identity_named_ok = false;
    bool       wildcard_seen = false;
    char       *str, *tkn, *tail, *cur;
    double     weight = 0.0;
    double     wildcard_qval = 0.0;
    uint32_t   named = 0;
    nxt_off_t  clen = nxt_http_comp_resp_length(r);
    nxt_int_t  idx = NXT_HTTP_COMP_SCHEME_IDENTITY;

    *identity_refused = false;

    str = nxt_str_cstrz(r->mem_pool, token);
    if (str == NULL) {
        return NXT_HTTP_COMP_SCHEME_IDENTITY;
    }

    cur = tail = str;
    /*
     * To ease parsing the Accept-Encoding header, remove all optional
     * whitespace, which holds no semantic meaning.
     *
     * OWS is SP or HTAB (RFC 9110 Sect. 5.6.3), and it is legal on either
     * side of the weight's semicolon.  Removing only the space left
     * "identity;<HTAB>q=0" unparsed, so the element read as an unknown
     * coding and the refusal it carried was lost -- which handed a client
     * that refused identity a 206 of exactly those bytes.
     */
    for (; *cur != '\0'; cur++) {
        if (*cur == ' ' || *cur == '\t') {
            continue;
        }

        *tail++ = *cur;
    }
    *tail = '\0';

    while ((tkn = strsep(&str, ","))) {
        bool        wildcard;
        char        *qptr;
        double      qval = 1.0;
        nxt_str_t   enc;
        nxt_uint_t  ecidx;

        qptr = nxt_http_comp_find_weight(tkn);
        if (qptr != NULL && !nxt_http_comp_parse_weight(qptr + 3, &qval)) {
            continue;
        }

        enc.start = (u_char *)tkn;
        enc.length = qptr != NULL ? (size_t)(qptr - tkn) : strlen(tkn);

        /*
         * The wildcard is the whole token, not merely its first character:
         * "*" is a tchar, so "*foo" is a legal (and unknown) coding name, and
         * matching on the first byte alone made it stand for every coding.
         */

        wildcard = (enc.length == 1 && enc.start[0] == '*');

        /*
         * The wildcard names no coding of its own, so it is only recorded
         * here; the pass after the loop turns it into candidates.
         */

        if (wildcard) {
            wildcard_seen = true;
            wildcard_qval = qval;
            continue;
        }

        ecidx = nxt_http_comp_compressor_lookup_enabled(conf, &enc);
        if (ecidx == NXT_HTTP_COMP_SCHEME_UNKNOWN) {
            continue;
        }

        /* A built configuration holds identity at index 0. */

        if (ecidx == NXT_HTTP_COMP_SCHEME_IDENTITY) {
            identity_named = true;
            identity_named_ok = (qval != 0.0);

        } else {
            /*
             * Listed, whether or not it can be applied: the wildcard stands
             * only for the codings the field did not name.  An index past
             * the width of the mask is left out of the wildcard, which is
             * the safe direction -- "compressors" may repeat an encoding, so
             * the index is not bounded by the number of distinct codings.
             */

            if (ecidx < sizeof(named) * 8) {
                named |= (uint32_t) 1 << ecidx;
            }

            /*
             * A coding below its own "min_length" is never applied, so it
             * cannot stand in for a refused identity and it must not outrank
             * a coding that can.  "min_length" is per compressor, so with
             * gzip at 1000 and zstd at 0 a 100-byte response is serveable as
             * zstd; picking gzip on its weight alone and stopping there
             * refused a request that could be satisfied.
             */

            if (nxt_http_comp_below_min_len(clen, &conf->enabled[ecidx])) {
                continue;
            }
        }

        if (qval == 0.0 || qval < weight) {
            continue;
        }

        idx = ecidx;
        weight = qval;
    }

    /*
     * Sect. 12.5.3: identity "is acceptable by default unless specifically
     * excluded by the Accept-Encoding header field stating either
     * 'identity;q=0' or '*;q=0' without a more specific entry for
     * 'identity'".  The explicit token therefore wins outright: in
     * "gzip, identity;q=0.5, *;q=0" the client refused everything it did not
     * name and then named identity as acceptable, so identity is acceptable.
     */

    if (identity_named) {
        identity_allowed = identity_named_ok;

    } else if (wildcard_seen) {
        identity_allowed = (wildcard_qval != 0.0);
    }

    *identity_refused = !identity_allowed;

    /*
     * Sect. 12.5.3: "*" matches any available content coding not explicitly
     * listed in the field.  It therefore stands for every enabled coding the
     * client did not name, and for identity, at its own weight.  Reading it
     * as identity alone left the wildcard unable to select a compressor at
     * all, so "identity;q=0, *;q=1" was answered 406 although the gzip the
     * wildcard offered was enabled and applicable.
     *
     * The pass runs after the loop so that a named coding keeps its own
     * weight whatever its place in the field, and the comparison is strict
     * so that a named coding wins a tie with the wildcard.  Identity is
     * taken before any compressor because all of these stand at the one
     * wildcard weight and identity is the coding that needs nothing applied.
     */

    if (wildcard_qval > weight) {
        if (!identity_named) {
            idx = NXT_HTTP_COMP_SCHEME_IDENTITY;
            weight = wildcard_qval;

        } else {
            for (nxt_uint_t i = 1; i < conf->nr_enabled; i++) {
                const nxt_str_t  *tok = &conf->enabled[i].type->token;

                if (i >= sizeof(named) * 8
                    || (named & ((uint32_t) 1 << i)) != 0)
                {
                    continue;
                }

                /*
                 * A lookup by name only ever finds the first entry for a
                 * coding, so a repeated "compressors" entry is unreachable
                 * by name.  The wildcard must not reach it either, or "*"
                 * would run a coding under options no named request can ask
                 * for.
                 */

                if (nxt_http_comp_compressor_lookup_enabled(conf, tok) != i) {
                    continue;
                }

                if (nxt_http_comp_below_min_len(clen, &conf->enabled[i])) {
                    continue;
                }

                idx = i;
                weight = wildcard_qval;

                break;
            }
        }
    }

    if (idx == NXT_HTTP_COMP_SCHEME_IDENTITY && !identity_allowed) {
        return -1;
    }

    return idx;
}


static nxt_int_t
nxt_http_comp_set_header(const nxt_http_comp_conf_t *conf,
                         nxt_http_request_t *r, nxt_uint_t comp_idx)
{
    const nxt_str_t   *token;
    nxt_http_field_t  *f;

    static const nxt_str_t  content_encoding_str =
                                    nxt_string("Content-Encoding");

    f = nxt_http_resp_field_add(&r->resp, r->mem_pool);
    if (nxt_slow_path(f == NULL)) {
        return NXT_ERROR;
    }

    token = &conf->enabled[comp_idx].type->token;

    *f = (nxt_http_field_t){};

    f->name = content_encoding_str.start;
    f->name_length = content_encoding_str.length;
    f->value = token->start;
    f->value_length = token->length;

    r->resp.content_length = NULL;
    r->resp.content_length_n = -1;

    if (r->resp.mime_type == NULL) {
        nxt_http_field_t *f;

        /*
         * As per RFC 2616 section 4.4 item 3, you should not send
         * Content-Length when a Transfer-Encoding header is present.
         *
         * Skip every Content-Length, not just the first: leaving a second one
         * behind would emit an advertised body length alongside the chunked
         * framing this response now uses, which a downstream parser can use to
         * re-frame the body.  Match on name_length + nxt_memcasecmp() rather
         * than nxt_strcasecmp(): nxt_http_field_t carries a length-tracked
         * name and only happens to be NUL-terminated on the paths that reach
         * compression today (libunit-built app responses and static-file
         * literals), which is not part of the type's contract.
         */
        nxt_http_fields_each(f, r->resp.inline_fields, r->resp.num_inline_fields,
                             r->resp.fields)
        {
            if (f->name_length == nxt_length("Content-Length")
                && nxt_memcasecmp(f->name, "Content-Length",
                                  nxt_length("Content-Length")) == 0)
            {
                f->skip = true;
            }
        } nxt_http_fields_loop;
    }

    return NXT_OK;
}


static bool
nxt_http_comp_is_resp_content_encoded(const nxt_http_request_t *r)
{
    nxt_http_field_t  *f;

    nxt_http_fields_each(f, r->resp.inline_fields, r->resp.num_inline_fields,
                         r->resp.fields)
    {
        if (nxt_strcasecmp(f->name, (const u_char *)"Content-Encoding") == 0) {
            return true;
        }
    } nxt_http_fields_loop;

    return false;
}


/*
 * Adds "Vary: Accept-Encoding", so a shared cache keys on the header that
 * chose this representation.
 *
 * RFC 9110 Sect. 12.5.5: a response that was subject to proactive negotiation
 * must say which request headers it varied on, or a cache is entitled to
 * serve it to a client that would have been given a different representation
 * -- gzip bytes to a client that cannot decode them, or identity to one that
 * could have had the small copy.
 *
 * Emitted on the identity response as well as the coded one.  The identity
 * response is precisely the one a cache must not reuse for a gzip-capable
 * client, so omitting it there would leave the hole open from the other side.
 *
 * This is the companion of weakening the entity-tag for a coded
 * representation: that makes revalidation distinguish the two, this makes the
 * cache key distinguish them.  Either alone leaves shared caches able to mix
 * them.
 */

nxt_int_t
nxt_http_comp_merge_vary(nxt_http_request_t *r)
{
    u_char                  *p, *end, *tok;
    nxt_int_t               len;
    size_t                  keep;
    nxt_http_field_t        *f, *vary;
    nxt_http_fields_iter_t  iter;

    static const nxt_str_t  accept_encoding = nxt_string("Accept-Encoding");

    /*
     * An existing Vary is merged into, not replaced and not deferred to.
     * Something else naming a different header -- "Vary: Origin", say --
     * still needs Accept-Encoding added, because the response varies on both;
     * treating any existing Vary as sufficient would leave the coding out of
     * the cache key, which is the hole this function exists to close.
     */

    vary = NULL;

    for (f = nxt_http_fields_first(&iter, r->resp.inline_fields,
                                   r->resp.num_inline_fields, r->resp.fields);
         f != NULL;
         f = nxt_http_fields_next(&iter))
    {
        if (!f->skip && f->name_length == nxt_length("Vary")
            && nxt_strncasecmp(f->name, (u_char *) "Vary",
                               nxt_length("Vary")) == 0)
        {
            vary = f;
            break;
        }
    }

    if (vary != NULL) {
        p = vary->value;
        end = p + vary->value_length;

        /*
         * "Vary: *" already varies on everything; adding to it says less.
         *
         * Trim both ends before the test.  These are response fields an
         * application handed to libunit, not request headers the parser has
         * normalised, so "Vary: * " arrives with its trailing space intact --
         * and appending to that would emit "* , Accept-Encoding", which is
         * not a valid field value.
         */

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }

        if (end - p == 1 && *p == '*') {
            return NXT_OK;
        }

        /*
         * An empty value carries no tokens, so there is nothing to append to
         * and nothing to search: replacing it avoids emitting ", A-E" with a
         * leading comma.  RFC 9110 Sect. 5.6.1.2 permits the empty element,
         * but there is no reason to produce one.
         */

        end = vary->value + vary->value_length;

        if (end == vary->value) {
            vary->value = accept_encoding.start;
            vary->value_length = accept_encoding.length;

            return NXT_OK;
        }

        /*
         * Already listed?  Compare per token, so "X-Accept-Encoding" misses.
         *
         * A while loop rather than a for with p++: the inner scan can leave p
         * at end, and incrementing there would form a pointer past
         * one-past-the-end, which C does not define even where it is
         * harmless in practice.
         */

        p = vary->value;

        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) {
                p++;
            }

            tok = p;

            while (p < end && *p != ',') {
                p++;
            }

            len = p - tok;

            while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t')) {
                len--;
            }

            if (len == (nxt_int_t) accept_encoding.length
                && nxt_strncasecmp(tok, accept_encoding.start,
                                   accept_encoding.length) == 0)
            {
                return NXT_OK;
            }

            if (p < end) {
                p++;
            }
        }

        /*
         * Append after the last real token, not after whatever the value
         * happens to end with: "Origin," would otherwise become
         * "Origin,, Accept-Encoding".  Empty list elements are legal and
         * ignored (Sect. 5.6.1.2), but there is no reason to emit one.
         */

        keep = vary->value_length;

        while (keep > 0
               && (vary->value[keep - 1] == ' '
                   || vary->value[keep - 1] == '\t'
                   || vary->value[keep - 1] == ','))
        {
            keep--;
        }

        len = keep + nxt_length(", ") + accept_encoding.length;

        p = nxt_mp_nget(r->mem_pool, len);
        if (nxt_slow_path(p == NULL)) {
            return NXT_ERROR;
        }

        nxt_memcpy(p, vary->value, keep);
        nxt_memcpy(p + keep, ", ", nxt_length(", "));
        nxt_memcpy(p + keep + nxt_length(", "),
                   accept_encoding.start, accept_encoding.length);

        vary->value = p;
        vary->value_length = len;

        return NXT_OK;
    }

    f = nxt_http_resp_field_zero_add(&r->resp, r->mem_pool);
    if (nxt_slow_path(f == NULL)) {
        return NXT_ERROR;
    }

    nxt_http_field_name_set(f, "Vary");

    f->value = accept_encoding.start;
    f->value_length = accept_encoding.length;

    return NXT_OK;
}


/*
 * A 406 depends on Accept-Encoding as much as the representation it stands in
 * for, so it carries "Vary: Accept-Encoding" too.  Without it a cache that
 * stores error responses may hand this 406 to a later client that would have
 * been served.  nxt_http_request_error() drops every response field, but
 * nxt_http_request_header_send() re-adds Vary from this flag.
 */

static nxt_int_t
nxt_http_comp_not_acceptable(nxt_http_request_t *r)
{
    r->resp.vary_accept_encoding = 1;

    return NXT_HTTP_NOT_ACCEPTABLE;
}


/*
 * Decides whether an acceptable representation exists, and remembers which
 * compressor would be used, without touching the response or allocating a
 * compressor context.
 *
 * Separated from applying that decision because RFC 9110 Sect. 13.2.1 puts
 * this ahead of precondition evaluation: a request that cannot be satisfied
 * at all must be answered 406, not 304 or 412.  The caller therefore asks
 * this first, evaluates preconditions, and only then applies -- so a 304
 * neither carries a Content-Encoding header nor leaves an initialised
 * compressor behind, which would leak, since the compressor is torn down by
 * the last deflate() call and a 304 makes none.
 */

nxt_int_t
nxt_http_comp_check_acceptable(nxt_task_t *task, nxt_http_request_t *r)
{
    bool                  identity_refused;
    nxt_int_t             ret, idx;
    nxt_str_t             accept_encoding, mime_type = {};
    nxt_http_comp_ctx_t   *ctx = nxt_http_comp_ctx();
    nxt_http_comp_conf_t  *conf = nxt_http_comp_request_conf(r);

    *ctx = (nxt_http_comp_ctx_t){ .resp_clen = -1, .sel_idx = -1 };

    /* A built configuration always holds identity, so NULL is the only
       "no compression" state. */
    if (conf == NULL) {
        return NXT_OK;
    }

    if (r->resp.content_length == NULL && r->resp.content_length_n == -1) {
        return NXT_OK;
    }

    if (r->resp.content_length_n == 0) {
        return NXT_OK;
    }

    /*
     * A 1xx, 204 or 304 describes no representation, so there is nothing for
     * the client to have refused and no 406 to give: negotiation is skipped
     * whatever the request asked for.  The length tests above do not cover
     * this.  An application may send Content-Length with such a status -- a
     * 304 carries the length of the body the client already has -- and
     * nxt_http_response_content_length() stores that field without setting
     * content_length_n, which stays -1.
     *
     * r->status holds the response status at both callers: the application
     * path copies it out of the response before asking, and the static path
     * is at 200 here, ahead of precondition evaluation, so a 406 still
     * outranks the 304 or 412 a validator would give.
     */

    if (nxt_http_status_no_representation(r->status)) {
        return NXT_OK;
    }

    if (nxt_http_comp_is_resp_content_encoded(r)) {
        return NXT_OK;
    }

    ret = nxt_http_comp_accept_encoding(r, &accept_encoding);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

    /*
     * Ask what the client accepts before anything below rules compression
     * out.  Asking afterwards, once the response had been declared
     * serveable, is how the file's own bytes reached a client that had
     * refused them (#390).
     */

    idx = nxt_http_comp_select_compressor(conf, r, &accept_encoding,
                                          &identity_refused);
    if (idx == -1) {
        return nxt_http_comp_not_acceptable(r);
    }

    if (r->resp.mime_type != NULL) {
        mime_type = *r->resp.mime_type;
    } else if (r->resp.content_type != NULL) {
        mime_type.start = r->resp.content_type->value;
        mime_type.length = r->resp.content_type->value_length;
    }

    /*
     * A coding was selected, but the "types" rule below can still withdraw
     * it, and the fallback is always identity.  Where the client refused
     * identity that fallback does not exist, so the answer is 406 rather
     * than the bytes it declined.  "min_length" cannot reach here: a coding
     * below it is not selected in the first place.
     */

    if (mime_type.start == NULL) {
        return identity_refused ? nxt_http_comp_not_acceptable(r) : NXT_OK;
    }

    if (conf->mime_types_rule != NULL) {
        ret = nxt_http_route_test_rule(r, conf->mime_types_rule,
                                       mime_type.start,
                                       mime_type.length);
        if (ret == 0) {
            return identity_refused ? nxt_http_comp_not_acceptable(r) : NXT_OK;
        }
    }

    /*
     * Past every early return above, so this response really was subject to
     * negotiation on Accept-Encoding, whichever coding is chosen below.
     */

    r->resp.vary_accept_encoding = 1;

    if (nxt_slow_path(nxt_http_comp_merge_vary(r) != NXT_OK)) {
        return NXT_ERROR;
    }

    ctx->sel_idx = idx;
    ctx->identity_refused = identity_refused;

    return NXT_OK;
}


/*
 * Weakens the response's ETag, so that a coded representation does not carry
 * the same strong validator as the identity one.
 *
 * RFC 9110 Sect. 8.8.3: a strong validator must change whenever the selected
 * representation changes, and a content coding selects a different
 * representation.  Unit derives the tag from the file's mtime and size, which
 * are the same whichever coding is served, so without this a client that took
 * its tag from a gzip response and then sent it back in If-Range -- which
 * Sect. 13.1.5 compares strongly -- would match, and be handed identity bytes
 * for offsets it believes are gzip offsets.  Weakening makes that strong
 * comparison fail, so the range is simply not applied.
 *
 * If-None-Match keeps working: it compares weakly, so revalidation of a coded
 * representation still answers 304.
 *
 * nginx does the same thing (ngx_http_weak_etag(), called from its gzip
 * header filter); Apache appends "-gzip" and Go's net/http appends the coding
 * name.  All three make the tag differ per coding; weakening is the smallest
 * of the three and needs no extra allocation beyond the prefix.
 */

static nxt_int_t
nxt_http_comp_weaken_etag(nxt_http_request_t *r)
{
    u_char                  *p;
    nxt_http_field_t        *f;
    nxt_http_fields_iter_t  iter;

    for (f = nxt_http_fields_first(&iter, r->resp.inline_fields,
                                   r->resp.num_inline_fields, r->resp.fields);
         f != NULL;
         f = nxt_http_fields_next(&iter))
    {
        if (f->skip || f->name_length != nxt_length("ETag")
            || nxt_strncasecmp(f->name, (u_char *) "ETag",
                               nxt_length("ETag")) != 0)
        {
            continue;
        }

        if (f->value_length >= 2
            && f->value[0] == 'W' && f->value[1] == '/')
        {
            return NXT_OK;
        }

        p = nxt_mp_nget(r->mem_pool, f->value_length + nxt_length("W/"));
        if (nxt_slow_path(p == NULL)) {
            return NXT_ERROR;
        }

        /* Copy out of the old value before the field is repointed. */
        nxt_memcpy(p, "W/", nxt_length("W/"));
        nxt_memcpy(p + nxt_length("W/"), f->value, f->value_length);

        f->value = p;
        f->value_length += nxt_length("W/");

        return NXT_OK;
    }

    return NXT_OK;
}


/*
 * Applies the decision nxt_http_comp_check_acceptable() reached: adds the
 * Content-Encoding header and initialises the compressor.  Call it only on a
 * path that will actually send a body.
 */

nxt_int_t
nxt_http_comp_apply_compression(nxt_task_t *task, nxt_http_request_t *r)
{
    int                         err;
    nxt_int_t                   idx;
    nxt_http_comp_ctx_t         *ctx = nxt_http_comp_ctx();
    nxt_http_comp_conf_t        *conf;
    nxt_http_comp_compressor_t  *compressor;

    idx = ctx->sel_idx;

    if (idx == -1 || idx == NXT_HTTP_COMP_SCHEME_IDENTITY) {
        return NXT_OK;
    }

    /*
     * Reached only when nxt_http_comp_check_acceptable() chose a compressor,
     * which it does only from a non-NULL configuration.
     */
    conf = nxt_http_comp_request_conf(r);
    compressor = &conf->enabled[idx];

    ctx->resp_clen = nxt_http_comp_resp_length(r);

    if (nxt_http_comp_below_min_len(ctx->resp_clen, compressor)) {
        return NXT_OK;
    }

    nxt_http_comp_set_header(conf, r, idx);

    if (nxt_slow_path(nxt_http_comp_weaken_etag(r) != NXT_OK)) {
        return NXT_ERROR;
    }

    ctx->idx = idx;
    ctx->type = compressor->type;
    ctx->ctx.level = compressor->opts.level;

    err = compressor->type->cops->init(&ctx->ctx);
    if (nxt_slow_path(err)) {
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_uint_t
nxt_http_comp_compressor_token2idx(const nxt_str_t *token)
{
    for (nxt_uint_t i = 0; i < nxt_nitems(nxt_http_comp_compressors); i++) {
        if (nxt_strstr_eq(token, &nxt_http_comp_compressors[i].token)) {
            return i;
        }
    }

    return NXT_HTTP_COMP_SCHEME_UNKNOWN;
}


bool
nxt_http_comp_compressor_is_valid(const nxt_str_t *token)
{
    nxt_uint_t  idx;

    idx = nxt_http_comp_compressor_token2idx(token);
    if (idx != NXT_HTTP_COMP_SCHEME_UNKNOWN) {
        return true;
    }

    return false;
}


static nxt_int_t
nxt_http_comp_set_compressor(nxt_task_t *task, nxt_router_conf_t *rtcf,
                             nxt_http_comp_conf_t *conf,
                             const nxt_conf_value_t *comp, nxt_uint_t index)
{
    nxt_int_t                   ret;
    nxt_str_t                   token;
    nxt_uint_t                  cidx;
    nxt_conf_value_t            *obj;
    nxt_http_comp_compressor_t  *compr;

    static const nxt_str_t  token_str = nxt_string("encoding");

    obj = nxt_conf_get_object_member(comp, &token_str, NULL);
    if (obj == NULL) {
        return NXT_ERROR;
    }

    nxt_conf_get_string(obj, &token);
    cidx = nxt_http_comp_compressor_token2idx(&token);

    compr = &conf->enabled[index];

    compr->type = &nxt_http_comp_compressors[cidx];
    compr->opts.level = compr->type->def_compr;
    compr->opts.min_len = -1;

    ret = nxt_conf_map_object(rtcf->mem_pool, comp,
                              nxt_http_comp_compressors_opts_map,
                              nxt_nitems(nxt_http_comp_compressors_opts_map),
                              &compr->opts);
    if (nxt_slow_path(ret == NXT_ERROR)) {
        return NXT_ERROR;
    }

    if (compr->opts.level < compr->type->comp_min
        || compr->opts.level > compr->type->comp_max)
    {
        nxt_log(task, NXT_LOG_NOTICE,
                "Overriding invalid compression level for [%V] [%d] -> [%d]",
                &compr->type->token, compr->opts.level,
                compr->type->def_compr);
        compr->opts.level = compr->type->def_compr;
    }

    return NXT_OK;
}


nxt_int_t
nxt_http_comp_compression_init(nxt_task_t *task, nxt_router_conf_t *rtcf,
                               const nxt_conf_value_t *comp_conf)
{
    nxt_int_t             ret;
    nxt_uint_t            n = 1;  /* 'identity' */
    nxt_conf_value_t      *comps, *mimes;
    nxt_http_comp_conf_t  *conf;

    static const nxt_str_t  comps_str = nxt_string("compressors");
    static const nxt_str_t  mimes_str = nxt_string("types");

    conf = nxt_mp_zalloc(rtcf->mem_pool, sizeof(nxt_http_comp_conf_t));
    if (nxt_slow_path(conf == NULL)) {
        return NXT_ERROR;
    }

    mimes = nxt_conf_get_object_member(comp_conf, &mimes_str, NULL);
    if (mimes != NULL) {
        conf->mime_types_rule =
                        nxt_http_route_types_rule_create(task,
                                                         rtcf->mem_pool, mimes);
        if (nxt_slow_path(conf->mime_types_rule == NULL)) {
            return NXT_ERROR;
        }
    }

    comps = nxt_conf_get_object_member(comp_conf, &comps_str, NULL);
    if (nxt_slow_path(comps == NULL)) {
        return NXT_ERROR;
    }

    if (nxt_conf_type(comps) == NXT_CONF_OBJECT) {
        n++;
    } else {
        n += nxt_conf_object_members_count(comps);
    }
    conf->nr_enabled = n;

    conf->enabled = nxt_mp_zalloc(rtcf->mem_pool,
                                  sizeof(nxt_http_comp_compressor_t) * n);
    if (nxt_slow_path(conf->enabled == NULL)) {
        return NXT_ERROR;
    }

    conf->enabled[0] =
        (nxt_http_comp_compressor_t){ .type = &nxt_http_comp_compressors[0],
                                      .opts.level = NXT_COMP_LEVEL_UNSET,
                                      .opts.min_len = -1 };

    if (nxt_conf_type(comps) == NXT_CONF_OBJECT) {
        ret = nxt_http_comp_set_compressor(task, rtcf, conf, comps, 1);
        if (nxt_slow_path(ret == NXT_ERROR)) {
            return NXT_ERROR;
        }

    } else {
        for (nxt_uint_t i = 1; i < conf->nr_enabled; i++) {
            nxt_conf_value_t  *obj;

            obj = nxt_conf_get_array_element(comps, i - 1);
            ret = nxt_http_comp_set_compressor(task, rtcf, conf, obj, i);
            if (ret == NXT_ERROR) {
                return NXT_ERROR;
            }
        }
    }

    /* One publish point: a failure above leaves rtcf->compression NULL. */

    rtcf->compression = conf;

    return NXT_OK;
}
