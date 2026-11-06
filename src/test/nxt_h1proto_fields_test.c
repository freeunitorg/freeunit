/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * The "Connection" and the "Transfer-Encoding" request field handlers of
 * src/nxt_h1proto.c.
 *
 * "Connection" is a comma-separated list of options (RFC 9110, 7.6.1).  An
 * option is a token in any letter case.  Spaces and tabs around an element
 * are not part of it, and an empty element does not count (RFC 9110,
 * 5.6.1).  "close" wins over "keep-alive", in one field line and over
 * several of them (RFC 9112, 9.6).  The test runs the handler on each field
 * line of a request, as the field dispatch does, and reads three results:
 * keep-alive, close and upgrade.
 *
 * The expected results come from three sources: a table of named cases, a
 * rule for each value of a byte next to or inside an option, and, for
 * random lists, a reference model.  The model has its own fold, and it
 * looks for each option at the start of each element.  It does not cut the
 * value into trimmed elements as the handler does.
 *
 * Before this test, the handler searched the value for the three options
 * as substrings.  The table marks the cases that this changed: an option
 * inside a longer token does not count now, and "close" wins over
 * "keep-alive" in any order.
 *
 * "Transfer-Encoding" is chunked only when the value is the one coding name
 * "chunked", in any letter case (RFC 9112, 7).
 *
 * Each value is copied to the end of a page that a PROT_NONE page follows,
 * and to the start of the page after it.  So a read past the value or
 * before it kills the child.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_h1proto.h>
#include "nxt_tests.h"


#define NXT_H1PF_MAX_VALUE  64
#define NXT_H1PF_MAX_LINES  3
#define NXT_H1PF_ROUNDS     200000

/* The results of the "Connection" handler. */
#define NXT_H1PF_KA         1   /* h1p->keepalive */
#define NXT_H1PF_CL         2   /* h1p->connection_close */
#define NXT_H1PF_UP         4   /* h1p->connection_upgrade */

#define nxt_h1pf_lowcase(c)                                                   \
    (u_char) (((c) >= 'A' && (c) <= 'Z') ? (c) + ('a' - 'A') : (c))

#define nxt_h1pf_ows(c)                                                       \
    ((c) == ' ' || (c) == '\t')


typedef struct {
    nxt_thread_t  *thr;
    u_char        *end;      /* A PROT_NONE page starts here. */
    u_char        *start;    /* A PROT_NONE page ends here. */
    uint32_t      seed;
    nxt_uint_t    checks;
} nxt_h1pf_ctx_t;


typedef struct {
    u_char        data[NXT_H1PF_MAX_VALUE];
    size_t        length;
} nxt_h1pf_value_t;


typedef struct {
    uint8_t       http11;
    uint8_t       result;
    const char    *lines[NXT_H1PF_MAX_LINES];
} nxt_h1pf_case_t;


static const char  *nxt_h1pf_options[] = { "close", "keep-alive", "upgrade" };

static const uint8_t  nxt_h1pf_option_bits[] = {
    NXT_H1PF_CL, NXT_H1PF_KA, NXT_H1PF_UP,
};


static const nxt_h1pf_case_t  nxt_h1pf_cases[] = {
    { 1, NXT_H1PF_CL,                 { "close" } },
    { 1, NXT_H1PF_CL,                 { "Close" } },
    { 1, NXT_H1PF_CL,                 { "CLOSE" } },
    { 0, NXT_H1PF_KA,                 { "keep-alive" } },
    { 0, NXT_H1PF_KA,                 { "Keep-Alive" } },
    { 0, NXT_H1PF_KA,                 { "KEEP-ALIVE" } },
    { 1, NXT_H1PF_KA,                 { "keep-alive" } },
    { 1, NXT_H1PF_KA | NXT_H1PF_UP,   { "Upgrade" } },
    { 1, NXT_H1PF_KA | NXT_H1PF_UP,   { "UPGRADE" } },

    /* Firefox sends this for a WebSocket (nginx/unit PR 1550). */
    { 1, NXT_H1PF_KA | NXT_H1PF_UP,   { "keep-alive, Upgrade" } },
    { 0, NXT_H1PF_KA | NXT_H1PF_UP,   { "keep-alive, Upgrade" } },
    { 1, NXT_H1PF_CL | NXT_H1PF_UP,   { "Upgrade, close" } },
    { 1, NXT_H1PF_CL,                 { "TE, close" } },

    /*
     * "close" wins.  The substring search let "keep-alive" win in the same
     * line, and the last line win over the lines before it.
     */
    { 1, NXT_H1PF_CL,                 { "close, keep-alive" } },
    { 1, NXT_H1PF_CL,                 { "keep-alive, close" } },
    { 0, NXT_H1PF_CL,                 { "close, keep-alive" } },
    { 1, NXT_H1PF_CL,                 { "close", "keep-alive" } },
    { 0, NXT_H1PF_CL,                 { "close", "Keep-Alive" } },
    { 1, NXT_H1PF_CL,                 { "keep-alive", "close" } },
    { 1, NXT_H1PF_CL | NXT_H1PF_UP,   { "keep-alive", "Upgrade", "close" } },
    { 1, NXT_H1PF_CL,                 { "close", "keep-alive",
                                        "keep-alive" } },

    /* An option inside a longer token.  The substring search matched it. */
    { 1, NXT_H1PF_KA,                 { "X-Closer" } },
    { 1, NXT_H1PF_KA,                 { "closed" } },
    { 1, NXT_H1PF_KA,                 { "x-close" } },
    { 0, 0,                           { "Keep-Alive-Extension" } },
    { 0, 0,                           { "x-keep-alive" } },
    { 1, NXT_H1PF_KA,                 { "X-Upgrade-Info" } },
    { 1, NXT_H1PF_KA,                 { "upgraded" } },

    /* A token has no space in it, so each of these is one element. */
    { 1, NXT_H1PF_KA,                 { "close keep-alive" } },
    { 0, 0,                           { "keep-alive upgrade" } },
    { 1, NXT_H1PF_KA,                 { "close;x=1" } },
    { 1, NXT_H1PF_KA,                 { "c lose" } },
    { 1, NXT_H1PF_KA,                 { "clos" } },

    /* Spaces, tabs and empty elements. */
    { 1, NXT_H1PF_CL,                 { " close" } },
    { 1, NXT_H1PF_CL,                 { "close " } },
    { 1, NXT_H1PF_CL,                 { "\tclose\t" } },
    { 1, NXT_H1PF_CL,                 { "  close ," } },
    { 1, NXT_H1PF_CL,                 { ", ,\tclose\t, ," } },
    { 1, NXT_H1PF_CL,                 { "close," } },
    { 1, NXT_H1PF_CL,                 { ",close" } },
    { 0, NXT_H1PF_KA,                 { ",,,keep-alive,,," } },
    { 1, NXT_H1PF_KA | NXT_H1PF_UP,   { "keep-alive , upgrade" } },
    { 1, NXT_H1PF_KA,                 { "," } },
    { 1, NXT_H1PF_KA,                 { " , \t, " } },
    { 1, NXT_H1PF_KA,                 { "" } },
    { 0, 0,                           { "" } },
    { 0, NXT_H1PF_KA,                 { "", "keep-alive" } },
};


static u_char *
nxt_h1pf_hex(u_char *p, const u_char *s, size_t length)
{
    static const u_char  hex[] = "0123456789abcdef";

    while (length-- != 0) {
        *p++ = hex[*s >> 4];
        *p++ = hex[*s++ & 0xf];
    }

    return p;
}


static void
nxt_h1pf_report(nxt_h1pf_ctx_t *ctx, const char *what, nxt_bool_t http11,
    const nxt_h1pf_value_t *lines, nxt_uint_t n, nxt_int_t got,
    nxt_int_t expect)
{
    u_char      *p, buf[NXT_H1PF_MAX_LINES * (2 * NXT_H1PF_MAX_VALUE + 1)];
    nxt_uint_t  i;

    p = buf;

    for (i = 0; i < n; i++) {
        if (i != 0) {
            *p++ = '|';
        }

        p = nxt_h1pf_hex(p, lines[i].data, lines[i].length);
    }

    nxt_log_alert(ctx->thr->log, "h1proto fields test: %s: HTTP/1.%d "
                  "lines %*s gave %i, expected %i", what, http11 ? 1 : 0,
                  (size_t) (p - buf), buf, got, expect);
}


/*
 * Runs the "Connection" handler on the lines of one request.  The request
 * starts with the keep-alive default of its version, as in
 * nxt_h1p_conn_request_header_parse().  Each line runs with the value at
 * the end of a page and at the start of one.  Both runs must give the same
 * result.
 */

static nxt_int_t
nxt_h1pf_conn_run(nxt_h1pf_ctx_t *ctx, nxt_bool_t http11,
    const nxt_h1pf_value_t *lines, nxt_uint_t n, nxt_uint_t *result)
{
    u_char              *p;
    nxt_int_t           ret;
    nxt_uint_t          i, layout, got, res;
    nxt_h1proto_t       h1p;
    nxt_http_field_t    field;
    nxt_http_request_t  r;

    res = 0;

    for (layout = 0; layout < 2; layout++) {
        nxt_memzero(&h1p, sizeof(nxt_h1proto_t));
        nxt_memzero(&r, sizeof(nxt_http_request_t));

        r.proto.h1 = &h1p;
        h1p.keepalive = http11;

        for (i = 0; i < n; i++) {
            p = (layout == 0) ? ctx->end - lines[i].length : ctx->start;
            nxt_memcpy(p, lines[i].data, lines[i].length);

            nxt_memzero(&field, sizeof(nxt_http_field_t));
            field.value = p;
            field.value_length = lines[i].length;

            ret = nxt_h1p_test_connection(&r, &field);

            if (ret != NXT_OK || !field.hopbyhop || field.skip) {
                nxt_h1pf_report(ctx, "Connection handler returned an error "
                                "or marked the field wrong", http11, lines,
                                n, ret, NXT_OK);
                return NXT_ERROR;
            }
        }

        got = (h1p.keepalive ? NXT_H1PF_KA : 0)
              | (h1p.connection_close ? NXT_H1PF_CL : 0)
              | (h1p.connection_upgrade ? NXT_H1PF_UP : 0);

        if (layout != 0 && got != res) {
            nxt_h1pf_report(ctx, "Connection result depends on the layout",
                            http11, lines, n, got, res);
            return NXT_ERROR;
        }

        res = got;
    }

    ctx->checks++;
    *result = res;

    return NXT_OK;
}


static nxt_int_t
nxt_h1pf_conn_check(nxt_h1pf_ctx_t *ctx, const char *what, nxt_bool_t http11,
    const nxt_h1pf_value_t *lines, nxt_uint_t n, nxt_uint_t expect)
{
    nxt_uint_t  res;

    if (nxt_h1pf_conn_run(ctx, http11, lines, n, &res) != NXT_OK) {
        return NXT_ERROR;
    }

    if (res != expect) {
        nxt_h1pf_report(ctx, what, http11, lines, n, res, expect);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * The result of a request whose lines have the option "bit" or not, and no
 * other option.
 */

static nxt_uint_t
nxt_h1pf_expect(nxt_bool_t http11, nxt_uint_t bit, nxt_bool_t match)
{
    if (!match) {
        return http11 ? NXT_H1PF_KA : 0;
    }

    switch (bit) {

    case NXT_H1PF_CL:
        return NXT_H1PF_CL;

    case NXT_H1PF_KA:
        return NXT_H1PF_KA;

    default:
        return (http11 ? NXT_H1PF_KA : 0) | NXT_H1PF_UP;
    }
}


/*
 * The reference model.  It folds a copy of the value with its own fold.
 * Then at the start of each element it skips spaces and tabs, compares the
 * option, and skips spaces and tabs again; the element must end there.
 */

static nxt_bool_t
nxt_h1pf_model_has(const nxt_h1pf_value_t *v, const char *option)
{
    u_char  low[NXT_H1PF_MAX_VALUE];
    size_t  i, k, n, length;

    length = v->length;
    n = nxt_strlen(option);

    for (i = 0; i < length; i++) {
        low[i] = nxt_h1pf_lowcase(v->data[i]);
    }

    i = 0;

    for ( ;; ) {
        k = i;

        while (k < length && nxt_h1pf_ows(low[k])) {
            k++;
        }

        if (length - k >= n && memcmp(&low[k], option, n) == 0) {
            k += n;

            while (k < length && nxt_h1pf_ows(low[k])) {
                k++;
            }

            if (k == length || low[k] == ',') {
                return 1;
            }
        }

        while (i < length && low[i] != ',') {
            i++;
        }

        if (i == length) {
            return 0;
        }

        i++;
    }
}


static nxt_uint_t
nxt_h1pf_model(nxt_bool_t http11, const nxt_h1pf_value_t *lines,
    nxt_uint_t n)
{
    nxt_uint_t  i, j, found;

    found = 0;

    for (i = 0; i < n; i++) {
        for (j = 0; j < nxt_nitems(nxt_h1pf_options); j++) {
            if (nxt_h1pf_model_has(&lines[i], nxt_h1pf_options[j])) {
                found |= nxt_h1pf_option_bits[j];
            }
        }
    }

    if (found & NXT_H1PF_CL) {
        return found & ~NXT_H1PF_KA;
    }

    if (http11) {
        found |= NXT_H1PF_KA;
    }

    return found;
}


static void
nxt_h1pf_value_set(nxt_h1pf_value_t *v, const char *s)
{
    v->length = nxt_strlen(s);
    nxt_memcpy(v->data, s, v->length);
}


static nxt_int_t
nxt_h1pf_conn_cases(nxt_h1pf_ctx_t *ctx)
{
    nxt_uint_t              i, n;
    nxt_h1pf_value_t        lines[NXT_H1PF_MAX_LINES];
    const nxt_h1pf_case_t   *c;

    for (i = 0; i < nxt_nitems(nxt_h1pf_cases); i++) {
        c = &nxt_h1pf_cases[i];

        for (n = 0; n < NXT_H1PF_MAX_LINES && c->lines[n] != NULL; n++) {
            nxt_h1pf_value_set(&lines[n], c->lines[n]);
        }

        if (nxt_h1pf_conn_check(ctx, "named case", c->http11, lines, n,
                                c->result)
            != NXT_OK)
        {
            return NXT_ERROR;
        }

        /* The model must agree with the table. */

        if (nxt_h1pf_model(c->http11, lines, n) != c->result) {
            nxt_h1pf_report(ctx, "the model disagrees with a named case",
                            c->http11, lines, n,
                            nxt_h1pf_model(c->http11, lines, n), c->result);
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


/*
 * Each option with every byte value before it, after it, in place of each
 * of its bytes, and inserted between two of its bytes.  Before or after
 * the option, only a comma, a space or a tab keeps the option whole.  In
 * place of a byte, only the same byte or a letter of the other case
 * matches.  A byte inserted inside the option never matches.
 */

static nxt_int_t
nxt_h1pf_conn_bytes(nxt_h1pf_ctx_t *ctx)
{
    u_char            c;
    size_t            length;
    nxt_uint_t        b, i, j, http11, bit;
    nxt_bool_t        sep, match;
    const u_char      *opt;
    nxt_h1pf_value_t  v;

    for (j = 0; j < nxt_nitems(nxt_h1pf_options); j++) {
        opt = (const u_char *) nxt_h1pf_options[j];
        length = nxt_strlen(opt);
        bit = nxt_h1pf_option_bits[j];

        for (b = 0; b < 256; b++) {
            c = (u_char) b;
            sep = (c == ',' || c == ' ' || c == '\t');

            for (http11 = 0; http11 < 2; http11++) {

                /* The byte before the option. */

                v.data[0] = c;
                nxt_memcpy(&v.data[1], opt, length);
                v.length = length + 1;

                if (nxt_h1pf_conn_check(ctx, "byte before the option",
                                        http11, &v, 1,
                                        nxt_h1pf_expect(http11, bit, sep))
                    != NXT_OK)
                {
                    return NXT_ERROR;
                }

                /* The byte after the option. */

                nxt_memcpy(v.data, opt, length);
                v.data[length] = c;

                if (nxt_h1pf_conn_check(ctx, "byte after the option",
                                        http11, &v, 1,
                                        nxt_h1pf_expect(http11, bit, sep))
                    != NXT_OK)
                {
                    return NXT_ERROR;
                }

                /* The byte in place of each byte of the option. */

                v.length = length;

                for (i = 0; i < length; i++) {
                    nxt_memcpy(v.data, opt, length);
                    v.data[i] = c;

                    match = (c == opt[i]
                             || (opt[i] >= 'a' && opt[i] <= 'z'
                                 && c == opt[i] - ('a' - 'A')));

                    if (nxt_h1pf_conn_check(ctx, "byte in the option",
                                            http11, &v, 1,
                                            nxt_h1pf_expect(http11, bit,
                                                            match))
                        != NXT_OK)
                    {
                        return NXT_ERROR;
                    }
                }

                /* The byte inserted between two bytes of the option. */

                v.length = length + 1;

                for (i = 1; i < length; i++) {
                    nxt_memcpy(v.data, opt, i);
                    v.data[i] = c;
                    nxt_memcpy(&v.data[i + 1], &opt[i], length - i);

                    if (nxt_h1pf_conn_check(ctx, "byte inserted in the option",
                                            http11, &v, 1,
                                            nxt_h1pf_expect(http11, bit, 0))
                        != NXT_OK)
                    {
                        return NXT_ERROR;
                    }
                }
            }
        }
    }

    return NXT_OK;
}


/* Each option in each combination of the letter cases of its letters. */

static nxt_int_t
nxt_h1pf_conn_cases_of_letters(nxt_h1pf_ctx_t *ctx)
{
    size_t            length;
    nxt_uint_t        i, j, mask, http11;
    const u_char      *opt;
    nxt_h1pf_value_t  v;

    for (j = 0; j < nxt_nitems(nxt_h1pf_options); j++) {
        opt = (const u_char *) nxt_h1pf_options[j];
        length = nxt_strlen(opt);

        for (mask = 0; mask < ((nxt_uint_t) 1 << length); mask++) {

            for (i = 0; i < length; i++) {
                if ((mask & ((nxt_uint_t) 1 << i)) == 0) {
                    v.data[i] = opt[i];
                    continue;
                }

                /* A mask bit on '-' gives a value that was checked. */

                if (opt[i] < 'a' || opt[i] > 'z') {
                    break;
                }

                v.data[i] = opt[i] - ('a' - 'A');
            }

            if (i != length) {
                continue;
            }

            v.length = length;

            for (http11 = 0; http11 < 2; http11++) {
                if (nxt_h1pf_conn_check(ctx, "letter case of the option",
                                        http11, &v, 1,
                                        nxt_h1pf_expect(http11,
                                                        nxt_h1pf_option_bits[j],
                                                        1))
                    != NXT_OK)
                {
                    return NXT_ERROR;
                }
            }
        }
    }

    return NXT_OK;
}


static uint32_t
nxt_h1pf_random(nxt_h1pf_ctx_t *ctx)
{
    uint32_t  x;

    /* xorshift32: the same sequence on every run and platform. */

    x = ctx->seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    ctx->seed = x;

    return x;
}


static void
nxt_h1pf_append(nxt_h1pf_value_t *v, const u_char *s, size_t length)
{
    if (NXT_H1PF_MAX_VALUE - v->length >= length) {
        nxt_memcpy(&v->data[v->length], s, length);
        v->length += length;
    }
}


/* Zero, one or two spaces or tabs. */

static void
nxt_h1pf_append_ows(nxt_h1pf_ctx_t *ctx, nxt_h1pf_value_t *v)
{
    u_char      c;
    uint32_t    r;
    nxt_uint_t  i, n;

    r = nxt_h1pf_random(ctx);
    n = r % 3;

    for (i = 0; i < n; i++) {
        c = (r & (0x100 << i)) ? '\t' : ' ';
        nxt_h1pf_append(v, &c, 1);
    }
}


/*
 * Random lists of 1 to 3 lines.  An element is an option in a random case,
 * a near miss, a few bytes that are often a comma, a space, a tab or a
 * byte next to a letter, or nothing.
 */

static nxt_int_t
nxt_h1pf_conn_random(nxt_h1pf_ctx_t *ctx)
{
    u_char            c, buf[8];
    size_t            length;
    uint32_t          r;
    nxt_uint_t        round, n, i, e, elements, k;
    const char        *s;
    nxt_h1pf_value_t  lines[NXT_H1PF_MAX_LINES];

    static const char  *near[] = {
        "closed", "x-close", "clos", "lose", "close-x", "keep-alive-x",
        "keepalive", "keep_alive", "x-keep-alive", "keep-alivE2", "upgrades",
        "upgrad", "x-upgrade", "Upgrade-Insecure-Requests", "TE", "foo",
    };

    static const char  *seps[] = { ",", ", ", " ,", ",\t", ",,", " , " };

    static const u_char  edge[] = {
        ',', ' ', '\t', 0x00, 0x0d, '-', 0x7f, 0x80, 0xff, 0xe3, 0xc3, '@',
        '[', '`', '{', 'A', 'Z', 'a', 'z', 'c', 'k', 'u', 'E', 'e',
    };

    for (round = 0; round < NXT_H1PF_ROUNDS; round++) {
        n = 1 + nxt_h1pf_random(ctx) % NXT_H1PF_MAX_LINES;

        for (i = 0; i < n; i++) {
            lines[i].length = 0;
            elements = nxt_h1pf_random(ctx) % 5;

            for (e = 0; e < elements; e++) {
                if (e != 0) {
                    s = seps[nxt_h1pf_random(ctx) % nxt_nitems(seps)];
                    nxt_h1pf_append(&lines[i], (const u_char *) s,
                                    nxt_strlen(s));
                }

                nxt_h1pf_append_ows(ctx, &lines[i]);

                r = nxt_h1pf_random(ctx);

                switch (r % 8) {

                case 0:
                case 1:
                case 2:
                    s = nxt_h1pf_options[(r >> 3) % 3];
                    length = nxt_strlen(s);

                    for (k = 0; k < length; k++) {
                        c = s[k];

                        if (c >= 'a' && c <= 'z'
                            && (nxt_h1pf_random(ctx) & 1))
                        {
                            c -= 'a' - 'A';
                        }

                        nxt_h1pf_append(&lines[i], &c, 1);
                    }

                    break;

                case 3:
                case 4:
                    s = near[(r >> 3) % nxt_nitems(near)];
                    nxt_h1pf_append(&lines[i], (const u_char *) s,
                                    nxt_strlen(s));
                    break;

                case 5:
                case 6:
                    length = 1 + (r >> 3) % sizeof(buf);

                    for (k = 0; k < length; k++) {
                        r = nxt_h1pf_random(ctx);
                        buf[k] = (r & 1) ? edge[(r >> 1) % sizeof(edge)]
                                         : (u_char) (r >> 8);
                    }

                    nxt_h1pf_append(&lines[i], buf, length);
                    break;

                default:
                    break;
                }

                nxt_h1pf_append_ows(ctx, &lines[i]);
            }
        }

        k = nxt_h1pf_random(ctx) & 1;

        if (nxt_h1pf_conn_check(ctx, "random list", k, lines, n,
                                nxt_h1pf_model(k, lines, n))
            != NXT_OK)
        {
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


/*
 * Runs the "Transfer-Encoding" handler on one field line of a new request.
 * The field must be marked as skipped and hop-by-hop, and the request must
 * keep the field as its chunked field when the value is chunked.
 */

static nxt_int_t
nxt_h1pf_te_check(nxt_h1pf_ctx_t *ctx, const nxt_h1pf_value_t *v,
    nxt_bool_t chunked)
{
    u_char              *p;
    nxt_int_t           ret;
    nxt_uint_t          layout;
    nxt_http_te_t       te;
    nxt_h1proto_t       h1p;
    nxt_http_field_t    field;
    nxt_http_request_t  r;

    te = chunked ? NXT_HTTP_TE_CHUNKED : NXT_HTTP_TE_UNSUPPORTED;

    for (layout = 0; layout < 2; layout++) {
        nxt_memzero(&h1p, sizeof(nxt_h1proto_t));
        nxt_memzero(&r, sizeof(nxt_http_request_t));

        r.proto.h1 = &h1p;

        p = (layout == 0) ? ctx->end - v->length : ctx->start;
        nxt_memcpy(p, v->data, v->length);

        nxt_memzero(&field, sizeof(nxt_http_field_t));
        field.value = p;
        field.value_length = v->length;

        ret = nxt_h1p_test_transfer_encoding(&r, &field);

        if (ret != NXT_OK || !field.skip || !field.hopbyhop
            || h1p.transfer_encoding != te
            || r.chunked_field != (chunked ? &field : NULL))
        {
            nxt_h1pf_report(ctx, "Transfer-Encoding", 1, v, 1,
                            h1p.transfer_encoding, te);
            return NXT_ERROR;
        }
    }

    ctx->checks++;

    return NXT_OK;
}


static nxt_int_t
nxt_h1pf_te(nxt_h1pf_ctx_t *ctx)
{
    u_char              c;
    nxt_int_t           ret;
    nxt_uint_t          b, i, mask;
    nxt_bool_t          match;
    nxt_h1proto_t       h1p;
    nxt_http_field_t    first, second;
    nxt_h1pf_value_t    v;
    nxt_http_request_t  r;

    static const u_char  chunked[] = "chunked";

    static const char  *other[] = {
        "", "chunke", "hunked", "chunkedx", "chunked,", " chunked",
        "chunked ", "chunked;x=1", "x-chunked", "gzip", "gzip, chunked",
        "chunked, gzip", "identity",
    };

    /* Every letter case. */

    v.length = nxt_length("chunked");

    for (mask = 0; mask < ((nxt_uint_t) 1 << nxt_length("chunked")); mask++) {
        for (i = 0; i < v.length; i++) {
            v.data[i] = (mask & ((nxt_uint_t) 1 << i))
                        ? chunked[i] - ('a' - 'A') : chunked[i];
        }

        if (nxt_h1pf_te_check(ctx, &v, 1) != NXT_OK) {
            return NXT_ERROR;
        }
    }

    /* Every byte value in place of each byte. */

    for (i = 0; i < v.length; i++) {
        for (b = 0; b < 256; b++) {
            c = (u_char) b;

            nxt_memcpy(v.data, chunked, v.length);
            v.data[i] = c;

            match = (c == chunked[i] || c == chunked[i] - ('a' - 'A'));

            if (nxt_h1pf_te_check(ctx, &v, match) != NXT_OK) {
                return NXT_ERROR;
            }
        }
    }

    /* Every byte value before and after it. */

    v.length = nxt_length("chunked") + 1;

    for (b = 0; b < 256; b++) {
        v.data[0] = (u_char) b;
        nxt_memcpy(&v.data[1], chunked, nxt_length("chunked"));

        if (nxt_h1pf_te_check(ctx, &v, 0) != NXT_OK) {
            return NXT_ERROR;
        }

        nxt_memcpy(v.data, chunked, nxt_length("chunked"));
        v.data[nxt_length("chunked")] = (u_char) b;

        if (nxt_h1pf_te_check(ctx, &v, 0) != NXT_OK) {
            return NXT_ERROR;
        }
    }

    for (i = 0; i < nxt_nitems(other); i++) {
        nxt_h1pf_value_set(&v, other[i]);

        if (nxt_h1pf_te_check(ctx, &v, 0) != NXT_OK) {
            return NXT_ERROR;
        }
    }

    /* "chunked" in two field lines is a bad request, in any letter case. */

    nxt_memzero(&h1p, sizeof(nxt_h1proto_t));
    nxt_memzero(&r, sizeof(nxt_http_request_t));
    nxt_memzero(&first, sizeof(nxt_http_field_t));
    nxt_memzero(&second, sizeof(nxt_http_field_t));

    r.proto.h1 = &h1p;

    first.value = (u_char *) "Chunked";
    first.value_length = nxt_length("Chunked");
    second.value = (u_char *) "chunked";
    second.value_length = nxt_length("chunked");

    ret = nxt_h1p_test_transfer_encoding(&r, &first);

    if (ret != NXT_OK || r.chunked_field != &first) {
        nxt_log_alert(ctx->thr->log, "h1proto fields test: "
                      "\"Transfer-Encoding: Chunked\" gave %i", ret);
        return NXT_ERROR;
    }

    ret = nxt_h1p_test_transfer_encoding(&r, &second);

    if (ret != NXT_HTTP_BAD_REQUEST) {
        nxt_log_alert(ctx->thr->log, "h1proto fields test: "
                      "\"Chunked\" and \"chunked\" lines gave %i, not 400",
                      ret);
        return NXT_ERROR;
    }

    ctx->checks++;

    return NXT_OK;
}


static int
nxt_h1pf_child(void *data)
{
    u_char          *p;
    nxt_h1pf_ctx_t  *ctx;

    ctx = data;

    /*
     * Three pages: the value ends where the second one starts, or starts
     * where it ends.  The second page cannot be read.
     */

    p = mmap(NULL, 3 * nxt_pagesize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | NXT_MEM_MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        return 2;
    }

    if (mprotect(p + nxt_pagesize, nxt_pagesize, PROT_NONE) != 0) {
        return 2;
    }

    ctx->end = p + nxt_pagesize;
    ctx->start = p + 2 * nxt_pagesize;

    if (nxt_h1pf_conn_cases(ctx) != NXT_OK
        || nxt_h1pf_conn_bytes(ctx) != NXT_OK
        || nxt_h1pf_conn_cases_of_letters(ctx) != NXT_OK
        || nxt_h1pf_conn_random(ctx) != NXT_OK
        || nxt_h1pf_te(ctx) != NXT_OK)
    {
        return 1;
    }

    nxt_log_error(NXT_LOG_NOTICE, ctx->thr->log,
                  "h1proto fields test: %ui checks", ctx->checks);

    return 0;
}


nxt_int_t
nxt_h1proto_fields_test(nxt_thread_t *thr)
{
    int             rc;
    nxt_h1pf_ctx_t  ctx;

    nxt_thread_time_update(thr);

    nxt_memzero(&ctx, sizeof(nxt_h1pf_ctx_t));
    ctx.thr = thr;
    ctx.seed = 0x2545f491;

    rc = nxt_test_in_child(thr, "h1proto fields test", nxt_h1pf_child, &ctx);

    NXT_TEST_CHECK(thr->log, rc == 0,
                   "h1proto fields test failed (child status %d)", rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "h1proto fields test passed");

    return NXT_OK;
}
