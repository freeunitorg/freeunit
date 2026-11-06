
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_STRING_H_INCLUDED_
#define _NXT_STRING_H_INCLUDED_


#define nxt_lowcase(c)                                                        \
    (u_char) ((c >= 'A' && c <= 'Z') ? c | 0x20 : c)

#define nxt_upcase(c)                                                         \
    (u_char) ((c >= 'a' && c <= 'z') ? c & ~0x20 : c)

#define nxt_isdigit(c)                                                        \
    ((u_char) ((c) - '0') <= 9)

#define nxt_strtod(s, endptr)                                                 \
    strtod((char *) s, (char **) endptr)


#define nxt_strlen(s)                                                         \
    strlen((char *) s)


#define nxt_strdup(s)                                                         \
    strdup((char *) s)


#define nxt_strchr(buf, delim)                                                \
    (u_char *) strchr((char *) buf, delim)


#define nxt_memzero(buf, length)                                              \
    (void) memset(buf, 0, length)


#define nxt_memset(buf, c, length)                                            \
    (void) memset(buf, c, length)


#define nxt_memcpy(dst, src, length)                                          \
    (void) memcpy(dst, src, length)


NXT_EXPORT void nxt_memcpy_lowcase(u_char *dst, const u_char *src,
    size_t length);
NXT_EXPORT void nxt_memcpy_upcase(u_char *dst, const u_char *src,
    size_t length);


/*
 * nxt_cpymem() is an inline function but not a macro to
 * eliminate possible double evaluation of length "length".
 */
nxt_inline void *
nxt_cpymem(void *dst, const void *src, size_t length)
{
    /*
     * memcpy() declares "src" non-null even for a zero-length copy, and
     * callers reach here with the NULL start of an empty nxt_str_t.  The
     * copy has nothing to do in that case and dst + 0 is dst.
     */
    if (length == 0) {
        return dst;
    }

    return memcpy(dst, src, length) + length;
}


#define nxt_memmove(dst, src, length)                                         \
    (void) memmove(dst, src, length)


#define nxt_strcmp(s1, s2)                                                    \
    strcmp((char *) s1, (char *) s2)


#define nxt_strncmp(s1, s2, length)                                           \
    strncmp((char *) s1, (char *) s2, length)


NXT_EXPORT u_char *nxt_cpystr(u_char *dst, const u_char *src);
NXT_EXPORT u_char *nxt_cpystrn(u_char *dst, const u_char *src, size_t length);
NXT_EXPORT nxt_int_t nxt_strcasecmp(const u_char *s1, const u_char *s2);
NXT_EXPORT nxt_int_t nxt_strncasecmp(const u_char *s1, const u_char *s2,
    size_t length);
NXT_EXPORT nxt_int_t nxt_memcasecmp(const void *p1, const void *p2,
    size_t length);


/*
 * Case-insensitive equality of two byte strings of the same known length.
 * The result is that of nxt_memcasecmp(p, s, length) == 0 for every input:
 * any byte values, NUL and 0x80 to 0xFF included, and the length 0.
 *
 * The functions below load words of 8, 4 and 2 bytes.  They read exactly
 * "length" bytes of each string and no byte outside them.  A NUL does not
 * stop them.  So "length" must be the known length of both strings.  Never
 * pass a NUL-terminated string whose length is not known: the loads do not
 * stop at its NUL.
 *
 * The byte order does not change a result.  Each word is loaded with
 * memcpy() in the byte order of the host, and the constant and the mask are
 * loaded the same way as the input.  The mask of a byte depends on that
 * byte only, and a compare of two words is a compare of their bytes in
 * pairs.
 */


/* Loads at any address.  memcpy() makes one load where the target allows. */

nxt_inline uint64_t
nxt_load64(const u_char *p)
{
    uint64_t  w;

    memcpy(&w, p, 8);

    return w;
}


nxt_inline uint32_t
nxt_load32(const u_char *p)
{
    uint32_t  w;

    memcpy(&w, p, 4);

    return w;
}


nxt_inline uint16_t
nxt_load16(const u_char *p)
{
    uint16_t  w;

    memcpy(&w, p, 2);

    return w;
}


/* The byte "c" in each of the 8 bytes of a word. */

#define nxt_word_bytes(c)  ((uint64_t) (c) * 0x0101010101010101ULL)


/*
 * The fold mask of a word: 0x20 in each byte that is an ASCII letter, 'A'
 * to 'Z' or 'a' to 'z', and 0x00 in each other byte.  "h" holds each byte
 * with bit 5 set and bit 7 clear, so both cases of a letter give its lower
 * case, and each sum below stays under 0x100: no carry goes into the next
 * byte.  Bit 7 of the first sum is set from 'a' up, and bit 7 of the second
 * sum is set from '{' up, so the XOR keeps 'a' to 'z'.  "& ~w" then clears
 * the bytes 0x80 to 0xFF, because their low 7 bits can be a letter.
 */

nxt_inline uint64_t
nxt_word_alpha(uint64_t w)
{
    uint64_t  h;

    h = (w | nxt_word_bytes(0x20)) & nxt_word_bytes(0x7f);

    return (((h + nxt_word_bytes(0x80 - 'a'))
             ^ (h + nxt_word_bytes(0x80 - 'z' - 1)))
            & ~w & nxt_word_bytes(0x80)) >> 2;
}


nxt_inline nxt_bool_t
nxt_memcase_eq_word(const u_char *p, const u_char *s, size_t i)
{
    uint64_t  t, m;

    t = nxt_load64(s + i);
    m = nxt_word_alpha(t);

    return ((nxt_load64(p + i) | m) == (t | m));
}


/*
 * nxt_memcase_eq() takes the mask from "s" only.  A letter of "s" matches
 * only the two cases of that letter: (b | 0x20) == (l | 0x20).  Any other
 * byte of "s" matches only itself, because no letter folds to a byte that
 * is not a letter.  That is nxt_lowcase() on both sides.
 *
 * From 8 bytes on, it compares 8-byte words.  When the length is not a
 * multiple of 8, the last word ends at the last byte and overlaps the word
 * before it.  The words before the last one are written out up to 32
 * bytes, and a loop takes the words after them.  GCC and Clang do not
 * unroll that loop at -O, and in a loop the mask of a constant is computed
 * at run time.  Below 8 bytes, two overlapping loads of 4 or of 2 bytes
 * make one word.
 *
 * With a string literal (nxt_memcase_eq_lit()), the compiler computes the
 * masks and the folded words at compile time.  A word then costs one load,
 * one OR and one compare with constants.  With any other "s" the result is
 * the same, and the masks are computed at run time.
 */

nxt_inline nxt_bool_t
nxt_memcase_eq(const u_char *p, const u_char *s, size_t length)
{
    size_t    i;
    uint64_t  w, t, m;

    if (length >= 8) {
        if (length > 8 && !nxt_memcase_eq_word(p, s, 0)) {
            return 0;
        }

        if (length > 16 && !nxt_memcase_eq_word(p, s, 8)) {
            return 0;
        }

        if (length > 24 && !nxt_memcase_eq_word(p, s, 16)) {
            return 0;
        }

        for (i = 24; i + 8 < length; i += 8) {
            if (!nxt_memcase_eq_word(p, s, i)) {
                return 0;
            }
        }

        return nxt_memcase_eq_word(p, s, length - 8);
    }

    if (length >= 4) {
        w = nxt_load32(p) | (uint64_t) nxt_load32(p + length - 4) << 32;
        t = nxt_load32(s) | (uint64_t) nxt_load32(s + length - 4) << 32;

    } else if (length >= 2) {
        w = nxt_load16(p) | (uint64_t) nxt_load16(p + length - 2) << 16;
        t = nxt_load16(s) | (uint64_t) nxt_load16(s + length - 2) << 16;

    } else if (length == 1) {
        w = p[0];
        t = s[0];

    } else {
        return 1;
    }

    m = nxt_word_alpha(t);

    return ((w | m) == (t | m));
}


/*
 * "" s "" accepts a string literal only: nxt_length() of a pointer is the
 * size of the pointer minus one, not the length of the string.
 */

#define nxt_memcase_eq_lit(p, s)                                              \
    nxt_memcase_eq(p, (const u_char *) "" s "", nxt_length("" s ""))


/*
 * A stored pattern for nxt_memcase_eq_fold(): nxt_casefold_set() writes the
 * "length" bytes of "src" in lower case to "low", and after them "length"
 * mask bytes, 0x20 for a letter and 0x00 for any other byte.  So "low" must
 * have room for 2 * "length" bytes.
 */

NXT_EXPORT void nxt_casefold_set(u_char *low, const u_char *src,
    size_t length);


nxt_inline nxt_bool_t
nxt_memcase_eq_fold_word(const u_char *p, const u_char *low,
    const u_char *fold, size_t i)
{
    return ((nxt_load64(p + i) | nxt_load64(fold + i))
            == nxt_load64(low + i));
}


/*
 * nxt_memcase_eq_fold() is nxt_memcase_eq() against a stored pattern.  It
 * reads "length" bytes at "p" and 2 * "length" bytes at "low".  The words
 * are taken as in nxt_memcase_eq(), and a word costs three loads, one OR and
 * one compare: the mask is not computed.
 */

nxt_inline nxt_bool_t
nxt_memcase_eq_fold(const u_char *p, const u_char *low, size_t length)
{
    size_t        i;
    uint64_t      w, t, m;
    const u_char  *fold;

    /* With the length 0 no byte of "low" is read, and "low" may be NULL. */

    if (length >= 8) {
        fold = low + length;

        if (length > 8 && !nxt_memcase_eq_fold_word(p, low, fold, 0)) {
            return 0;
        }

        if (length > 16 && !nxt_memcase_eq_fold_word(p, low, fold, 8)) {
            return 0;
        }

        if (length > 24 && !nxt_memcase_eq_fold_word(p, low, fold, 16)) {
            return 0;
        }

        for (i = 24; i + 8 < length; i += 8) {
            if (!nxt_memcase_eq_fold_word(p, low, fold, i)) {
                return 0;
            }
        }

        return nxt_memcase_eq_fold_word(p, low, fold, length - 8);
    }

    if (length >= 4) {
        fold = low + length;

        w = nxt_load32(p) | (uint64_t) nxt_load32(p + length - 4) << 32;
        m = nxt_load32(fold) | (uint64_t) nxt_load32(fold + length - 4) << 32;
        t = nxt_load32(low) | (uint64_t) nxt_load32(low + length - 4) << 32;

    } else if (length >= 2) {
        fold = low + length;

        w = nxt_load16(p) | (uint64_t) nxt_load16(p + length - 2) << 16;
        m = nxt_load16(fold) | (uint64_t) nxt_load16(fold + length - 2) << 16;
        t = nxt_load16(low) | (uint64_t) nxt_load16(low + length - 2) << 16;

    } else if (length == 1) {
        w = p[0];
        m = low[1];
        t = low[0];

    } else {
        return 1;
    }

    return ((w | m) == t);
}


NXT_EXPORT u_char *nxt_memstrn(const u_char *s, const u_char *end,
    const char *ss, size_t length);
NXT_EXPORT u_char *nxt_memcasestrn(const u_char *s, const u_char *end,
    const char *ss, size_t length);
NXT_EXPORT u_char *nxt_rmemstrn(const u_char *s, const u_char *end,
    const char *ss, size_t length);
NXT_EXPORT size_t nxt_str_strip(const u_char *start, const u_char *end);


typedef struct {
    size_t                    length;
    u_char                    *start;
} nxt_str_t;


#define nxt_string(str)       { nxt_length(str), (u_char *) str }
#define nxt_string_zero(str)  { sizeof(str), (u_char *) str }
#define nxt_null_string       { 0, NULL }


#define nxt_str_set(str, text)                                                \
    do {                                                                      \
        (str)->length = nxt_length(text);                                     \
        (str)->start = (u_char *) text;                                       \
    } while (0)


#define nxt_str_null(str)                                                     \
    do {                                                                      \
        (str)->length = 0;                                                    \
        (str)->start = NULL;                                                  \
    } while (0)


NXT_EXPORT nxt_str_t *nxt_str_alloc(nxt_mp_t *mp, size_t length);
NXT_EXPORT nxt_str_t *nxt_str_dup(nxt_mp_t *mp, nxt_str_t *dst,
    const nxt_str_t *src);
NXT_EXPORT char *nxt_str_cstrz(nxt_mp_t *mp, const nxt_str_t *src);


/*
 * memcmp() declares both pointers non-null whatever the length, and a
 * zero-length nxt_str_t carries a NULL start, so the length test has to
 * short-circuit the call rather than only decide its result.  Two empty
 * strings are equal, which is what falling through to "true" gives.
 */
#define nxt_strstr_eq(s1, s2)                                                 \
    (((s1)->length == (s2)->length)                                           \
      && ((s1)->length == 0                                                   \
          || memcmp((s1)->start, (s2)->start, (s1)->length) == 0))


#define nxt_strcasestr_eq(s1, s2)                                             \
    (((s1)->length == (s2)->length)                                           \
      && (nxt_memcasecmp((s1)->start, (s2)->start, (s1)->length) == 0))


/* The same zero-length short circuit as nxt_strstr_eq(), for memcmp(). */

#define nxt_str_eq(s, p, _length)                                             \
    (((s)->length == _length)                                                 \
      && ((s)->length == 0 || memcmp((s)->start, p, _length) == 0))


#define nxt_str_start(s, p, _length)                                          \
    (((s)->length >= _length)                                                 \
      && ((_length) == 0 || memcmp((s)->start, p, _length) == 0))


#define nxt_strchr_eq(s, c)                                                   \
    (((s)->length == 1) && ((s)->start[0] == c))


#define nxt_strchr_start(s, c)                                                \
    (((s)->length != 0) && ((s)->start[0] == c))


NXT_EXPORT nxt_int_t nxt_strverscmp(const u_char *s1, const u_char *s2);
NXT_EXPORT nxt_bool_t nxt_strvers_match(u_char *version, u_char *prefix,
    size_t length);

NXT_EXPORT u_char *nxt_decode_uri(u_char *dst, u_char *src, size_t length);
NXT_EXPORT u_char *nxt_decode_uri_plus(u_char *dst, u_char *src, size_t length);
NXT_EXPORT uintptr_t nxt_encode_uri(u_char *dst, u_char *src, size_t length);
NXT_EXPORT uintptr_t nxt_encode_complex_uri(u_char *dst, u_char *src,
    size_t length);
NXT_EXPORT nxt_bool_t nxt_is_complex_uri_encoded(u_char *s, size_t length);

NXT_EXPORT ssize_t nxt_base64_decode(u_char *dst, u_char *src, size_t length);

extern const uint8_t  nxt_hex2int[256];


#endif /* _NXT_STRING_H_INCLUDED_ */
