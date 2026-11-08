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
 * Patterns match in UTF mode only with PCRE2 10.48 or later, in the headers
 * and in the library that runs.  Before 10.48, PCRE2_MATCH_INVALID_UTF is
 * not safe with a subject from a client.
 */
#if (NXT_HAVE_PCRE2 && defined(PCRE2_MATCH_INVALID_UTF)                     \
     && (PCRE2_MAJOR > 10 || (PCRE2_MAJOR == 10 && PCRE2_MINOR >= 48)))
#define NXT_REGEX_TEST_UTF_HEADERS  1
#else
#define NXT_REGEX_TEST_UTF_HEADERS  0
#endif

static nxt_regex_t *nxt_regex_test_compile(nxt_thread_t *thr, nxt_mp_t *mp,
    const char *pattern);
static nxt_int_t nxt_regex_reuse_test(nxt_thread_t *thr);
static nxt_int_t nxt_regex_route_test(nxt_thread_t *thr);
static nxt_int_t nxt_regex_test_utf(void);
static nxt_int_t nxt_regex_jit_test(nxt_thread_t *thr);

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


static nxt_int_t
nxt_regex_test_utf(void)
{
#if (NXT_REGEX_TEST_UTF_HEADERS)
    int   major, minor;
    char  version[64];

    if (pcre2_config(PCRE2_CONFIG_VERSION, version) <= 0
        || sscanf(version, "%d.%d", &major, &minor) != 2)
    {
        return 0;
    }

    return (major > 10 || (major == 10 && minor >= 48));
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

    *result = nxt_regex_match(re, subject, length, match);

    return NXT_OK;
}


nxt_int_t
nxt_regex_test(nxt_thread_t *thr)
{
    u_char     *subject;
    nxt_mp_t   *mp;
    nxt_int_t  ret, slow, deep, shallow, starts, anchored, utf, utf_char,
               utf_invalid, utf_pattern, utf_escape, utf_escape_byte,
               utf_negative, utf_lookbehind;

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
     * In UTF mode, invalid UTF-8 in the subject matches no pattern item.
     * The match must not fail with an error.
     */

    NXT_TEST_CHECK(thr->log, utf_invalid == !utf, "regex test: subject "
                   "with invalid UTF-8 returned %i, expected %i",
                   utf_invalid, (nxt_int_t) !utf);

    NXT_TEST_CHECK(thr->log, utf_pattern == 1, "regex test: pattern with "
                   "invalid UTF-8 returned %i, expected 1", utf_pattern);

    NXT_TEST_CHECK(thr->log, utf_escape == utf, "regex test: \"\\xff\" on "
                   "U+00FF returned %i, expected %i", utf_escape, utf);

    NXT_TEST_CHECK(thr->log, utf_escape_byte == !utf, "regex test: "
                   "\"\\xff\" on the byte FF returned %i, expected %i",
                   utf_escape_byte, (nxt_int_t) !utf);

    NXT_TEST_CHECK(thr->log, utf_negative == !utf, "regex test: \".*\" on "
                   "an invalid byte returned %i, expected %i",
                   utf_negative, (nxt_int_t) !utf);

    NXT_TEST_CHECK(thr->log, utf_lookbehind == 1, "regex test: lookbehind "
                   "before an invalid byte returned %i, expected 1",
                   utf_lookbehind);

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

    limited = nxt_regex_match(slow, subject, 2001, match);
    matched = nxt_regex_match(simple, (u_char *) "/foo", 4, match);
    missed = nxt_regex_match(simple, (u_char *) "/baz", 4, match);

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
