
/*
 * Copyright (C) Axel Duch
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_REGEX_H_INCLUDED_
#define _NXT_REGEX_H_INCLUDED_

#if (NXT_HAVE_REGEX)

/*
 * The most match steps that one match of one pattern may use.  The library
 * default is 10,000,000.  The library counts steps from zero again at each
 * start position of a pattern that is not anchored.  So such a pattern also
 * gets a callout before each item, and one match may make at most this
 * number of callouts in total.
 */
#define NXT_REGEX_MATCH_LIMIT  100000

/*
 * The most bytes of a subject that a match error writes to the log.  The
 * subject can come from a client.
 */
#define NXT_REGEX_LOG_SUBJECT  64


typedef struct nxt_regex_s        nxt_regex_t;
typedef struct nxt_regex_match_s  nxt_regex_match_t;

typedef struct {
    size_t      offset;

#if (NXT_HAVE_PCRE2)
#define ERR_BUF_SIZE  256
    u_char      msg[ERR_BUF_SIZE];
#else
    const char  *msg;
#endif
} nxt_regex_err_t;


NXT_EXPORT void nxt_regex_init(void);
NXT_EXPORT nxt_regex_t *nxt_regex_compile(nxt_mp_t *mp, nxt_str_t *source,
    nxt_regex_err_t *err);
NXT_EXPORT nxt_regex_match_t *nxt_regex_match_create(nxt_mp_t *mp, size_t size);
NXT_EXPORT nxt_int_t nxt_regex_match(nxt_regex_t *re, u_char *subject,
    size_t length, nxt_regex_match_t *match);

#if (NXT_TESTS && NXT_HAVE_PCRE2)
typedef struct {
    nxt_uint_t  compiled;
    nxt_uint_t  executed;
    nxt_uint_t  freed;
    nxt_uint_t  fallback;
} nxt_regex_jit_test_stats_t;

/* 0: normal, 1: unavailable, 2: compile failure, 3: stack limit,
 * 4: cleanup registration failure, 5: stack limit with exhausted budget.
 * Hooks are only in the test archive. */
NXT_EXPORT void nxt_regex_jit_test_mode(nxt_uint_t mode);
NXT_EXPORT nxt_regex_jit_test_stats_t nxt_regex_jit_test_stats(void);
NXT_EXPORT nxt_bool_t nxt_regex_jit_test_available(void);
#endif

#endif /* NXT_HAVE_REGEX */

#endif /* _NXT_REGEX_H_INCLUDED_ */
