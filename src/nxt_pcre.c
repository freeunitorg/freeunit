/*
 * Copyright (C) Axel Duch
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_regex.h>
#include <pcre.h>


struct nxt_regex_s {
    pcre       *code;
    nxt_str_t  pattern;
};

/*
 * The limits and the match data of one request.  "extra" passes the
 * structure to the callout, which counts down "budget".
 */

struct nxt_regex_match_s {
    pcre_extra     extra;
    uint32_t       budget;
    int            ovecsize;
    int            ovec[];
};


static void *nxt_pcre_malloc(size_t size);
static void nxt_pcre_free(void *p);
static int nxt_pcre_callout(pcre_callout_block *block);

static nxt_mp_t  *nxt_pcre_mp;

/*
 * The most nested calls that one match may make.  PCRE 1 makes them on the
 * stack, 512 bytes each in libpcre3 8.39 on x86-64.  2,000 calls use about
 * 1 MB.  A request matches on a router thread with the default glibc stack:
 * 8 MB, or 2 MB when the stack rlimit is unlimited.
 */
#define NXT_PCRE_RECURSION_LIMIT  2000


nxt_regex_t *
nxt_regex_compile(nxt_mp_t *mp, nxt_str_t *source, nxt_regex_err_t *err)
{
    int            erroffset;
    char           *pattern;
    void           *saved_malloc, *saved_free;
    nxt_regex_t    *re;
    unsigned long  options;

    err->offset = source->length;

    re = nxt_mp_get(mp, sizeof(nxt_regex_t) + source->length + 1);
    if (nxt_slow_path(re == NULL)) {
        err->msg = "memory allocation failed";
        return NULL;
    }

    pattern = nxt_pointer_to(re, sizeof(nxt_regex_t));

    nxt_memcpy(pattern, source->start, source->length);
    pattern[source->length] = '\0';

    re->pattern.length = source->length;
    re->pattern.start = (u_char *) pattern;

    saved_malloc = pcre_malloc;
    saved_free = pcre_free;

    pcre_malloc = nxt_pcre_malloc;
    pcre_free = nxt_pcre_free;
    nxt_pcre_mp = mp;

    re->code = pcre_compile(pattern, 0, &err->msg, &erroffset, NULL);

    /*
     * The library counts match steps from zero again at each start position
     * of a pattern that is not anchored.  So the match limit does not bound
     * one whole match of it.  Such a pattern is compiled again with a
     * callout before each item.  One match may make at most
     * NXT_REGEX_MATCH_LIMIT callouts in total.  The callouts do not change
     * the result.  An anchored pattern has one start position and gets no
     * callouts.
     *
     * PCRE_INFO_OPTIONS writes an unsigned long.
     */

    if (re->code != NULL
        && pcre_fullinfo(re->code, NULL, PCRE_INFO_OPTIONS, &options) == 0
        && (options & PCRE_ANCHORED) == 0)
    {
        re->code = pcre_compile(pattern, PCRE_AUTO_CALLOUT, &err->msg,
                                &erroffset, NULL);
    }

    if (nxt_slow_path(re->code == NULL)) {
        err->offset = erroffset;
        re = NULL;
    }

    /*
     * The callout function is global in PCRE 1.  It is set here, before any
     * match with the pattern.  No other code in the process sets it.
     */

    if (pcre_callout != nxt_pcre_callout) {
        pcre_callout = nxt_pcre_callout;
    }

    pcre_malloc = saved_malloc;
    pcre_free = saved_free;

    return re;
}


static void*
nxt_pcre_malloc(size_t size)
{
    if (nxt_slow_path(nxt_pcre_mp == NULL)) {
        nxt_thread_log_alert("pcre_malloc(%uz) called without memory pool",
                             size);
        return NULL;
    }

    nxt_thread_log_debug("pcre_malloc(%uz), pool %p", size, nxt_pcre_mp);

    return nxt_mp_get(nxt_pcre_mp, size);
}


static void
nxt_pcre_free(void *p)
{
}


static int
nxt_pcre_callout(pcre_callout_block *block)
{
    nxt_regex_match_t  *match;

    match = block->callout_data;

    /* A negative value stops the whole match with this value. */

    if (match->budget == 0) {
        return PCRE_ERROR_CALLOUT;
    }

    match->budget--;

    return 0;
}


nxt_regex_match_t *
nxt_regex_match_create(nxt_mp_t *mp, size_t size)
{
    nxt_regex_match_t  *match;

    match = nxt_mp_zget(mp, sizeof(nxt_regex_match_t) + sizeof(int) * size);
    if (nxt_fast_path(match != NULL)) {
        match->extra.flags = PCRE_EXTRA_MATCH_LIMIT
                             | PCRE_EXTRA_MATCH_LIMIT_RECURSION
                             | PCRE_EXTRA_CALLOUT_DATA;
        match->extra.match_limit = NXT_REGEX_MATCH_LIMIT;
        match->extra.match_limit_recursion = NXT_PCRE_RECURSION_LIMIT;
        match->extra.callout_data = match;
        match->ovecsize = size;
    }

    return match;
}


nxt_int_t
nxt_regex_match(nxt_regex_t *re, u_char *subject, size_t length,
    nxt_regex_match_t *match)
{
    int  ret;

    match->budget = NXT_REGEX_MATCH_LIMIT;

    ret = pcre_exec(re->code, &match->extra, (const char *) subject, length,
                    0, 0, match->ovec, match->ovecsize);
    if (nxt_slow_path(ret < PCRE_ERROR_NOMATCH)) {

        /*
         * A client can cause these errors on each request.  So the log gets
         * no subject bytes for a limit, and at most 64 for others.  The
         * callout returns PCRE_ERROR_CALLOUT when one match makes more
         * than NXT_REGEX_MATCH_LIMIT callouts.
         */

        if (ret == PCRE_ERROR_MATCHLIMIT || ret == PCRE_ERROR_CALLOUT
            || ret == PCRE_ERROR_RECURSIONLIMIT)
        {
            nxt_thread_log_error(NXT_LOG_WARN,
                                 "pcre_exec() reached the %s limit on %uz "
                                 "bytes using \"%V\"",
                                 (ret == PCRE_ERROR_RECURSIONLIMIT)
                                 ? "recursion" : "match",
                                 length, &re->pattern);

            return NXT_ERROR;
        }

        length = nxt_min(length, NXT_REGEX_LOG_SUBJECT);

        nxt_thread_log_error(NXT_LOG_ERR,
                             "pcre_exec() failed: %d on \"%*s\" using \"%V\"",
                             ret, length, subject, &re->pattern);

        return NXT_ERROR;
    }

    return (ret != PCRE_ERROR_NOMATCH);
}
