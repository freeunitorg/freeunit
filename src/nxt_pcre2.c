/*
 * Copyright (C) Axel Duch
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_regex.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>


/*
 * The most heap memory in KiB that one match may use for backtracking
 * frames.  In PCRE2 10.42 on x86-64, a frame takes 128 bytes, plus 16
 * bytes for each capturing group.  100,000 frames of 160 bytes take about
 * 16 MB.  The match limit permits no more frames than that, so this limit
 * can stop a pattern with several capturing groups first: three with
 * PCRE2 10.42, two with 10.46, where a frame is 8 bytes larger.  The
 * library default is about 20 GB.
 */
#define NXT_PCRE2_HEAP_LIMIT  16384

/*
 * The most nested calls that one match may make with PCRE2 before 10.30.
 * These versions make the calls on the stack, 448 bytes each in PCRE2 10.23
 * of Amazon Linux 2 on x86-64.  2,000 calls use about 900 KB.  A request
 * matches on a router thread with the default glibc stack: 8 MB, or 2 MB
 * when the stack rlimit is unlimited.  The library default is 10,000,000.
 */
#define NXT_PCRE2_RECURSION_LIMIT  2000


static void *nxt_pcre2_malloc(PCRE2_SIZE size, void *memory_data);
static void nxt_pcre2_free(void *p, void *memory_data);
static int nxt_pcre2_callout(pcre2_callout_block *block, void *data);
static void nxt_pcre2_match_cleanup(nxt_task_t *task, void *obj, void *data);


struct nxt_regex_s {
    pcre2_code  *code;
    nxt_str_t   pattern;
};


/*
 * The match data and the match context of one request.  The callout of the
 * context gets the structure and counts down "budget".
 */

struct nxt_regex_match_s {
    pcre2_match_data     *data;
    pcre2_match_context  *ctx;
    uint32_t             budget;
};


nxt_regex_t *
nxt_regex_compile(nxt_mp_t *mp, nxt_str_t *source, nxt_regex_err_t *err)
{
    int                    errcode;
    uint32_t               options;
    nxt_int_t              ret;
    PCRE2_SIZE             erroffset;
    pcre2_code             *code;
    nxt_regex_t            *re;
    pcre2_general_context  *general_ctx;
    pcre2_compile_context  *compile_ctx;

    static const u_char    alloc_error[] = "memory allocation failed";

    general_ctx = pcre2_general_context_create(nxt_pcre2_malloc,
                                               nxt_pcre2_free, mp);
    if (nxt_slow_path(general_ctx == NULL)) {
        goto alloc_fail;
    }

    compile_ctx = pcre2_compile_context_create(general_ctx);
    if (nxt_slow_path(compile_ctx == NULL)) {
        goto alloc_fail;
    }

    re = nxt_mp_get(mp, sizeof(nxt_regex_t));
    if (nxt_slow_path(re == NULL)) {
        goto alloc_fail;
    }

    if (nxt_slow_path(nxt_str_dup(mp, &re->pattern, source) == NULL)) {
        goto alloc_fail;
    }

    re->code = pcre2_compile((PCRE2_SPTR) source->start, source->length, 0,
                             &errcode, &erroffset, compile_ctx);
    if (nxt_slow_path(re->code == NULL)) {
        goto compile_fail;
    }

    /*
     * The library counts match steps from zero again at each start position
     * of a pattern that is not anchored.  So the match limit does not bound
     * one whole match of it.  Such a pattern is compiled again with a
     * callout before each item.  One match may make at most
     * NXT_REGEX_MATCH_LIMIT callouts in total.  The callouts do not change
     * the result.  An anchored pattern has one start position and gets no
     * callouts.
     */

    if (pcre2_pattern_info(re->code, PCRE2_INFO_ALLOPTIONS, &options) == 0
        && (options & PCRE2_ANCHORED) == 0)
    {
        code = pcre2_compile((PCRE2_SPTR) source->start, source->length,
                             PCRE2_AUTO_CALLOUT, &errcode, &erroffset,
                             compile_ctx);
        if (nxt_slow_path(code == NULL)) {
            goto compile_fail;
        }

        pcre2_code_free(re->code);
        re->code = code;
    }

#if 0
    /*
     * The JIT honours the match limit and callouts but not the heap limit,
     * and it has its own stack limit.  This matters if the JIT is enabled
     * again.
     */

    errcode = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
    if (nxt_slow_path(errcode != 0 && errcode != PCRE2_ERROR_JIT_BADOPTION)) {
        ret = pcre2_get_error_message(errcode, (PCRE2_UCHAR *) err->msg,
                                      ERR_BUF_SIZE);
        if (ret < 0) {
            (void) nxt_sprintf(err->msg, err->msg + ERR_BUF_SIZE,
                               "JIT compilation failed with unknown "
                               "error code: %d%Z", errcode);
        }

        return NULL;
    }
#endif

    return re;

compile_fail:

    err->offset = erroffset;

    ret = pcre2_get_error_message(errcode, (PCRE2_UCHAR *) err->msg,
                                  ERR_BUF_SIZE);
    if (ret < 0) {
        (void) nxt_sprintf(err->msg, err->msg + ERR_BUF_SIZE,
                           "compilation failed with unknown "
                           "error code: %d%Z", errcode);
    }

    return NULL;

alloc_fail:

    err->offset = source->length;
    nxt_memcpy(err->msg, alloc_error, sizeof(alloc_error));

    return NULL;
}


static void *
nxt_pcre2_malloc(PCRE2_SIZE size, void *mp)
{
    return nxt_mp_get(mp, size);
}


static void
nxt_pcre2_free(void *p, void *mp)
{
}


static int
nxt_pcre2_callout(pcre2_callout_block *block, void *data)
{
    nxt_regex_match_t  *match;

    match = data;

    /* A negative value stops the whole match with this value. */

    if (match->budget == 0) {
        return PCRE2_ERROR_CALLOUT;
    }

    match->budget--;

    return 0;
}


static void
nxt_pcre2_match_cleanup(nxt_task_t *task, void *obj, void *data)
{
    pcre2_match_context_free(obj);
}


nxt_regex_match_t *
nxt_regex_match_create(nxt_mp_t *mp, size_t size)
{
    nxt_regex_match_t      *match;
    pcre2_match_context    *ctx;
    pcre2_general_context  *general_ctx;

    match = nxt_mp_get(mp, sizeof(nxt_regex_match_t));
    if (nxt_slow_path(match == NULL)) {
        return NULL;
    }

    general_ctx = pcre2_general_context_create(nxt_pcre2_malloc,
                                               nxt_pcre2_free, mp);
    if (nxt_slow_path(general_ctx == NULL)) {
        nxt_thread_log_alert("pcre2_general_context_create() failed");
        return NULL;
    }

    match->data = pcre2_match_data_create(size, general_ctx);
    if (nxt_slow_path(match->data == NULL)) {
        nxt_thread_log_alert("pcre2_match_data_create(%uz) failed", size);
        return NULL;
    }

    /*
     * PCRE2 10.30 to 10.40 allocate the backtracking frames of a match with
     * the allocator of the match context, and free them after the match.
     * The pool does not free memory.  So the context uses the system
     * allocator, and the pool frees the context when the pool is destroyed.
     */

    ctx = pcre2_match_context_create(NULL);
    if (nxt_slow_path(ctx == NULL)) {
        nxt_thread_log_alert("pcre2_match_context_create() failed");
        return NULL;
    }

    if (nxt_slow_path(nxt_mp_cleanup(mp, nxt_pcre2_match_cleanup, NULL, ctx,
                                     NULL)
                      != NXT_OK))
    {
        pcre2_match_context_free(ctx);
        return NULL;
    }

    (void) pcre2_set_match_limit(ctx, NXT_REGEX_MATCH_LIMIT);

#if (PCRE2_MAJOR > 10 || PCRE2_MINOR >= 30)
    (void) pcre2_set_heap_limit(ctx, NXT_PCRE2_HEAP_LIMIT);
#else
    (void) pcre2_set_recursion_limit(ctx, NXT_PCRE2_RECURSION_LIMIT);
#endif

    (void) pcre2_set_callout(ctx, nxt_pcre2_callout, match);

    match->ctx = ctx;

    return match;
}


nxt_int_t
nxt_regex_match(nxt_regex_t *re, u_char *subject, size_t length,
    nxt_regex_match_t *match)
{
    nxt_int_t    ret;
    const char   *limit;
    PCRE2_UCHAR  errptr[ERR_BUF_SIZE];

    match->budget = NXT_REGEX_MATCH_LIMIT;

    ret = pcre2_match(re->code, (PCRE2_SPTR) subject, length, 0, 0,
                      match->data, match->ctx);

    if (nxt_slow_path(ret < PCRE2_ERROR_NOMATCH)) {

        /*
         * A client can cause these errors on each request.  So the log gets
         * no subject bytes for a limit, and at most 64 for others.  The
         * callout returns PCRE2_ERROR_CALLOUT when one match makes more
         * than NXT_REGEX_MATCH_LIMIT callouts.
         */

        switch (ret) {

        case PCRE2_ERROR_MATCHLIMIT:
        case PCRE2_ERROR_CALLOUT:
            limit = "match";
            break;

#if defined(PCRE2_ERROR_HEAPLIMIT)
        case PCRE2_ERROR_HEAPLIMIT:
            limit = "heap";
            break;
#endif

        /* PCRE2 10.30 and later call it the depth limit. */

        case PCRE2_ERROR_RECURSIONLIMIT:
            limit = "recursion";
            break;

        default:
            limit = NULL;
            break;
        }

        if (limit != NULL) {
            nxt_thread_log_error(NXT_LOG_WARN,
                                 "pcre2_match() reached the %s limit on %uz "
                                 "bytes using \"%V\"", limit, length,
                                 &re->pattern);

            return NXT_ERROR;
        }

        length = nxt_min(length, NXT_REGEX_LOG_SUBJECT);

        if (pcre2_get_error_message(ret, errptr, ERR_BUF_SIZE) < 0) {
            nxt_thread_log_error(NXT_LOG_ERR,
                                 "pcre2_match() failed: %d on \"%*s\" "
                                 "using \"%V\"", ret, length, subject,
                                 &re->pattern);

        } else {
            nxt_thread_log_error(NXT_LOG_ERR,
                                 "pcre2_match() failed: %s (%d) on \"%*s\" "
                                 "using \"%V\"", errptr, ret, length, subject,
                                 &re->pattern);
        }

        return NXT_ERROR;
    }

    return (ret != PCRE2_ERROR_NOMATCH);
}
