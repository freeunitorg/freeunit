
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>


/*
 * The RFC 3492 punycode encoder, and no decoder.  A host match pattern
 * written in Unicode is converted to its ASCII form once, when the
 * configuration is loaded, so that a request Host header, which a client
 * sends in punycode, matches it with the plain byte compare of the
 * router.  Nothing on the request path decodes.
 */

#define NXT_PUNYCODE_BASE          36
#define NXT_PUNYCODE_TMIN          1
#define NXT_PUNYCODE_TMAX          26
#define NXT_PUNYCODE_SKEW          38
#define NXT_PUNYCODE_DAMP          700
#define NXT_PUNYCODE_INITIAL_BIAS  72
#define NXT_PUNYCODE_INITIAL_N     128


static uint32_t nxt_punycode_adapt(uint32_t delta, uint32_t numpoints,
    nxt_bool_t first);
static nxt_bool_t nxt_punycode_is_ascii(const u_char *p, size_t len);


nxt_inline u_char
nxt_punycode_digit(uint32_t d)
{
    return (d < 26) ? 'a' + d : '0' + (d - 26);
}


/*
 * Encodes one UTF-8 label of up to NXT_PUNYCODE_MAX_LABEL code points,
 * without the "xn--" prefix, into dst.  Returns the number of bytes
 * written, or -1 for invalid UTF-8, for more code points than a label
 * may hold, or for an output longer than size.
 *
 * The code point limit keeps the input in a fixed array and makes the
 * RFC 3492 overflow checks unnecessary: delta is at most about
 * 0x110000 * 64, well inside 32 bits.
 */
ssize_t
nxt_punycode_encode(u_char *dst, size_t size, const u_char *src, size_t len)
{
    size_t        i, n, b, h;
    u_char        *p, *end;
    uint32_t      c, m, q, k, t, bias, delta, cp[NXT_PUNYCODE_MAX_LABEL];
    const u_char  *s, *send;

    s = src;
    send = src + len;
    n = 0;

    while (s < send) {
        if (n == nxt_nitems(cp)) {
            return -1;
        }

        c = nxt_utf8_decode(&s, send);
        if (c == 0xFFFFFFFF) {
            return -1;
        }

        cp[n++] = c;
    }

    p = dst;
    end = dst + size;

    for (i = 0; i < n; i++) {
        if (cp[i] < 0x80) {
            if (p == end) {
                return -1;
            }

            *p++ = (u_char) cp[i];
        }
    }

    /* b basic code points were copied; h counts the code points handled. */
    b = p - dst;
    h = b;

    if (b > 0) {
        if (p == end) {
            return -1;
        }

        *p++ = '-';
    }

    c = NXT_PUNYCODE_INITIAL_N;
    bias = NXT_PUNYCODE_INITIAL_BIAS;
    delta = 0;

    while (h < n) {
        m = 0xFFFFFFFF;

        for (i = 0; i < n; i++) {
            if (cp[i] >= c && cp[i] < m) {
                m = cp[i];
            }
        }

        delta += (m - c) * (uint32_t) (h + 1);
        c = m;

        for (i = 0; i < n; i++) {
            if (cp[i] < c) {
                delta++;
            }

            if (cp[i] == c) {
                q = delta;

                for (k = NXT_PUNYCODE_BASE; ; k += NXT_PUNYCODE_BASE) {
                    if (k <= bias) {
                        t = NXT_PUNYCODE_TMIN;

                    } else if (k >= bias + NXT_PUNYCODE_TMAX) {
                        t = NXT_PUNYCODE_TMAX;

                    } else {
                        t = k - bias;
                    }

                    if (q < t) {
                        break;
                    }

                    if (p == end) {
                        return -1;
                    }

                    *p++ = nxt_punycode_digit(t + (q - t)
                                              % (NXT_PUNYCODE_BASE - t));
                    q = (q - t) / (NXT_PUNYCODE_BASE - t);
                }

                if (p == end) {
                    return -1;
                }

                *p++ = nxt_punycode_digit(q);

                bias = nxt_punycode_adapt(delta, (uint32_t) (h + 1), h == b);
                delta = 0;
                h++;
            }
        }

        delta++;
        c++;
    }

    return p - dst;
}


static uint32_t
nxt_punycode_adapt(uint32_t delta, uint32_t numpoints, nxt_bool_t first)
{
    uint32_t  k;

    delta = first ? delta / NXT_PUNYCODE_DAMP : delta / 2;
    delta += delta / numpoints;

    for (k = 0;
         delta > ((NXT_PUNYCODE_BASE - NXT_PUNYCODE_TMIN) * NXT_PUNYCODE_TMAX)
                 / 2;
         k += NXT_PUNYCODE_BASE)
    {
        delta /= NXT_PUNYCODE_BASE - NXT_PUNYCODE_TMIN;
    }

    return k + (NXT_PUNYCODE_BASE - NXT_PUNYCODE_TMIN + 1) * delta
               / (delta + NXT_PUNYCODE_SKEW);
}


static nxt_bool_t
nxt_punycode_is_ascii(const u_char *p, size_t len)
{
    while (len != 0) {
        if (*p++ >= 0x80) {
            return 0;
        }

        len--;
    }

    return 1;
}


/*
 * Converts each non-ASCII label of a host match pattern to punycode with
 * the "xn--" prefix.  The labels are split at '.', so a '*' wildcard may be
 * a label of its own or sit in an ASCII label, but not in a label with
 * non-ASCII characters: the punycode of part of a label is not part of the
 * punycode of the label.  A label already in punycode, an empty label and
 * a trailing dot pass through unchanged.  Only ASCII case is folded, by the
 * caller: a pattern must be written in lowercase Unicode, the form that a
 * browser sends.
 *
 * Returns NXT_OK and leaves *host alone for an ASCII pattern, NXT_OK with
 * *host replaced by a copy from mp, or NXT_ERROR with *error set.
 */
nxt_int_t
nxt_punycode_host(nxt_mp_t *mp, nxt_str_t *host, const char **error)
{
    size_t      i, n;
    ssize_t     len;
    u_char      *p, *dst, *dot, *start, *end;
    nxt_bool_t  ascii;

    n = 0;
    ascii = 1;

    for (i = 0; i < host->length; i++) {
        if (host->start[i] >= 0x80) {
            ascii = 0;

        } else if (host->start[i] == '.') {
            n += !ascii;
            ascii = 1;
        }
    }

    n += !ascii;

    if (n == 0) {
        return NXT_OK;
    }

    /* Each of the n converted labels is at most NXT_PUNYCODE_MAX_LABEL. */
    dst = nxt_mp_nget(mp, host->length + n * NXT_PUNYCODE_MAX_LABEL);
    if (nxt_slow_path(dst == NULL)) {
        *error = "memory allocation failed";
        return NXT_ERROR;
    }

    p = dst;
    start = host->start;
    end = start + host->length;

    for ( ;; ) {
        dot = memchr(start, '.', end - start);
        len = ((dot != NULL) ? dot : end) - start;

        if (nxt_punycode_is_ascii(start, len)) {
            p = nxt_cpymem(p, start, len);

        } else {
            if (memchr(start, '*', len) != NULL) {
                *error = "a \"*\" cannot be in a label with non-ASCII "
                         "characters";
                return NXT_ERROR;
            }

            if (!nxt_utf8_is_valid(start, len)) {
                *error = "invalid UTF-8";
                return NXT_ERROR;
            }

            p = nxt_cpymem(p, "xn--", 4);

            len = nxt_punycode_encode(p, NXT_PUNYCODE_MAX_LABEL - 4, start, len);
            if (len < 0) {
                *error = "a label is longer than 63 characters in punycode";
                return NXT_ERROR;
            }

            p += len;
        }

        if (dot == NULL) {
            break;
        }

        *p++ = '.';
        start = dot + 1;
    }

    host->start = dst;
    host->length = p - dst;

    return NXT_OK;
}
