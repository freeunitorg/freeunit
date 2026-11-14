/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * A pattern with two greedy groups and a class that the subject lacks
 * takes about 2M match steps on 2000 separators on PCRE2 10.4x and PCRE
 * 8.39.  The match must stop at NXT_REGEX_MATCH_LIMIT and report an error.
 *
 * A repeated group takes about 2 match steps per byte.  On PCRE2 10.30 and
 * later it must still match on an 8 KiB subject.  PCRE 1 and PCRE2 before
 * 10.30 also nest about 2 calls per byte on the stack.  On an 8 KiB subject
 * they must stop at the recursion limit and report an error, not overflow
 * the stack.  On a 512-byte subject they must match.
 *
 * The library counts match steps from zero again at each start position of
 * a pattern that is not anchored.  In PCRE2 10.42, "(?=a)(?:a|aa){0,12}[xy]"
 * takes 8,193 steps and 28,659 callouts at each of the first 976 positions
 * of 1000 "a" bytes and an "x".  This is below the match limit at each
 * position, but the whole match takes about 1 s.  The callouts must stop it
 * at NXT_REGEX_MATCH_LIMIT in total, with an error.  The same pattern with
 * "^" has one start position and must not match.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_regex.h>
#include "nxt_tests.h"

#if (NXT_HAVE_PCRE2)
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#endif


#define NXT_REGEX_TEST_SIZE     8192
#define NXT_REGEX_TEST_SHORT    512
#define NXT_REGEX_TEST_STARTS   1000

#if (NXT_HAVE_PCRE2 && (PCRE2_MAJOR > 10 || PCRE2_MINOR >= 30))
#define NXT_REGEX_TEST_LONG_RESULT  1
#else
#define NXT_REGEX_TEST_LONG_RESULT  NXT_ERROR
#endif

/*
 * "(?s).*(a+)+[bc]" on 12 "a" bytes and an invalid byte, 512 times.  With
 * PCRE2_MATCH_INVALID_UTF, the library matched each valid fragment with a
 * new step count, and the match took about 180 ms.  A subject that is not
 * valid UTF-8 now matches the byte-mode twin, which stops at the match limit.
 */
#define NXT_REGEX_TEST_FRAGMENTS  512
#define NXT_REGEX_TEST_FRAGMENT   13

static nxt_regex_t *nxt_regex_test_compile(nxt_thread_t *thr, nxt_mp_t *mp,
    const char *pattern);
static nxt_int_t nxt_regex_reuse_test(nxt_thread_t *thr);
static nxt_int_t nxt_regex_route_test(nxt_thread_t *thr);
static nxt_int_t nxt_regex_test_utf(void);
static nxt_int_t nxt_regex_test_restrict(void);
static nxt_int_t nxt_regex_jit_test(nxt_thread_t *thr);
static nxt_int_t nxt_regex_warn_test(nxt_thread_t *thr);
static nxt_int_t nxt_regex_ascii_test(nxt_thread_t *thr);
static int nxt_regex_ascii_test_child(void *data);
static nxt_int_t nxt_regex_utf_test(nxt_thread_t *thr);
#if (NXT_HAVE_PCRE2)
static nxt_int_t nxt_regex_utf_test_mode(nxt_thread_t *thr, nxt_uint_t mode);
static nxt_int_t nxt_regex_utf_test_set(nxt_thread_t *thr,
    nxt_regex_match_t *match, nxt_regex_t **set, nxt_uint_t n,
    const char *subject, size_t length, nxt_uint_t valid, const char *name);
static nxt_int_t nxt_regex_utf_test_misuse(nxt_thread_t *thr, nxt_mp_t *mp,
    nxt_regex_match_t *match, nxt_regex_t *re);
#endif

/* Compile the pattern.  A failure is logged and returns NULL. */

static nxt_regex_t *
nxt_regex_test_compile(nxt_thread_t *thr, nxt_mp_t *mp, const char *pattern)
{
    nxt_str_t        source;
    nxt_regex_t      *re;
    nxt_regex_err_t  err;

    source.start = (u_char *) pattern;
    source.length = nxt_strlen(pattern);

    re = nxt_regex_compile(mp, &source, &err);
    if (re == NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "regex test: compile of "
                      "\"%s\" failed: %s", pattern, err.msg);
    }

    return re;
}


/*
 * Caseless matching keeps ASCII and non-ASCII characters apart in UTF mode
 * if the library that runs accepts PCRE2_EXTRA_CASELESS_RESTRICT (10.43).
 */

static nxt_int_t
nxt_regex_test_restrict(void)
{
#if (NXT_HAVE_PCRE2 && defined(PCRE2_EXTRA_CASELESS_RESTRICT))
    int                    errcode;
    nxt_int_t              ret;
    PCRE2_SIZE             offset;
    pcre2_code             *code;
    pcre2_compile_context  *ctx;

    if (!nxt_regex_test_utf()) {
        return 0;
    }

    ctx = pcre2_compile_context_create(NULL);
    if (ctx == NULL) {
        return 0;
    }

    ret = 0;

    if (pcre2_set_compile_extra_options(ctx, PCRE2_EXTRA_CASELESS_RESTRICT)
        == 0)
    {
        code = pcre2_compile((PCRE2_SPTR) "a", 1, 0, &errcode, &offset, ctx);
        if (code != NULL) {
            ret = 1;
            pcre2_code_free(code);
        }
    }

    pcre2_compile_context_free(ctx);

    return ret;
#else
    return 0;
#endif
}


/* Patterns match in UTF mode with PCRE2 that has Unicode support. */

static nxt_int_t
nxt_regex_test_utf(void)
{
#if (NXT_HAVE_PCRE2)
    uint32_t  unicode;

    unicode = 0;

    return (pcre2_config(PCRE2_CONFIG_UNICODE, &unicode) == 0
            && unicode != 0);
#else
    return 0;
#endif
}


/*
 * Compile the pattern and match it against the subject.  The match result
 * goes to *result.  The function returns NXT_ERROR if the setup fails.
 */

static nxt_int_t
nxt_regex_test_run(nxt_thread_t *thr, nxt_mp_t *mp, const char *pattern,
    u_char *subject, size_t length, nxt_int_t *result)
{
    nxt_regex_t        *re;
    nxt_regex_match_t  *match;

    re = nxt_regex_test_compile(thr, mp, pattern);
    if (re == NULL) {
        return NXT_ERROR;
    }

    match = nxt_regex_match_create(mp, 1);
    if (match == NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "regex test: match data "
                      "for \"%s\" failed", pattern);
        return NXT_ERROR;
    }

    *result = nxt_regex_match(re, subject, length, match, NULL);

    return NXT_OK;
}


nxt_int_t
nxt_regex_test(nxt_thread_t *thr)
{
    u_char     *subject;
    nxt_mp_t   *mp;
    nxt_int_t  ret, slow, deep, shallow, starts, anchored, utf, utf_char,
               utf_invalid, utf_pattern, utf_escape, utf_escape_byte,
               utf_negative, utf_lookbehind, utf_dotstar, utf_caseless,
               utf_only, utf_fragments, utf_unanchored, utf_word,
               utf_word_valid, utf_block, utf_caseless_ascii,
               utf_caseless_cyrillic, utf_kelvin, restrict_;
    size_t     i;

    nxt_thread_time_update(thr);

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    subject = nxt_mp_alloc(mp, NXT_REGEX_TEST_SIZE);
    if (subject == NULL) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    subject[0] = '/';
    nxt_memset(subject + 1, '/', 2000);

    ret = nxt_regex_test_run(thr, mp, "^/(.*)/(.*)[xy]$", subject, 2001,
                             &slow);

    if (ret == NXT_OK) {
        nxt_memset(subject + 1, 'a', NXT_REGEX_TEST_SIZE - 1);

        ret = nxt_regex_test_run(thr, mp, "^/(a|b)+$", subject,
                                 NXT_REGEX_TEST_SIZE, &deep);
    }

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "^/(a|b)+$", subject,
                                 NXT_REGEX_TEST_SHORT, &shallow);
    }

    if (ret == NXT_OK) {
        nxt_memset(subject, 'a', NXT_REGEX_TEST_STARTS);
        subject[NXT_REGEX_TEST_STARTS] = 'x';

        ret = nxt_regex_test_run(thr, mp, "(?=a)(?:a|aa){0,12}[xy]", subject,
                                 NXT_REGEX_TEST_STARTS + 1, &starts);
    }

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "^(?=a)(?:a|aa){0,12}[xy]",
                                 subject, NXT_REGEX_TEST_STARTS + 1,
                                 &anchored);
    }

    /* "\xd0\xbf" is U+043F in UTF-8, "\xff" is never valid UTF-8. */

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "^/.$", (u_char *) "/\xd0\xbf", 3,
                                 &utf_char);
    }

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "^/[^/]+$", (u_char *) "/\xff", 2,
                                 &utf_invalid);
    }

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "^/\xff$", (u_char *) "/\xff", 2,
                                 &utf_pattern);
    }

    /* In UTF mode "\xff" in a pattern is U+00FF, the bytes C3 BF. */

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "^\\xff$", (u_char *) "\xc3\xbf", 2,
                                 &utf_escape);
    }

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "^\\xff$", (u_char *) "\xff", 1,
                                 &utf_escape_byte);
    }

    /*
     * ".*" does not go past an invalid byte in UTF mode.  So a negative
     * route pattern such as "!~^/admin/.*$" gives the opposite result.
     */

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "^/admin/.*$",
                                 (u_char *) "/admin/\xff", 8, &utf_negative);
    }

    /*
     * The JIT of PCRE2 before 10.48 with PCRE2_MATCH_INVALID_UTF gives no
     * match here, and the interpreter a match.
     */

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "(?<=a)", (u_char *) "a\x80", 2,
                                 &utf_lookbehind);
    }

    /*
     * The interpreter and the JIT gave different results here with
     * PCRE2_MATCH_INVALID_UTF.
     */

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "(?s).*b", (u_char *) "a\xff" "b", 3,
                                 &utf_dotstar);
    }

    /*
     * An unanchored pattern is compiled again with PCRE2_AUTO_CALLOUT.  The
     * second compile must keep UTF mode.
     */

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "\\xff", (u_char *) "\xc3\xbf", 2,
                                 &utf_unanchored);
    }

    /*
     * An ASCII subject skips the UTF-8 check, tested 8 bytes at a time.  An
     * invalid byte in the last byte of a word or in the tail must still go
     * to the byte-mode twin, where "\xff" is the byte FF.
     */

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "\\xff",
                                 (u_char *) "/admin/\xff" "aaaaaaaa", 16,
                                 &utf_word);
    }

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "\\xff",
                                 (u_char *) "/admin/aaaaaaaaaaaaaaaaaaaaaaaa"
                                            "\xff" "aaaaaaaa", 40,
                                 &utf_block);
    }

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "^/admin/a.$",
                                 (u_char *) "/admin/a\xd0\xbf", 10,
                                 &utf_word_valid);
    }

    /*
     * In UTF mode "s" also matches U+017F caselessly, unless
     * PCRE2_EXTRA_CASELESS_RESTRICT keeps ASCII and non-ASCII apart.  ASCII
     * and non-ASCII letters still match their own other case.
     */

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "(?i)^/admins$",
                                 (u_char *) "/ADMINS", 7, &utf_caseless_ascii);
    }

    /* U+212A KELVIN SIGN, with "k" as for U+017F with "s". */

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "(?i)^/k$",
                                 (u_char *) "/\xe2\x84\xaa", 4, &utf_kelvin);
    }

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "(?i)^/\xd0\xbf$",
                                 (u_char *) "/\xd0\x9f", 3,
                                 &utf_caseless_cyrillic);
    }

    if (ret == NXT_OK) {
        ret = nxt_regex_test_run(thr, mp, "(?i)^/admins$",
                                 (u_char *) "/admin\xc5\xbf", 8,
                                 &utf_caseless);
    }

    /* "\x{43f}" is valid only in UTF mode, so the pattern has no twin. */

    utf_only = NXT_ERROR;

    if (ret == NXT_OK && nxt_regex_test_utf()) {
        ret = nxt_regex_test_run(thr, mp, "^/\\x{43f}$", (u_char *) "/\xff",
                                 2, &utf_only);
    }

    if (ret == NXT_OK) {
        subject = nxt_mp_alloc(mp, NXT_REGEX_TEST_FRAGMENTS
                                   * NXT_REGEX_TEST_FRAGMENT);
        if (subject == NULL) {
            ret = NXT_ERROR;

        } else {
            for (i = 0; i < NXT_REGEX_TEST_FRAGMENTS * NXT_REGEX_TEST_FRAGMENT;
                 i++)
            {
                subject[i] = (i % NXT_REGEX_TEST_FRAGMENT
                              == NXT_REGEX_TEST_FRAGMENT - 1) ? 0xff : 'a';
            }

            ret = nxt_regex_test_run(thr, mp, "(?s).*(a+)+[bc]", subject,
                                     NXT_REGEX_TEST_FRAGMENTS
                                     * NXT_REGEX_TEST_FRAGMENT,
                                     &utf_fragments);
        }
    }

    nxt_mp_destroy(mp);

    if (ret != NXT_OK) {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(thr->log, slow == NXT_ERROR, "regex test: backtracking "
                   "pattern returned %i, expected %i", slow,
                   (nxt_int_t) NXT_ERROR);

    NXT_TEST_CHECK(thr->log, deep == NXT_REGEX_TEST_LONG_RESULT, "regex test: "
                   "repeated group on %d bytes returned %i, expected %i",
                   NXT_REGEX_TEST_SIZE, deep,
                   (nxt_int_t) NXT_REGEX_TEST_LONG_RESULT);

    NXT_TEST_CHECK(thr->log, shallow == 1, "regex test: repeated group "
                   "on %d bytes returned %i, expected 1",
                   NXT_REGEX_TEST_SHORT, shallow);

    NXT_TEST_CHECK(thr->log, starts == NXT_ERROR, "regex test: pattern "
                   "without an anchor on %d bytes returned %i, expected %i",
                   NXT_REGEX_TEST_STARTS + 1, starts, (nxt_int_t) NXT_ERROR);

    NXT_TEST_CHECK(thr->log, anchored == 0, "regex test: anchored pattern "
                   "on %d bytes returned %i, expected 0",
                   NXT_REGEX_TEST_STARTS + 1, anchored);

    utf = nxt_regex_test_utf();

    NXT_TEST_CHECK(thr->log, utf_char == utf, "regex test: \".\" on a "
                   "2-byte UTF-8 character returned %i, expected %i",
                   utf_char, utf);

    /*
     * A subject with invalid UTF-8 matches the byte-mode twin, so it gets
     * the result of byte mode, and no error.
     */

    NXT_TEST_CHECK(thr->log, utf_invalid == 1, "regex test: subject "
                   "with invalid UTF-8 returned %i, expected 1", utf_invalid);

    NXT_TEST_CHECK(thr->log, utf_pattern == 1, "regex test: pattern with "
                   "invalid UTF-8 returned %i, expected 1", utf_pattern);

    NXT_TEST_CHECK(thr->log, utf_escape == utf, "regex test: \"\\xff\" on "
                   "U+00FF returned %i, expected %i", utf_escape, utf);

    NXT_TEST_CHECK(thr->log, utf_escape_byte == 1, "regex test: "
                   "\"\\xff\" on the byte FF returned %i, expected 1",
                   utf_escape_byte);

    /* So a client cannot make a deny rule such as "~^/admin/.*$" miss. */

    NXT_TEST_CHECK(thr->log, utf_negative == 1, "regex test: \".*\" on "
                   "an invalid byte returned %i, expected 1", utf_negative);

    NXT_TEST_CHECK(thr->log, utf_lookbehind == 1, "regex test: lookbehind "
                   "before an invalid byte returned %i, expected 1",
                   utf_lookbehind);

    NXT_TEST_CHECK(thr->log, utf_unanchored == utf, "regex test: "
                   "unanchored \"\\xff\" on U+00FF returned %i, expected %i",
                   utf_unanchored, utf);

    NXT_TEST_CHECK(thr->log, utf_word == 1, "regex test: an invalid byte "
                   "in an 8-byte word returned %i, expected 1", utf_word);

    NXT_TEST_CHECK(thr->log, utf_block == 1, "regex test: an invalid byte "
                   "in a 32-byte block returned %i, expected 1", utf_block);

    NXT_TEST_CHECK(thr->log, utf_word_valid == utf, "regex test: a UTF-8 "
                   "character after 8 ASCII bytes returned %i, expected %i",
                   utf_word_valid, utf);

    NXT_TEST_CHECK(thr->log, utf_dotstar == 1, "regex test: \"(?s).*b\" "
                   "on an invalid byte returned %i, expected 1", utf_dotstar);

    restrict_ = nxt_regex_test_restrict();

    NXT_TEST_CHECK(thr->log, utf_caseless == (utf && !restrict_), "regex "
                   "test: caseless \"s\" on U+017F returned %i, expected %i",
                   utf_caseless, (nxt_int_t) (utf && !restrict_));

    NXT_TEST_CHECK(thr->log, utf_kelvin == (utf && !restrict_), "regex test: "
                   "caseless \"k\" on U+212A returned %i, expected %i",
                   utf_kelvin, (nxt_int_t) (utf && !restrict_));

    NXT_TEST_CHECK(thr->log, utf_caseless_ascii == 1, "regex test: caseless "
                   "\"admins\" on \"ADMINS\" returned %i, expected 1",
                   utf_caseless_ascii);

    NXT_TEST_CHECK(thr->log, utf_caseless_cyrillic == utf, "regex test: "
                   "caseless U+043F on U+041F returned %i, expected %i",
                   utf_caseless_cyrillic, utf);

    NXT_TEST_CHECK(thr->log, utf_only == NXT_ERROR, "regex test: a UTF-only "
                   "pattern on invalid UTF-8 returned %i, expected %i",
                   utf_only, (nxt_int_t) NXT_ERROR);

    NXT_TEST_CHECK(thr->log, utf_fragments == NXT_ERROR, "regex test: %d "
                   "invalid UTF-8 fragments returned %i, expected %i",
                   NXT_REGEX_TEST_FRAGMENTS, utf_fragments,
                   (nxt_int_t) NXT_ERROR);

    ret = nxt_regex_warn_test(thr);
    if (ret != NXT_OK) {
        return ret;
    }

    ret = nxt_regex_ascii_test(thr);
    if (ret != NXT_OK) {
        return ret;
    }

    ret = nxt_regex_utf_test(thr);
    if (ret != NXT_OK) {
        return ret;
    }

    ret = nxt_regex_reuse_test(thr);
    if (ret != NXT_OK) {
        return ret;
    }

    ret = nxt_regex_route_test(thr);
    if (ret != NXT_OK) {
        return ret;
    }

    ret = nxt_regex_jit_test(thr);
    if (ret != NXT_OK) {
        return ret;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "regex test passed");

    return NXT_OK;
}


/*
 * One match object serves every route pattern of a router thread, across
 * requests.  So it must survive a limit error and still match correctly on
 * the next request: nxt_regex_match() resets the per-match budget, and the
 * reused match data and context hold no state from the stopped match.  Reuse
 * one object: a match that reaches the limit returns an error, and the same
 * object then matches and rejects correctly with a normal pattern.  On the
 * old per-request object this could not regress; the reused object can.
 * nxt_regex_route_test() pins that the route code reuses one object.
 */

static nxt_int_t
nxt_regex_reuse_test(nxt_thread_t *thr)
{
    u_char             *subject;
    nxt_mp_t           *mp;
    nxt_int_t          limited, matched, missed;
    nxt_regex_t        *slow, *simple;
    nxt_regex_match_t  *match;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    match = nxt_regex_match_create(mp, 1);
    subject = (match != NULL) ? nxt_mp_alloc(mp, NXT_REGEX_TEST_SIZE) : NULL;

    slow = (subject != NULL)
           ? nxt_regex_test_compile(thr, mp, "^/(.*)/(.*)[xy]$") : NULL;

    simple = (slow != NULL)
             ? nxt_regex_test_compile(thr, mp, "^/(foo|bar)$") : NULL;

    if (simple == NULL) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    /* A long subject with no "x" or "y" makes the first pattern backtrack. */
    subject[0] = '/';
    nxt_memset(subject + 1, '/', 2000);

    limited = nxt_regex_match(slow, subject, 2001, match, NULL);
    matched = nxt_regex_match(simple, (u_char *) "/foo", 4, match, NULL);
    missed = nxt_regex_match(simple, (u_char *) "/baz", 4, match, NULL);

    nxt_mp_destroy(mp);

    NXT_TEST_CHECK(thr->log, limited == NXT_ERROR, "regex reuse test: limit "
                   "match returned %i, expected %i", limited,
                   (nxt_int_t) NXT_ERROR);

    NXT_TEST_CHECK(thr->log, matched == 1, "regex reuse test: match after a "
                   "limit returned %i, expected 1", matched);

    NXT_TEST_CHECK(thr->log, missed == 0, "regex reuse test: non-match after "
                   "a limit returned %i, expected 0", missed);

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "regex reuse test passed: limit error, then reuse");

    return NXT_OK;
}


/*
 * nxt_http_route_pattern() matches a route regex with the match object of
 * the router thread, not of the request: the requests of one thread share
 * one object, created once from the engine pool, and a request pool gets
 * nothing from a match, not even from one that reaches a limit.  Match two
 * requests on one engine: the first reaches the limit, the second still
 * matches and rejects correctly, one object was created, and both request
 * pools are still empty.  On the old code each request created its own
 * object from its pool: two objects, and request pools that are not empty.
 */

static nxt_int_t
nxt_regex_route_test(nxt_thread_t *thr)
{
    u_char              *subject;
    nxt_mp_t            *mp;
    nxt_int_t           limited, matched, missed;
    nxt_bool_t          empty;
    nxt_uint_t          creates;
    nxt_regex_t         *slow, *simple;
    nxt_http_request_t  r1, r2;
    nxt_event_engine_t  engine, *saved;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    nxt_memzero(&r1, sizeof(nxt_http_request_t));
    nxt_memzero(&r2, sizeof(nxt_http_request_t));

    r1.mem_pool = nxt_mp_create(1024, 128, 256, 32);
    r2.mem_pool = nxt_mp_create(1024, 128, 256, 32);

    subject = (r1.mem_pool != NULL && r2.mem_pool != NULL)
              ? nxt_mp_alloc(mp, NXT_REGEX_TEST_SIZE) : NULL;

    slow = (subject != NULL)
           ? nxt_regex_test_compile(thr, mp, "^/(.*)/(.*)[xy]$") : NULL;

    simple = (slow != NULL)
             ? nxt_regex_test_compile(thr, mp, "^/(foo|bar)$") : NULL;

    if (simple == NULL) {
        goto fail;
    }

    subject[0] = '/';
    nxt_memset(subject + 1, '/', 2000);

    /* The matcher uses the pool and the match object of the engine only. */
    nxt_memzero(&engine, sizeof(nxt_event_engine_t));
    engine.mem_pool = mp;

    saved = thr->engine;
    thr->engine = &engine;

    creates = nxt_regex_test_match_creates;

    limited = nxt_http_route_test_regex(&r1, slow, subject, 2001);
    matched = nxt_http_route_test_regex(&r2, simple, (u_char *) "/foo", 4);
    missed = nxt_http_route_test_regex(&r2, simple, (u_char *) "/baz", 4);

    creates = nxt_regex_test_match_creates - creates;
    empty = nxt_mp_is_empty(r1.mem_pool) && nxt_mp_is_empty(r2.mem_pool);

    thr->engine = saved;

    nxt_mp_destroy(r2.mem_pool);
    nxt_mp_destroy(r1.mem_pool);
    nxt_mp_destroy(mp);

    NXT_TEST_CHECK(thr->log, limited == NXT_ERROR, "regex route test: limit "
                   "match returned %i, expected %i", limited,
                   (nxt_int_t) NXT_ERROR);

    NXT_TEST_CHECK(thr->log, matched == 1, "regex route test: match on the "
                   "next request returned %i, expected 1", matched);

    NXT_TEST_CHECK(thr->log, missed == 0, "regex route test: non-match on "
                   "the next request returned %i, expected 0", missed);

    NXT_TEST_CHECK(thr->log, creates == 1, "regex route test: two requests "
                   "created %ui match objects, expected 1", creates);

    NXT_TEST_CHECK(thr->log, empty, "regex route test: a request pool holds "
                   "memory of the match");

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "regex route test passed: one match object per thread");

    return NXT_OK;

fail:

    if (r2.mem_pool != NULL) {
        nxt_mp_destroy(r2.mem_pool);
    }

    if (r1.mem_pool != NULL) {
        nxt_mp_destroy(r1.mem_pool);
    }

    nxt_mp_destroy(mp);

    return NXT_ERROR;
}


#if (NXT_HAVE_PCRE2 && NXT_REGEX_JIT && NXT_LINUX)

/*
 * The JIT allocator maps executable memory with mmap(), so a missing
 * pcre2_code_free() is invisible to the pool and to LeakSanitizer.  Sum
 * the anonymous executable mappings of the process instead.
 */

static size_t
nxt_regex_jit_test_exec_bytes(void)
{
    int            n;
    FILE           *f;
    char           line[512], perms[8];
    size_t         total;
    unsigned long  start, end, inode;

    f = fopen("/proc/self/maps", "r");
    if (f == NULL) {
        return 0;
    }

    total = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        n = 0;

        if (sscanf(line, "%lx-%lx %7s %*s %*s %lu %n",
                   &start, &end, perms, &inode, &n) < 4)
        {
            continue;
        }

        if (perms[2] == 'x' && inode == 0
            && (line[n] == '\0' || line[n] == '\n'))
        {
            total += end - start;
        }
    }

    (void) fclose(f);

    return total;
}

#endif

static nxt_int_t
nxt_regex_jit_test(nxt_thread_t *thr)
{
#if (NXT_HAVE_PCRE2 && NXT_REGEX_JIT)
    nxt_mp_t                    *mp;
    nxt_int_t                   ret, result;
    nxt_uint_t                  mode, i;
    nxt_str_t                   source;
    nxt_regex_t                 *re;
    nxt_regex_err_t             err;
    nxt_regex_jit_test_stats_t   before, after;
#if (NXT_LINUX)
    size_t                      exec_before, exec_after;
#endif

    if (!nxt_regex_jit_test_available()) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "regex JIT test: JIT unavailable, interpreter used");
        return NXT_OK;
    }

    /* Modes cover execution, no-JIT libraries, compile failure and retry. */
    for (mode = 0; mode < 4; mode++) {
        before = nxt_regex_jit_test_stats();
        nxt_regex_jit_test_mode(mode);

        for (i = 0; i < 64; i++) {
            mp = nxt_mp_create(1024, 128, 256, 32);
            if (mp == NULL) {
                return NXT_ERROR;
            }

            ret = nxt_regex_test_run(thr, mp, "^/(foo|bar)$",
                                     (u_char *) "/foo", 4, &result);
            if (ret == NXT_OK && result == 1) {
                ret = nxt_regex_test_run(thr, mp, "^/(foo|bar)$",
                                         (u_char *) "/baz", 4, &result);
                if (result != 0) {
                    ret = NXT_ERROR;
                }
            } else {
                ret = NXT_ERROR;
            }

            /* A failed validation still releases preceding JIT patterns. */
            source.start = (u_char *) "(";
            source.length = 1;
            re = nxt_regex_compile(mp, &source, &err);
            nxt_mp_destroy(mp);

            NXT_TEST_CHECK(thr->log, ret == NXT_OK && re == NULL,
                           "regex JIT test: mode %ui matching/validation", mode);
        }

        after = nxt_regex_jit_test_stats();
        if (mode == 0 || mode == 3) {
            NXT_TEST_CHECK(thr->log, after.compiled - before.compiled == 128,
                           "regex JIT test: compiled %ui, expected 128",
                           after.compiled - before.compiled);
            NXT_TEST_CHECK(thr->log, after.executed - before.executed == 128,
                           "regex JIT test: executed %ui, expected 128",
                           after.executed - before.executed);
            NXT_TEST_CHECK(thr->log, after.freed - before.freed == 128,
                           "regex JIT test: freed %ui, expected 128",
                           after.freed - before.freed);
        } else {
            NXT_TEST_CHECK(thr->log, after.compiled == before.compiled
                           && after.executed == before.executed,
                           "regex JIT test: fallback mode %ui used JIT", mode);
        }

        NXT_TEST_CHECK(thr->log,
                       after.fallback - before.fallback == (mode == 3 ? 128 : 0),
                       "regex JIT test: retry count %ui in mode %ui",
                       after.fallback - before.fallback, mode);
    }

#if (NXT_LINUX)
    /*
     * Each compile/destroy cycle must give its executable memory back.
     * A missing pcre2_code_free() leaks about 1 kB of mappings per
     * pattern here, 1 MB over the loop; the allocator itself keeps a
     * free chunk or two, so allow 256 kB of noise.
     */
    nxt_regex_jit_test_mode(0);
    exec_before = nxt_regex_jit_test_exec_bytes();

    for (i = 0; i < 1024; i++) {
        mp = nxt_mp_create(1024, 128, 256, 32);
        if (mp == NULL) {
            return NXT_ERROR;
        }

        ret = nxt_regex_test_run(thr, mp, "^/(foo|bar)$",
                                 (u_char *) "/foo", 4, &result);
        nxt_mp_destroy(mp);

        if (ret != NXT_OK || result != 1) {
            break;
        }
    }

    exec_after = nxt_regex_jit_test_exec_bytes();

    NXT_TEST_CHECK(thr->log, i == 1024 && exec_after <= exec_before + 262144,
                   "regex JIT test: executable mappings grew from %uz to "
                   "%uz bytes over %ui compile/destroy cycles",
                   exec_before, exec_after, i);
#endif

    /* A stack retry must not replenish an unanchored match's budget. */
    nxt_regex_jit_test_mode(5);
    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }
    result = 0;
    ret = nxt_regex_test_run(thr, mp, "foo", (u_char *) "/foo", 4, &result);
    nxt_mp_destroy(mp);
    NXT_TEST_CHECK(thr->log, ret == NXT_OK && result == NXT_ERROR,
                   "regex JIT test: exhausted retry budget returned %i, "
                   "expected %i", result, (nxt_int_t) NXT_ERROR);

    /* Registering cleanup is mandatory, even before a JIT attempt. */
    nxt_regex_jit_test_mode(4);
    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }
    source.start = (u_char *) "^/foo$";
    source.length = 6;
    re = nxt_regex_compile(mp, &source, &err);
    nxt_mp_destroy(mp);
    nxt_regex_jit_test_mode(0);
    NXT_TEST_CHECK(thr->log, re == NULL,
                   "regex JIT test: cleanup failure accepted a pattern");

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "regex JIT test passed: execution, fallback, cleanup");
#endif

    return NXT_OK;
}


/*
 * A pattern warns once for a limit and once for invalid UTF-8, however many
 * requests reach each.  One warning must not hide the other.
 */

static nxt_int_t
nxt_regex_warn_test(nxt_thread_t *thr)
{
    u_char             subject[32];
    nxt_mp_t           *mp;
    nxt_int_t          limit1, limit2, utf1, utf2;
    nxt_uint_t         warnings;
    nxt_regex_t        *re;
    nxt_regex_match_t  *match;

    if (!nxt_regex_test_utf()) {
        return NXT_OK;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    match = nxt_regex_match_create(mp, 1);

    re = (match != NULL)
         ? nxt_regex_test_compile(thr, mp, "^(?:\\x{43f}|(a+)+[bc])") : NULL;

    if (re == NULL) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    nxt_memset(subject, 'a', sizeof(subject));

    warnings = nxt_regex_test_warnings;

    limit1 = nxt_regex_match(re, subject, sizeof(subject), match, NULL);
    limit2 = nxt_regex_match(re, subject, sizeof(subject), match, NULL);
    utf1 = nxt_regex_match(re, (u_char *) "\xff", 1, match, NULL);
    utf2 = nxt_regex_match(re, (u_char *) "\xff", 1, match, NULL);

    warnings = nxt_regex_test_warnings - warnings;

    nxt_mp_destroy(mp);

    NXT_TEST_CHECK(thr->log, limit1 == NXT_ERROR && limit2 == NXT_ERROR,
                   "regex warn test: limit returned %i and %i, expected %i",
                   limit1, limit2, (nxt_int_t) NXT_ERROR);

    NXT_TEST_CHECK(thr->log, utf1 == NXT_ERROR && utf2 == NXT_ERROR,
                   "regex warn test: invalid UTF-8 returned %i and %i, "
                   "expected %i", utf1, utf2, (nxt_int_t) NXT_ERROR);

    NXT_TEST_CHECK(thr->log, warnings == 2, "regex warn test: %ui warnings, "
                   "expected 2", warnings);

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "regex warn test passed: one warning per kind");

    return NXT_OK;
}


/*
 * An ASCII subject skips the UTF-8 check of a UTF pattern, and only then:
 * not for a subject with a byte from 0x80, and not for a pattern that
 * matches bytes.  The ASCII test must read no byte after the subject: each
 * subject ends at a page with no access, in a child process.
 */

static nxt_int_t
nxt_regex_ascii_test(nxt_thread_t *thr)
{
    int                rc;
    nxt_mp_t           *mp;
    nxt_uint_t         checks;
    nxt_regex_t        *utf, *bytes;
    nxt_regex_match_t  *match;

    if (!nxt_regex_test_utf()) {
        return NXT_OK;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    match = nxt_regex_match_create(mp, 1);
    utf = (match != NULL) ? nxt_regex_test_compile(thr, mp, "^/a") : NULL;

    /* A raw FF byte is not valid in UTF mode, so this pattern matches bytes. */
    bytes = (utf != NULL) ? nxt_regex_test_compile(thr, mp, "^/a\xff$")
                          : NULL;

    if (bytes == NULL) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    checks = nxt_regex_test_no_utf_check;
    (void) nxt_regex_match(utf, (u_char *) "/abc", 4, match, NULL);
    checks = nxt_regex_test_no_utf_check - checks;

    NXT_TEST_CHECK(thr->log, checks == 1, "regex ASCII test: an ASCII "
                   "subject skipped the UTF-8 check %ui times, expected 1",
                   checks);

    checks = nxt_regex_test_no_utf_check;
    (void) nxt_regex_match(utf, (u_char *) "/a\xd0\xbf", 4, match, NULL);
    (void) nxt_regex_match(bytes, (u_char *) "/abc", 4, match, NULL);
    checks = nxt_regex_test_no_utf_check - checks;

    nxt_mp_destroy(mp);

    NXT_TEST_CHECK(thr->log, checks == 0, "regex ASCII test: a UTF-8 "
                   "subject or a byte-mode pattern skipped the check %ui "
                   "times, expected 0", checks);

    rc = nxt_test_in_child(thr, "regex ASCII test", nxt_regex_ascii_test_child,
                           NULL);

    NXT_TEST_CHECK(thr->log, rc == 0, "regex ASCII test: a subject at the "
                   "end of a page failed (child status %d)", rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "regex ASCII test passed: fast path, page end");

    return NXT_OK;
}


static int
nxt_regex_ascii_test_child(void *data)
{
    u_char             *page, *end;
    size_t             size, length;
    nxt_mp_t           *mp;
    nxt_regex_t        *re;
    nxt_regex_err_t    rerr;
    nxt_regex_match_t  *match;

    static nxt_str_t  pattern = nxt_string("^/admin/.*$");

    size = getpagesize();

    page = mmap(NULL, 2 * size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        return 1;
    }

    if (mprotect(page + size, size, PROT_NONE) != 0) {
        return 2;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    match = (mp != NULL) ? nxt_regex_match_create(mp, 1) : NULL;
    re = (match != NULL) ? nxt_regex_compile(mp, &pattern, &rerr) : NULL;

    if (re == NULL) {
        return 3;
    }

    end = page + size;

    for (length = 0; length <= 33; length++) {
        nxt_memset(end - length, 'a', length);
        (void) nxt_regex_match(re, end - length, length, match, NULL);

        if (length != 0) {
            end[-1] = 0xff;
            (void) nxt_regex_match(re, end - length, length, match, NULL);
        }
    }

    return 0;
}


/*
 * A caller can keep the UTF-8 check result of a subject in nxt_regex_utf_t
 * and give it to each match of the subject.  For each subject and each set
 * of two or three patterns: the results are the same as with no kept
 * result, PCRE2 checks the subject once (an ASCII subject not at all), and
 * the kept result says valid or invalid.  The sets cover patterns with and
 * without a byte-mode twin, and a pattern without a twin first.  This runs
 * with the interpreter and, if the build and the library have it, with the
 * JIT.
 */

typedef struct {
    const char  *name;
    const char  *subject;
    size_t      length;
    nxt_uint_t  valid;  /* 2: ASCII, 1: valid UTF-8, 0: invalid. */
} nxt_regex_utf_test_case_t;


#define nxt_regex_utf_case(name, subject, valid)                              \
    { name, subject, sizeof(subject) - 1, valid }

static const nxt_regex_utf_test_case_t  nxt_regex_utf_test_cases[] = {
    nxt_regex_utf_case("ASCII", "/a", 2),
    nxt_regex_utf_case("empty", "", 2),
    nxt_regex_utf_case("2-byte", "/\xd0\xbf", 1),
    nxt_regex_utf_case("3-byte", "/\xe2\x82\xac", 1),
    nxt_regex_utf_case("4-byte", "/\xf0\x9f\x98\x80", 1),
    nxt_regex_utf_case("U+10FFFF", "/\xf4\x8f\xbf\xbf", 1),
    nxt_regex_utf_case("first byte 0x80", "\x80/a", 0),
    nxt_regex_utf_case("lone 0x80", "/a\x80", 0),
    nxt_regex_utf_case("truncated", "/a\xd0", 0),
    nxt_regex_utf_case("overlong", "/\xc0\xaf", 0),
    nxt_regex_utf_case("surrogate", "/\xed\xa0\x80", 0),
    nxt_regex_utf_case("above U+10FFFF", "/\xf4\x90\x80\x80", 0),
    nxt_regex_utf_case("5-byte form", "/\xf8\x88\x80\x80\x80", 0),
    nxt_regex_utf_case("byte 0xFF", "/\xff", 0),
};


/*
 * "^/.$" and "^/..$" give a different result in UTF mode and in byte mode
 * on a 2-byte character.  "\x{43f}" is valid only in UTF mode, so the
 * pattern that has it has no byte-mode twin.
 */

static const char  *nxt_regex_utf_test_patterns[] = {
    "^/.$",
    "^/..$",
    "z",
    "^/(?:\\x{43f}|a)",
};

#define NXT_REGEX_UTF_TEST_PATTERNS  4
#define NXT_REGEX_UTF_TEST_NOTWIN    3


static nxt_int_t
nxt_regex_utf_test(nxt_thread_t *thr)
{
#if (NXT_HAVE_PCRE2)
    nxt_int_t  ret;

    if (!nxt_regex_test_utf()) {
        return NXT_OK;
    }

    /* Mode 1 compiles without the JIT. */

    ret = nxt_regex_utf_test_mode(thr, 1);

    nxt_regex_jit_test_mode(0);

    if (ret != NXT_OK) {
        return ret;
    }

#if (NXT_REGEX_JIT)
    if (nxt_regex_jit_test_available()) {
        ret = nxt_regex_utf_test_mode(thr, 0);
        if (ret != NXT_OK) {
            return ret;
        }
    }
#endif

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "regex UTF-8 result test passed: one check per subject");
#endif

    return NXT_OK;
}


#if (NXT_HAVE_PCRE2)

static nxt_int_t
nxt_regex_utf_test_mode(nxt_thread_t *thr, nxt_uint_t mode)
{
    size_t                           i;
    nxt_mp_t                         *mp;
    nxt_int_t                        ret;
    nxt_uint_t                       n, k;
    nxt_regex_t                      *re[NXT_REGEX_UTF_TEST_PATTERNS];
    nxt_regex_t                      *set[3];
    nxt_regex_match_t                *match;
    const nxt_regex_utf_test_case_t  *tc;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    nxt_regex_jit_test_mode(mode);

    match = nxt_regex_match_create(mp, 1);
    ret = (match != NULL) ? NXT_OK : NXT_ERROR;

    for (k = 0; ret == NXT_OK && k < NXT_REGEX_UTF_TEST_PATTERNS; k++) {
        re[k] = nxt_regex_test_compile(thr, mp, nxt_regex_utf_test_patterns[k]);
        if (re[k] == NULL) {
            ret = NXT_ERROR;
        }
    }

    if (ret != NXT_OK) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

#if (NXT_REGEX_JIT)
    for (k = 0; k < NXT_REGEX_UTF_TEST_PATTERNS; k++) {
        if (nxt_regex_jit_compiled(re[k]) != (mode == 0)) {
            nxt_mp_destroy(mp);
            NXT_TEST_CHECK(thr->log, 0, "regex UTF-8 result test: pattern "
                           "%ui is %s JIT code in mode %ui", k,
                           (mode == 0) ? "not" : "still", mode);
        }
    }
#endif

    for (i = 0; i < nxt_nitems(nxt_regex_utf_test_cases); i++) {
        tc = &nxt_regex_utf_test_cases[i];

        for (n = 2; n <= 3; n++) {

            /* Three patterns with a twin. */

            set[0] = re[0];
            set[1] = re[1];
            set[2] = re[2];

            ret = nxt_regex_utf_test_set(thr, match, set, n, tc->subject,
                                         tc->length, tc->valid, tc->name);
            if (ret != NXT_OK) {
                goto fail;
            }

            /* A pattern without a twin after one with a twin. */

            set[1] = re[NXT_REGEX_UTF_TEST_NOTWIN];

            ret = nxt_regex_utf_test_set(thr, match, set, n, tc->subject,
                                         tc->length, tc->valid, tc->name);
            if (ret != NXT_OK) {
                goto fail;
            }

            /* A pattern without a twin first. */

            set[0] = re[NXT_REGEX_UTF_TEST_NOTWIN];
            set[1] = re[0];

            ret = nxt_regex_utf_test_set(thr, match, set, n, tc->subject,
                                         tc->length, tc->valid, tc->name);
            if (ret != NXT_OK) {
                goto fail;
            }
        }
    }

    ret = nxt_regex_utf_test_misuse(thr, mp, match, re[0]);

fail:

    nxt_mp_destroy(mp);

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "regex UTF-8 result test: "
                      "%s passed", (mode == 0) ? "JIT" : "interpreter");
    }

    return ret;
}


/*
 * Match the subject with each pattern of the set twice: with no kept
 * result, and with one result kept across the set.
 */

static nxt_int_t
nxt_regex_utf_test_set(nxt_thread_t *thr, nxt_regex_match_t *match,
    nxt_regex_t **set, nxt_uint_t n, const char *subject, size_t length,
    nxt_uint_t valid, const char *name)
{
    u_char                 *p;
    nxt_int_t              expected[3], got[3];
    nxt_uint_t             k, checks, expected_checks;
    nxt_regex_utf_t        utf;
    nxt_regex_utf_state_t  expected_state;

    p = (u_char *) subject;

    for (k = 0; k < n; k++) {
        expected[k] = nxt_regex_match(set[k], p, length, match, NULL);
    }

    nxt_regex_utf_init(&utf);

    checks = nxt_regex_test_utf_checks;

    for (k = 0; k < n; k++) {
        got[k] = nxt_regex_match(set[k], p, length, match, &utf);
    }

    checks = nxt_regex_test_utf_checks - checks;

    for (k = 0; k < n; k++) {
        NXT_TEST_CHECK(thr->log, got[k] == expected[k], "regex UTF-8 "
                       "result test: %s subject, pattern %ui of %ui returned "
                       "%i with a kept result, %i without", name, k + 1, n,
                       got[k], expected[k]);
    }

    expected_checks = (valid == 2) ? 0 : 1;

    NXT_TEST_CHECK(thr->log, checks == expected_checks, "regex UTF-8 result "
                   "test: %s subject, %ui patterns checked the subject %ui "
                   "times, expected %ui", name, n, checks, expected_checks);

    expected_state = (valid != 0) ? NXT_REGEX_UTF_VALID
                                  : NXT_REGEX_UTF_INVALID;

    NXT_TEST_CHECK(thr->log, utf.state == expected_state
                   && utf.start == p && utf.length == length,
                   "regex UTF-8 result test: %s subject, kept result %d, "
                   "expected %d", name, (int) utf.state, (int) expected_state);

    return NXT_OK;
}


/*
 * The kept result is for its own bytes only.  A result that a caller gives
 * by mistake with another subject is not used: with another start or
 * another length the subject is checked again.  A result also does not
 * come from a match that stopped at a limit, or from a pattern that
 * matches bytes.  The caller must reset the result when it changes the
 * bytes at the same place: nxt_regex_match() cannot see that.
 */

static nxt_int_t
nxt_regex_utf_test_misuse(nxt_thread_t *thr, nxt_mp_t *mp,
    nxt_regex_match_t *match, nxt_regex_t *re)
{
    u_char           buf[8], slow_subject[40];
    nxt_int_t        ret, expected;
    nxt_uint_t       checks;
    nxt_regex_t      *slow, *bytes;
    nxt_regex_utf_t  utf;

    /* A valid result of "/п" given with "/\xff" at another place. */

    nxt_regex_utf_init(&utf);

    ret = nxt_regex_match(re, (u_char *) "/\xd0\xbf", 3, match, &utf);

    NXT_TEST_CHECK(thr->log, ret == 1 && utf.state == NXT_REGEX_UTF_VALID,
                   "regex UTF-8 misuse test: \"/\\xd0\\xbf\" returned %i, "
                   "kept result %d", ret, (int) utf.state);

    nxt_memcpy(buf, "/\xff\xff", 3);

    expected = nxt_regex_match(re, buf, 3, match, NULL);

    checks = nxt_regex_test_utf_checks;
    ret = nxt_regex_match(re, buf, 3, match, &utf);
    checks = nxt_regex_test_utf_checks - checks;

    NXT_TEST_CHECK(thr->log, ret == expected && checks == 1
                   && utf.state == NXT_REGEX_UTF_INVALID && utf.start == buf,
                   "regex UTF-8 misuse test: a valid result of another "
                   "subject gave %i (expected %i), %ui checks, kept result %d",
                   ret, expected, checks, (int) utf.state);

    /* An invalid result given with a valid subject at another place. */

    checks = nxt_regex_test_utf_checks;
    ret = nxt_regex_match(re, (u_char *) "/\xd0\xbf", 3, match, &utf);
    checks = nxt_regex_test_utf_checks - checks;

    NXT_TEST_CHECK(thr->log, ret == 1 && checks == 1
                   && utf.state == NXT_REGEX_UTF_VALID,
                   "regex UTF-8 misuse test: an invalid result of another "
                   "subject gave %i (expected 1), %ui checks", ret, checks);

    /* The same start with another length: "/п" is valid, "/п\xff" is not. */

    nxt_memcpy(buf, "/\xd0\xbf\xff", 4);
    nxt_regex_utf_init(&utf);

    ret = nxt_regex_match(re, buf, 3, match, &utf);

    NXT_TEST_CHECK(thr->log, ret == 1 && utf.state == NXT_REGEX_UTF_VALID,
                   "regex UTF-8 misuse test: 3 bytes returned %i, kept "
                   "result %d", ret, (int) utf.state);

    expected = nxt_regex_match(re, buf, 4, match, NULL);

    checks = nxt_regex_test_utf_checks;
    ret = nxt_regex_match(re, buf, 4, match, &utf);
    checks = nxt_regex_test_utf_checks - checks;

    NXT_TEST_CHECK(thr->log, ret == expected && checks == 1
                   && utf.state == NXT_REGEX_UTF_INVALID && utf.length == 4,
                   "regex UTF-8 misuse test: a result for 3 bytes given "
                   "with 4 gave %i (expected %i), %ui checks, kept result %d",
                   ret, expected, checks, (int) utf.state);

    /* After a reset, the same place with new bytes is checked again. */

    buf[3] = 'a';
    nxt_regex_utf_init(&utf);

    checks = nxt_regex_test_utf_checks;
    ret = nxt_regex_match(re, buf, 4, match, &utf);
    checks = nxt_regex_test_utf_checks - checks;

    NXT_TEST_CHECK(thr->log, ret == 0 && checks == 1
                   && utf.state == NXT_REGEX_UTF_VALID,
                   "regex UTF-8 misuse test: new bytes after a reset gave "
                   "%i (expected 0), %ui checks, kept result %d", ret, checks,
                   (int) utf.state);

    /*
     * A match that stops at the limit keeps no result.  The subject is
     * "п" and 32 "a" bytes.
     */

    slow = nxt_regex_test_compile(thr, mp, "^\xd0\xbf?(a+)+[bc]");
    bytes = (slow != NULL) ? nxt_regex_test_compile(thr, mp, "^/a\xff$")
                           : NULL;
    if (bytes == NULL) {
        return NXT_ERROR;
    }

    nxt_memcpy(slow_subject, "\xd0\xbf", 2);
    nxt_memset(slow_subject + 2, 'a', 32);

    nxt_regex_utf_init(&utf);

    ret = nxt_regex_match(slow, slow_subject, 34, match, &utf);

    NXT_TEST_CHECK(thr->log, ret == NXT_ERROR
                   && utf.state == NXT_REGEX_UTF_UNKNOWN,
                   "regex UTF-8 misuse test: a limit returned %i, kept "
                   "result %d, expected %i and %d", ret, (int) utf.state,
                   (nxt_int_t) NXT_ERROR, (int) NXT_REGEX_UTF_UNKNOWN);

    /* A pattern that matches bytes does not check, so it keeps nothing. */

    checks = nxt_regex_test_utf_checks;
    ret = nxt_regex_match(bytes, (u_char *) "/a\xff", 3, match, &utf);
    checks = nxt_regex_test_utf_checks - checks;

    NXT_TEST_CHECK(thr->log, ret == 1 && checks == 0
                   && utf.state == NXT_REGEX_UTF_UNKNOWN,
                   "regex UTF-8 misuse test: a byte-mode pattern returned %i, "
                   "%ui checks, kept result %d", ret, checks, (int) utf.state);

    /*
     * A pattern that matches bytes after a kept invalid result: it has no
     * twin, and it does not use the result.
     */

    nxt_memcpy(buf, "/a\xff", 3);
    nxt_regex_utf_init(&utf);

    ret = nxt_regex_match(re, buf, 3, match, &utf);

    NXT_TEST_CHECK(thr->log, ret == 0 && utf.state == NXT_REGEX_UTF_INVALID,
                   "regex UTF-8 misuse test: \"/a\\xff\" returned %i, kept "
                   "result %d", ret, (int) utf.state);

    checks = nxt_regex_test_utf_checks;
    ret = nxt_regex_match(bytes, buf, 3, match, &utf);
    checks = nxt_regex_test_utf_checks - checks;

    NXT_TEST_CHECK(thr->log, ret == 1 && checks == 0
                   && utf.state == NXT_REGEX_UTF_INVALID,
                   "regex UTF-8 misuse test: a byte-mode pattern after a kept "
                   "invalid result returned %i (expected 1), %ui checks, kept "
                   "result %d", ret, checks, (int) utf.state);

    /* With no kept result, each match checks, as before. */

    checks = nxt_regex_test_utf_checks;
    (void) nxt_regex_match(re, (u_char *) "/\xd0\xbf", 3, match, NULL);
    (void) nxt_regex_match(re, (u_char *) "/\xd0\xbf", 3, match, NULL);
    checks = nxt_regex_test_utf_checks - checks;

    NXT_TEST_CHECK(thr->log, checks == 2, "regex UTF-8 misuse test: two "
                   "matches with no kept result checked %ui times, expected 2",
                   checks);

    return NXT_OK;
}

#endif
