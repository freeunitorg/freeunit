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

/*
 * A pattern is compiled in UTF mode, so "." and a character class match one
 * whole UTF-8 character, and "\xHH" is a code point.  A subject such as a
 * request path or a header value may hold any bytes.  In UTF mode
 * pcre2_match() checks the subject first and returns one of the
 * PCRE2_ERROR_UTF8_ERR* codes if it is not valid UTF-8.  Such a subject is
 * then matched with a twin of the pattern that is compiled without UTF
 * mode, so it gets the same result as before UTF mode.
 *
 * PCRE2_MATCH_INVALID_UTF is not used.  With it, a pattern item that must
 * match an invalid byte does not match, so a client could choose a byte
 * that makes a deny rule such as "~^/admin/.*$" not match.  The library also
 * restarts the match at each valid fragment of the subject with a new
 * match step count, and before 10.48 it is not safe with a subject from a
 * client (GHSA-9qww-pwc4-77qq).
 */
#define NXT_PCRE2_UTF_ERROR(ret)                                              \
    ((ret) <= PCRE2_ERROR_UTF8_ERR1 && (ret) >= PCRE2_ERROR_UTF8_ERR21)


static void *nxt_pcre2_malloc(PCRE2_SIZE size, void *memory_data);
static void nxt_pcre2_free(void *p, void *memory_data);
static int nxt_pcre2_callout(pcre2_callout_block *block, void *data);
static void nxt_pcre2_match_cleanup(nxt_task_t *task, void *obj, void *data);
static void nxt_pcre2_match_data_cleanup(nxt_task_t *task, void *obj,
    void *data);
static void nxt_pcre2_code_cleanup(nxt_task_t *task, void *obj, void *data);
static pcre2_code *nxt_pcre2_compile(nxt_str_t *source, uint32_t options,
    pcre2_compile_context *ctx, int *errcode, PCRE2_SIZE *erroffset);
static nxt_bool_t nxt_pcre2_jit_compile(pcre2_code *code);
static uint32_t nxt_pcre2_utf_options(void);
#if defined(PCRE2_EXTRA_CASELESS_RESTRICT)
static nxt_bool_t nxt_pcre2_caseless_restrict(void);
#endif
static nxt_int_t nxt_pcre2_match(pcre2_code *code, u_char *subject,
    size_t length, uint32_t options, nxt_bool_t jit, nxt_regex_match_t *match);
static nxt_bool_t nxt_pcre2_ascii(const u_char *p, size_t length);
static size_t nxt_pcre2_byte_escape(nxt_str_t *source);
static nxt_bool_t nxt_pcre2_hex(u_char c);
static nxt_uint_t nxt_pcre2_hex_value(u_char c);
static nxt_bool_t nxt_pcre2_utf(pcre2_code *code);

#if (NXT_TESTS)
static nxt_uint_t                   nxt_pcre2_test_mode;
static nxt_regex_jit_test_stats_t   nxt_pcre2_test_stats;

nxt_uint_t  nxt_regex_test_match_creates;
nxt_uint_t  nxt_regex_test_warnings;
nxt_uint_t  nxt_regex_test_no_utf_check;

void
nxt_regex_jit_test_mode(nxt_uint_t mode)
{
    nxt_pcre2_test_mode = mode;
}

nxt_regex_jit_test_stats_t
nxt_regex_jit_test_stats(void)
{
    return nxt_pcre2_test_stats;
}

nxt_bool_t
nxt_regex_jit_test_available(void)
{
    int         errcode;
    uint32_t    available;
    PCRE2_SIZE  offset;
    pcre2_code  *code;
    nxt_bool_t  usable;

    available = 0;
    if (pcre2_config(PCRE2_CONFIG_JIT, &available) != 0 || available == 0) {
        return 0;
    }

    code = pcre2_compile((PCRE2_SPTR) "a", 1, 0, &errcode, &offset, NULL);
    if (code == NULL) {
        return 0;
    }
    usable = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE) == 0;
    pcre2_code_free(code);
    return usable;
}

#if (NXT_REGEX_JIT)
static pcre2_jit_stack *
nxt_pcre2_test_jit_stack(void *data)
{
    nxt_pcre2_test_stats.executed++;
    return NULL;
}
#endif
#endif


struct nxt_regex_s {
    pcre2_code    *code;
    pcre2_code    *bytes;
    nxt_str_t     pattern;
    nxt_bool_t    jit;
    nxt_bool_t    bytes_jit;
    nxt_bool_t    utf;
    nxt_atomic_t  warned_limit;
    nxt_atomic_t  warned_utf;
};


/*
 * The match data and the match context of one router thread.  The callout
 * of the context gets the structure and counts down "budget".
 */

struct nxt_regex_match_s {
    pcre2_match_data     *data;
    pcre2_match_context  *ctx;
    uint32_t             budget;
};


nxt_regex_t *
nxt_regex_compile(nxt_mp_t *mp, nxt_str_t *source, nxt_regex_err_t *err)
{
    int                    errcode, utf_errcode;
    uint32_t               utf;
    nxt_int_t              ret;
    PCRE2_SIZE             erroffset, utf_erroffset;
    nxt_regex_t            *re;
    pcre2_general_context  *general_ctx;
    pcre2_compile_context  *compile_ctx;
    PCRE2_UCHAR            errmsg[ERR_BUF_SIZE];

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

    re->code = NULL;
    re->bytes = NULL;

    utf = nxt_pcre2_utf_options();

#if defined(PCRE2_EXTRA_CASELESS_RESTRICT)
    /*
     * In UTF mode a caseless "k" also matches U+212A and "s" matches U+017F.
     * So a caseless pattern that starts with them loses the search for its
     * first code unit, and a rule written for ASCII matches more.  Keep
     * ASCII and non-ASCII characters apart in caseless matching.  The
     * option came in PCRE2 10.43; an older library that runs with newer
     * headers rejects it, so it is used only if the library accepts it.
     */

    if (utf != 0 && nxt_pcre2_caseless_restrict()) {
        (void) pcre2_set_compile_extra_options(compile_ctx,
                                               PCRE2_EXTRA_CASELESS_RESTRICT);
    }
#endif

    if (utf != 0) {
        re->code = nxt_pcre2_compile(source, utf, compile_ctx, &utf_errcode,
                                     &utf_erroffset);
    }

    re->bytes = nxt_pcre2_compile(source, 0, compile_ctx, &errcode,
                                  &erroffset);

    if (re->code == NULL) {

        /*
         * A pattern that UTF mode does not accept, such as one with invalid
         * UTF-8 from an old configuration, matches bytes, as before.  If it
         * is not valid without UTF mode either, the UTF mode error is
         * reported.
         */

        if (nxt_slow_path(re->bytes == NULL)) {
            if (utf != 0) {
                errcode = utf_errcode;
                erroffset = utf_erroffset;
            }

            goto compile_fail;
        }

        if (utf != 0) {
            if (pcre2_get_error_message(utf_errcode, errmsg, ERR_BUF_SIZE)
                < 0)
            {
                errmsg[0] = '\0';
            }

            nxt_thread_log_error(NXT_LOG_NOTICE,
                                 "regex \"%V\" matches bytes, not UTF-8 "
                                 "characters: %s at offset %uz",
                                 &re->pattern, errmsg, utf_erroffset);
        }

        re->code = re->bytes;
        re->bytes = NULL;
    }

    /*
     * A pattern such as "\x{43f}" is valid only in UTF mode, so it has no
     * twin.  A pattern that starts with "(*UTF)" is compiled in UTF mode
     * even without the option, so its twin is a UTF code too and is of no
     * use: it is freed.  A subject with invalid UTF-8 then gives an error,
     * as before UTF mode.
     */

    if (re->bytes != NULL && nxt_pcre2_utf(re->bytes)) {
        pcre2_code_free(re->bytes);
        re->bytes = NULL;
    }

    /* Only a UTF code checks the subject, so only it needs the ASCII test. */

    re->utf = nxt_pcre2_utf(re->code);

    re->jit = 0;
    re->bytes_jit = 0;
    re->warned_limit = 0;
    re->warned_utf = 0;

    /* JIT executable memory is not owned by the configuration pool. */
#if (NXT_TESTS)
    if (nxt_pcre2_test_mode == 4) {
        ret = NXT_ERROR;
    } else
#endif
    {
        ret = nxt_mp_cleanup(mp, nxt_pcre2_code_cleanup, NULL, re, NULL);
    }

    if (ret != NXT_OK) {
        pcre2_code_free(re->code);
        if (re->bytes != NULL) {
            pcre2_code_free(re->bytes);
        }
        goto alloc_fail;
    }

    re->jit = nxt_pcre2_jit_compile(re->code);
#if (NXT_TESTS)
    if (re->jit) {
        nxt_pcre2_test_stats.compiled++;
    }
#endif

    if (re->bytes != NULL) {
        re->bytes_jit = nxt_pcre2_jit_compile(re->bytes);
    }

    /*
     * In UTF mode "\xc3\xa9" is the two code points U+00C3 U+00A9, not the
     * UTF-8 bytes of U+00E9, so a pattern written for bytes no longer
     * matches that character.  Tell the operator at configuration load.
     */

    if (re->bytes != NULL) {
        erroffset = nxt_pcre2_byte_escape(source);

        if (erroffset != source->length) {
            nxt_thread_log_error(NXT_LOG_NOTICE,
                                 "regex \"%V\" has a byte escape at offset "
                                 "%uz; in UTF mode \"\\xHH\" is a code "
                                 "point, not a byte: write a non-ASCII "
                                 "character as itself",
                                 &re->pattern, erroffset);
        }
    }

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


/*
 * The library counts match steps from zero again at each start position of
 * a pattern that is not anchored.  So the match limit does not bound one
 * whole match of it.  Such a pattern is compiled again with a callout before
 * each item.  One match may make at most NXT_REGEX_MATCH_LIMIT callouts in
 * total.  The callouts do not change the result.  An anchored pattern has
 * one start position and gets no callouts.
 */

static pcre2_code *
nxt_pcre2_compile(nxt_str_t *source, uint32_t options,
    pcre2_compile_context *ctx, int *errcode, PCRE2_SIZE *erroffset)
{
    uint32_t    info;
    pcre2_code  *code, *callout;

    code = pcre2_compile((PCRE2_SPTR) source->start, source->length, options,
                         errcode, erroffset, ctx);
    if (code == NULL) {
        return NULL;
    }

    if (pcre2_pattern_info(code, PCRE2_INFO_ALLOPTIONS, &info) == 0
        && (info & PCRE2_ANCHORED) == 0)
    {
        callout = pcre2_compile((PCRE2_SPTR) source->start, source->length,
                                options | PCRE2_AUTO_CALLOUT, errcode,
                                erroffset, ctx);

        pcre2_code_free(code);
        code = callout;
    }

    return code;
}


static nxt_bool_t
nxt_pcre2_jit_compile(pcre2_code *code)
{
#if (NXT_REGEX_JIT)
    int         errcode;
    uint32_t    available;
    PCRE2_SIZE  jit_size;

    available = 0;
    (void) pcre2_config(PCRE2_CONFIG_JIT, &available);

#if (NXT_TESTS)
    if (nxt_pcre2_test_mode == 1) {
        available = 0;
    }
#endif

    if (available == 0) {
        return 0;
    }

#if (NXT_TESTS)
    if (nxt_pcre2_test_mode == 2) {
        errcode = PCRE2_ERROR_NOMEMORY;
    } else
#endif
    {
        errcode = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);
    }

    jit_size = 0;

    return (errcode == 0
            && pcre2_pattern_info(code, PCRE2_INFO_JITSIZE, &jit_size) == 0
            && jit_size != 0);
#else
    return 0;
#endif
}


/*
 * Returns the offset of the first escape of a code from 0x80 to 0xff in the
 * pattern, "\xHH" or "\x{HH}", else its length.  Such an escape is a byte
 * in byte mode and a code point in UTF mode.  An escaped backslash is
 * skipped, so "\\x80" is not an escape.  Leading zeros in "\x{...}" count:
 * "\x{00e9}" is U+00E9 too.  A false notice costs nothing.
 */

static size_t
nxt_pcre2_byte_escape(nxt_str_t *source)
{
    u_char      *p, *q, *end;
    nxt_uint_t  value, digits;

    p = source->start;
    end = p + source->length;

    while (end - p >= 4) {

        if (p[0] != '\\') {
            p++;
            continue;
        }

        if (p[1] == 'x') {

            if (p[2] == '{') {
                value = 0;
                digits = 0;

                for (q = p + 3; q < end && nxt_pcre2_hex(*q); q++) {
                    if (value < 0x100) {
                        value = value * 16 + nxt_pcre2_hex_value(*q);
                    }

                    digits++;
                }

                if (digits != 0 && q < end && *q == '}'
                    && value >= 0x80 && value <= 0xff)
                {
                    return p - source->start;
                }

            } else if (nxt_pcre2_hex(p[2]) && nxt_pcre2_hex(p[3])
                       && nxt_pcre2_hex_value(p[2]) >= 8)
            {
                return p - source->start;
            }
        }

        p += 2;
    }

    return source->length;
}


static nxt_bool_t
nxt_pcre2_hex(u_char c)
{
    c |= 0x20;

    return ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}


static nxt_uint_t
nxt_pcre2_hex_value(u_char c)
{
    c |= 0x20;

    return (c <= '9') ? (nxt_uint_t) (c - '0') : (nxt_uint_t) (c - 'a' + 10);
}


static nxt_bool_t
nxt_pcre2_utf(pcre2_code *code)
{
    uint32_t  options;

    return (pcre2_pattern_info(code, PCRE2_INFO_ALLOPTIONS, &options) == 0
            && (options & PCRE2_UTF) != 0);
}


/*
 * Returns 1 if the subject has only ASCII bytes, so it is valid UTF-8.
 * 32 bytes are tested at a time, then 8, then one.
 */

static nxt_bool_t
nxt_pcre2_ascii(const u_char *p, size_t length)
{
    uint64_t  word, w[4];

    while (length >= sizeof(w)) {
        nxt_memcpy(w, p, sizeof(w));

        if (((w[0] | w[1] | w[2] | w[3]) & 0x8080808080808080ULL) != 0) {
            return 0;
        }

        p += sizeof(w);
        length -= sizeof(w);
    }

    while (length >= sizeof(uint64_t)) {
        nxt_memcpy(&word, p, sizeof(uint64_t));

        if ((word & 0x8080808080808080ULL) != 0) {
            return 0;
        }

        p += sizeof(uint64_t);
        length -= sizeof(uint64_t);
    }

    while (length != 0) {
        if ((*p & 0x80) != 0) {
            return 0;
        }

        p++;
        length--;
    }

    return 1;
}


/*
 * Returns PCRE2_UTF if the library that runs has Unicode support, else 0.
 * The result is the same on each call, so a race on the cache is harmless.
 */

static uint32_t
nxt_pcre2_utf_options(void)
{
    uint32_t          unicode;
    static nxt_int_t  utf = -1;

    if (utf < 0) {
        unicode = 0;
        utf = (pcre2_config(PCRE2_CONFIG_UNICODE, &unicode) == 0
               && unicode != 0);
    }

    return utf ? PCRE2_UTF : 0;
}


#if defined(PCRE2_EXTRA_CASELESS_RESTRICT)

/*
 * Returns 1 if the library that runs accepts PCRE2_EXTRA_CASELESS_RESTRICT.
 * PCRE2 before 10.43 fails a compile with it with PCRE2_ERROR_BAD_OPTIONS.
 * The value is stored once, when it is known.  It is the same each time,
 * so two threads that probe at once store the same value.
 */

static nxt_bool_t
nxt_pcre2_caseless_restrict(void)
{
    int                    errcode;
    nxt_bool_t             accepted;
    PCRE2_SIZE             erroffset;
    pcre2_code             *code;
    pcre2_compile_context  *ctx;
    static nxt_int_t       restrict_ = -1;

    if (restrict_ >= 0) {
        return restrict_;
    }

    /* A failed allocation is not cached; the next pattern asks again. */

    ctx = pcre2_compile_context_create(NULL);
    if (ctx == NULL) {
        return 0;
    }

    accepted = 0;

    if (pcre2_set_compile_extra_options(ctx, PCRE2_EXTRA_CASELESS_RESTRICT)
        == 0)
    {
        code = pcre2_compile((PCRE2_SPTR) "a", 1, 0, &errcode, &erroffset,
                             ctx);
        if (code != NULL) {
            accepted = 1;
            pcre2_code_free(code);
        }
    }

    pcre2_compile_context_free(ctx);

    restrict_ = accepted;

    return accepted;
}

#endif


#if (NXT_REGEX_JIT)

nxt_bool_t
nxt_regex_jit_compiled(nxt_regex_t *re)
{
    return re->jit;
}

#endif


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


static void
nxt_pcre2_match_data_cleanup(nxt_task_t *task, void *obj, void *data)
{
    pcre2_match_data_free(obj);
}


static void
nxt_pcre2_code_cleanup(nxt_task_t *task, void *obj, void *data)
{
    nxt_regex_t  *re;

    re = obj;
    pcre2_code_free(re->code);

    if (re->bytes != NULL) {
        pcre2_code_free(re->bytes);
    }
#if (NXT_TESTS)
    if (re->jit) {
        nxt_pcre2_test_stats.freed++;
    }
#endif
}


nxt_regex_match_t *
nxt_regex_match_create(nxt_mp_t *mp, size_t size)
{
    nxt_regex_match_t    *match;
    pcre2_match_context  *ctx;

    match = nxt_mp_get(mp, sizeof(nxt_regex_match_t));
    if (nxt_slow_path(match == NULL)) {
        return NULL;
    }

    /*
     * The backtracking frame vector of a match is the large, subject-driven
     * allocation: from PCRE2 10.41 it comes from the match data's allocator,
     * before 10.41 from the match context's.  The subject can come from a
     * client, so a match may grow the vector up to the heap limit.  "mp" is
     * the router thread's pool: one match object serves the whole thread and
     * "mp" never frees memory, so a vector taken from it, and every larger
     * one a later match reallocates, would be stranded for the thread's life.
     * Both the match data and the match context therefore use the system
     * allocator (a NULL general context): the library frees each outgrown
     * vector in place and keeps only the live one, bounded by the heap limit,
     * which "mp" releases through the cleanups below when it is destroyed.
     */

    match->data = pcre2_match_data_create(size, NULL);
    if (nxt_slow_path(match->data == NULL)) {
        nxt_thread_log_alert("pcre2_match_data_create(%uz) failed", size);
        return NULL;
    }

    if (nxt_slow_path(nxt_mp_cleanup(mp, nxt_pcre2_match_data_cleanup, NULL,
                                     match->data, NULL)
                      != NXT_OK))
    {
        pcre2_match_data_free(match->data);
        return NULL;
    }

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

#if (NXT_TESTS && NXT_REGEX_JIT)
    pcre2_jit_stack_assign(ctx, nxt_pcre2_test_jit_stack, NULL);
#endif

    match->ctx = ctx;

#if (NXT_TESTS)
    nxt_regex_test_match_creates++;
#endif

    return match;
}


nxt_int_t
nxt_regex_match(nxt_regex_t *re, u_char *subject, size_t length,
    nxt_regex_match_t *match)
{
    uint32_t     options;
    nxt_int_t    ret;
    nxt_bool_t   utf_error;
    const char   *limit;
    PCRE2_UCHAR  errptr[ERR_BUF_SIZE];

    match->budget = NXT_REGEX_MATCH_LIMIT;

    /*
     * In UTF mode pcre2_match() checks the whole subject before each match,
     * at about 8 instructions a byte.  An ASCII subject is valid UTF-8, and
     * the test below costs about 0.5 instructions a byte, so the check is
     * skipped for it.
     */

    options = 0;

    if (re->utf && nxt_pcre2_ascii(subject, length)) {
        options = PCRE2_NO_UTF_CHECK;
#if (NXT_TESTS)
        nxt_regex_test_no_utf_check++;
#endif
    }

    ret = nxt_pcre2_match(re->code, subject, length, options, re->jit, match);

    /*
     * The subject is not valid UTF-8.  The check runs before the match, so
     * the callout budget is not spent.
     */

    if (NXT_PCRE2_UTF_ERROR(ret) && re->bytes != NULL) {
        ret = nxt_pcre2_match(re->bytes, subject, length, 0, re->bytes_jit,
                              match);
    }

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
            limit = NXT_PCRE2_UTF_ERROR(ret) ? "UTF-8" : NULL;
            break;
        }

        if (limit != NULL) {

            /*
             * A client can reach a limit on every request.  Warn once per
             * compiled pattern, so a flood of crafted requests cannot flood
             * the log.  The pattern is shared by the router threads, so the
             * flag is set with an atomic compare-and-set: one thread warns.
             */

            utf_error = NXT_PCRE2_UTF_ERROR(ret);

            if (nxt_atomic_cmp_set(utf_error ? &re->warned_utf
                                             : &re->warned_limit, 0, 1))
            {
#if (NXT_TESTS)
                nxt_regex_test_warnings++;
#endif
                if (utf_error) {
                    nxt_thread_log_error(NXT_LOG_WARN,
                                         "pcre2_match() got invalid UTF-8 "
                                         "on %uz bytes using \"%V\", which "
                                         "has no byte-mode twin", length,
                                         &re->pattern);

                } else {
                    nxt_thread_log_error(NXT_LOG_WARN,
                                         "pcre2_match() reached the %s limit "
                                         "on %uz bytes using \"%V\"", limit,
                                         length, &re->pattern);
                }
            }

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


static nxt_int_t
nxt_pcre2_match(pcre2_code *code, u_char *subject, size_t length,
    uint32_t options, nxt_bool_t jit, nxt_regex_match_t *match)
{
    nxt_int_t  ret;

    ret = pcre2_match(code, (PCRE2_SPTR) subject, length, 0, options,
                      match->data, match->ctx);

#if (NXT_REGEX_JIT)
    /*
     * Use PCRE2's bounded default 32 KiB stack on the calling thread.
     * Retry stack exhaustion once in the interpreter with the same limit.
     * Preserve the remaining callout budget across the retry.
     * Never retry MATCHLIMIT: that could double pathological backtracking.
     */
#if (NXT_TESTS)
    if (jit && (nxt_pcre2_test_mode == 3 || nxt_pcre2_test_mode == 5)) {
        if (nxt_pcre2_test_mode == 5) {
            match->budget = 0;
        }
        ret = PCRE2_ERROR_JIT_STACKLIMIT;
    }
#endif
    if (ret == PCRE2_ERROR_JIT_STACKLIMIT) {
#if (NXT_TESTS)
        nxt_pcre2_test_stats.fallback++;
#endif
        ret = pcre2_match(code, (PCRE2_SPTR) subject, length, 0,
                          options | PCRE2_NO_JIT, match->data, match->ctx);
    }
#endif

    return ret;
}
