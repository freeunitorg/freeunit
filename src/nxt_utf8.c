
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>


static uint32_t nxt_utf8_decode2(const u_char **start, const u_char *end);


u_char *
nxt_utf8_encode(u_char *p, uint32_t u)
{
    if (u < 0x80) {
        *p++ = (u_char) (u & 0xFF);
        return p;
    }

    if (u < 0x0800) {
        *p++ = (u_char) (( u >> 6)          | 0xC0);
        *p++ = (u_char) (( u        & 0x3F) | 0x80);
        return p;
    }

    if (u < 0x10000) {
        *p++ = (u_char) ( (u >> 12)         | 0xE0);
        *p++ = (u_char) (((u >>  6) & 0x3F) | 0x80);
        *p++ = (u_char) (( u        & 0x3F) | 0x80);
        return p;
    }

    if (u < 0x110000) {
        *p++ = (u_char) ( (u >> 18)         | 0xF0);
        *p++ = (u_char) (((u >> 12) & 0x3F) | 0x80);
        *p++ = (u_char) (((u >>  6) & 0x3F) | 0x80);
        *p++ = (u_char) (( u        & 0x3F) | 0x80);
        return p;
    }

    return NULL;
}


/*
 * nxt_utf8_decode() decodes UTF-8 sequences and returns a valid
 * character 0x00 - 0x10FFFF, or 0xFFFFFFFF for invalid or overlong
 * UTF-8 sequence.
 */

uint32_t
nxt_utf8_decode(const u_char **start, const u_char *end)
{
    uint32_t  u;

    u = (uint32_t) **start;

    if (u < 0x80) {
        (*start)++;
        return u;
    }

    return nxt_utf8_decode2(start, end);
}


/*
 * nxt_utf8_decode2() decodes two and more bytes UTF-8 sequences only
 * and returns a valid character 0x80 - 0x10FFFF, or 0xFFFFFFFF for
 * invalid or overlong UTF-8 sequence.
 */

static uint32_t
nxt_utf8_decode2(const u_char **start, const u_char *end)
{
    u_char        c;
    size_t        n;
    uint32_t      u, overlong;
    const u_char  *p;

    p = *start;
    u = (uint32_t) *p;

    if (u >= 0xE0) {

        if (u >= 0xF0) {

            if (nxt_slow_path(u > 0xF4)) {
                /*
                 * The maximum valid Unicode character is 0x10FFFF
                 * which is encoded as 0xF4 0x8F 0xBF 0xBF.
                 */
                return 0xFFFFFFFF;
            }

            u &= 0x07;
            overlong = 0x00FFFF;
            n = 3;

        } else {
            u &= 0x0F;
            overlong = 0x07FF;
            n = 2;
        }

    } else if (u >= 0xC2) {

        /* 0x80 is encoded as 0xC2 0x80. */

        u &= 0x1F;
        overlong = 0x007F;
        n = 1;

    } else {
        /* u <= 0xC2 */
        return 0xFFFFFFFF;
    }

    p++;

    if (nxt_fast_path(p + n <= end)) {

        do {
            c = *p++;
            /*
             * The byte must in the 0x80 - 0xBF range.
             * Values below 0x80 become >= 0x80.
             */
            c = c - 0x80;

            if (nxt_slow_path(c > 0x3F)) {
                return 0xFFFFFFFF;
            }

            u = (u << 6) | c;
            n--;

        } while (n != 0);

        /*
         * Shortest form, inside the Unicode range, and not a surrogate:
         * U+D800-U+DFFF exist only to be paired inside UTF-16 and have no
         * UTF-8 encoding at all (Unicode 15.0 Sect. 3.9, D92), so a decoder
         * that returns them hands its caller a code point that cannot be
         * re-encoded -- and, for anything that then writes the bytes back
         * out, output no strict reader will take.
         */

        if (overlong < u && u < 0x110000
            && !(u >= 0xD800 && u <= 0xDFFF))
        {
            *start = p;
            return u;
        }
    }

    return 0xFFFFFFFF;
}


ssize_t
nxt_utf8_length(const u_char *p, size_t len)
{
    ssize_t       length;
    const u_char  *end;

    length = 0;

    end = p + len;

    while (p < end) {
        if (nxt_slow_path(nxt_utf8_decode(&p, end) == 0xFFFFFFFF)) {
            return -1;
        }

        length++;
    }

    return length;
}


/*
 * Copy "src" into the pool, replacing every byte that begins no valid UTF-8
 * sequence with U+FFFD.  Returns "src" itself when there is nothing to
 * replace, so the ordinary path neither allocates nor copies.
 *
 * The replacement is per byte rather than per maximal subpart (Unicode 15.0
 * Sect. 3.9): a truncated four-byte sequence therefore yields up to four
 * U+FFFD where a maximal-subpart resync yields one.  That changes how many
 * replacement characters a reader sees, never whether the result is valid,
 * and it keeps the worst-case expansion at the three bytes of U+FFFD per
 * input byte -- which matters when the input is a request header somebody
 * else chose.
 */

nxt_int_t
nxt_utf8_sanitize(nxt_mp_t *mp, nxt_str_t *dst, const nxt_str_t *src)
{
    size_t        len;
    u_char        *d, *out;
    const u_char  *p, *seq, *end, *start;

    /*
     * "dst" and "src" may be the same nxt_str_t -- the access log sanitizes
     * a value in place -- so the input is read through locals captured here
     * and "dst" is not written until both passes are done.
     */

    start = src->start;
    end = start + src->length;

    p = start;
    len = 0;

    while (p < end) {

        /*
         * nxt_utf8_decode() is a call into another translation unit, and a
         * logged value is mostly ASCII, so the single-byte case is decided
         * here rather than paid for at that price once per character.
         */

        if (nxt_fast_path(*p < 0x80)) {
            p++;
            len++;

            continue;
        }

        seq = p;

        if (nxt_utf8_decode(&seq, end) == 0xFFFFFFFF) {
            len += nxt_length("\xEF\xBF\xBD");
            p++;

            continue;
        }

        len += seq - p;
        p = seq;
    }

    if (nxt_fast_path(len == src->length)) {
        *dst = *src;

        return NXT_OK;
    }

    out = nxt_mp_nget(mp, len);
    if (nxt_slow_path(out == NULL)) {
        return NXT_ERROR;
    }

    d = out;
    p = start;

    while (p < end) {

        if (nxt_fast_path(*p < 0x80)) {
            *d++ = *p++;

            continue;
        }

        seq = p;

        if (nxt_utf8_decode(&seq, end) == 0xFFFFFFFF) {
            *d++ = 0xEF; *d++ = 0xBF; *d++ = 0xBD;
            p++;

            continue;
        }

        d = nxt_cpymem(d, p, seq - p);
        p = seq;
    }

    dst->start = out;
    dst->length = len;

    return NXT_OK;
}


nxt_bool_t
nxt_utf8_is_valid(const u_char *p, size_t len)
{
    const u_char  *end;

    end = p + len;

    while (p < end) {
        if (nxt_slow_path(nxt_utf8_decode(&p, end) == 0xFFFFFFFF)) {
            return 0;
        }
    }

    return 1;
}
