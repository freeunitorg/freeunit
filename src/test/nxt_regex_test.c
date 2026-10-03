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

static nxt_int_t nxt_regex_jit_test(nxt_thread_t *thr);

/*
 * Compile the pattern and match it against the subject.  The match result
 * goes to *result.  The function returns NXT_ERROR if the setup fails.
 */

static nxt_int_t
nxt_regex_test_run(nxt_thread_t *thr, nxt_mp_t *mp, const char *pattern,
    u_char *subject, size_t length, nxt_int_t *result)
{
    nxt_str_t          source;
    nxt_regex_t        *re;
    nxt_regex_err_t    err;
    nxt_regex_match_t  *match;

    source.start = (u_char *) pattern;
    source.length = nxt_strlen(pattern);

    re = nxt_regex_compile(mp, &source, &err);
    if (re == NULL) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "regex test: compile of "
                      "\"%s\" failed: %s", pattern, err.msg);
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
    nxt_int_t  ret, slow, deep, shallow, starts, anchored;

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

    ret = nxt_regex_jit_test(thr);
    if (ret != NXT_OK) {
        return ret;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "regex test passed");

    return NXT_OK;
}


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
