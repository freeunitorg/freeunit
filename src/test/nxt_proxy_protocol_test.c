/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_main.h>
#include "nxt_tests.h"


/*
 * nxt_proxy_protocol_parse() reads the PROXY protocol header that a proxy
 * sends at the start of a connection.  The router drops the connection
 * on NXT_ERROR and waits for more bytes on NXT_AGAIN, so the table pins
 * three things for every shape: the verdict, the exact length of a whole
 * header, and that a truncated header waits while a header that can never
 * become valid, or that claims a length above the cap, fails at once.
 *
 * The v2 inputs are written with octal escapes, because a hex escape
 * swallows the hex-like characters after it.
 */

#define NXT_PP2_SIG     "\r\n\r\n\0\r\nQUIT\n"

#define NXT_PP2_INET                                                          \
    "\001\002\003\004" "\005\006\007\010" "\022\064" "\000\120"
#define NXT_PP2_INET6                                                         \
    "\040\001\015\270\000\000\000\000\000\000\000\000\000\000\000\001"        \
    "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\001"        \
    "\001\273" "\000\120"


typedef struct {
    nxt_uint_t  version;
    nxt_str_t   input;
    nxt_int_t   ret;
    size_t      length;
    uint8_t     local;
    uint8_t     tls;
    nxt_str_t   remote;
} nxt_proxy_protocol_test_case_t;


static const nxt_proxy_protocol_test_case_t  nxt_proxy_protocol_tests[] = {

    /* Version 2, valid. */

    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\021\000\014" NXT_PP2_INET),
      NXT_OK, 28, 0, 0, nxt_string("1.2.3.4:4660") },

#if (NXT_INET6)
    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\041\000\044" NXT_PP2_INET6),
      NXT_OK, 52, 0, 0, nxt_string("[2001:db8::1]:443") },
#endif

    /* The header is followed by a request; only the header is counted. */
    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\021\000\014" NXT_PP2_INET
                 "GET / HTTP/1.1\r\n"),
      NXT_OK, 28, 0, 0, nxt_string("1.2.3.4:4660") },

    /* PP2_TYPE_SSL with PP2_CLIENT_SSL. */
    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\021\000\024" NXT_PP2_INET
                 "\040\000\005" "\001\000\000\000\000"),
      NXT_OK, 36, 0, 1, nxt_string("1.2.3.4:4660") },

    /* PP2_TYPE_SSL without the client bit. */
    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\021\000\024" NXT_PP2_INET
                 "\040\000\005" "\002\000\000\000\000"),
      NXT_OK, 36, 0, 0, nxt_string("1.2.3.4:4660") },

    /* A NOOP TLV, then SSL with sub-TLVs, then an unknown type. */
    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\021\000\046" NXT_PP2_INET
                 "\004\000\003" "abc"
                 "\040\000\014" "\001\000\000\000\000" "\041\000\004" "TLSv"
                 "\360\000\002" "xy"),
      NXT_OK, 54, 0, 1, nxt_string("1.2.3.4:4660") },

    /* LOCAL: the address block is skipped whatever it holds. */
    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\040\000\000\000"),
      NXT_OK, 16, 1, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\040\061\000\003" "xyz"),
      NXT_OK, 19, 1, 0, nxt_null_string },

    /* Version 2, incomplete. */

    { NXT_PROXY_PROTOCOL_V2, nxt_string(""), NXT_AGAIN, 0, 0, 0,
      nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2, nxt_string("\r\n\r\n\0"), NXT_AGAIN, 0, 0, 0,
      nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2, nxt_string(NXT_PP2_SIG "\041\021\000"),
      NXT_AGAIN, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\021\000\014" "\001\002\003\004"),
      NXT_AGAIN, 0, 0, 0, nxt_null_string },

    /* A declared length at the cap waits; one byte above it fails. */
    { NXT_PROXY_PROTOCOL_V2, nxt_string(NXT_PP2_SIG "\041\021\003\360"),
      NXT_AGAIN, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2, nxt_string(NXT_PP2_SIG "\041\021\003\361"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2, nxt_string(NXT_PP2_SIG "\040\000\377\377"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* Version 2, malformed. */

    /* Not the signature: a request, a ClientHello, a v1 line. */
    { NXT_PROXY_PROTOCOL_V2, nxt_string("GET / HTTP/1.1\r\n\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2, nxt_string("\026\003\001\002\000\001\000"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2,
      nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 4660 80\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* The last signature byte differs. */
    { NXT_PROXY_PROTOCOL_V2, nxt_string("\r\n\r\n\0\r\nQUIT\r"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* Version nibble 1 and 3. */
    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\021\021\000\014" NXT_PP2_INET),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\061\021\000\014" NXT_PP2_INET),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* Command 2. */
    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\042\021\000\014" NXT_PP2_INET),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* PROXY with AF_UNSPEC, AF_UNIX, and UDP. */
    { NXT_PROXY_PROTOCOL_V2, nxt_string(NXT_PP2_SIG "\041\000\000\000"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2, nxt_string(NXT_PP2_SIG "\041\061\000\330"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\022\000\014" NXT_PP2_INET),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\042\000\044" NXT_PP2_INET6),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* The length is too short for the family. */
    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\021\000\013" NXT_PP2_INET),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\041\000\014" NXT_PP2_INET6),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* TLV bytes that do not fit: a cut header, an overrun, a short SSL. */
    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\021\000\016" NXT_PP2_INET "\040\000"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\021\000\017" NXT_PP2_INET "\004\000\005"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V2,
      nxt_string(NXT_PP2_SIG "\041\021\000\021" NXT_PP2_INET
                 "\040\000\002" "\001\000"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* Version 1, valid. */

    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\r\n"),
      NXT_OK, 47, 0, 0, nxt_string("192.168.0.1:56324") },

#if (NXT_INET6)
    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP6 2001:db8::1 ::1 65535 1\r\n"),
      NXT_OK, 36, 0, 0, nxt_string("[2001:db8::1]:65535") },
#endif

    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 0 80\r\nGET / HTTP/1.1\r\n"),
      NXT_OK, 33, 0, 0, nxt_string("1.2.3.4:0") },

#if (NXT_INET6)
    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP6 ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff "
                 "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff 65535 65535\r\n"),
      NXT_OK, 104, 0, 0,
      nxt_string("[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:65535") },
#endif

    /* The longest TCP4 line of the specification. */
    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP4 255.255.255.255 255.255.255.255 65535 65535\r\n"),
      NXT_OK, 56, 0, 0, nxt_string("255.255.255.255:65535") },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 0.0.0.0 0.0.0.0 1 2\r\n"),
      NXT_OK, 32, 0, 0, nxt_string("0.0.0.0:1") },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY UNKNOWN\r\n"),
      NXT_OK, 15, 1, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY UNKNOWN ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff "
                 "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff 65535 65535\r\n"),
      NXT_OK, 107, 1, 0, nxt_null_string },

    /* Version 1, incomplete. */

    { NXT_PROXY_PROTOCOL_V1, nxt_string(""), NXT_AGAIN, 0, 0, 0,
      nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY"), NXT_AGAIN, 0, 0, 0,
      nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2."), NXT_AGAIN, 0, 0, 0,
      nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 1 2\r"),
      NXT_AGAIN, 0, 0, 0, nxt_null_string },

    /* Version 1, malformed. */

    { NXT_PROXY_PROTOCOL_V1, nxt_string("GET / HTTP/1.1\r\n\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("\026\003\001\002\000\001\000"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1,
      nxt_string(NXT_PP2_SIG "\041\021\000\014" NXT_PP2_INET),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXZ"), NXT_ERROR, 0, 0, 0,
      nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("proxy TCP4 1.2.3.4 5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY tcp4 1.2.3.4 5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY UDP4 1.2.3.4 5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY UNKNOWNX\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* 107 bytes without a CRLF can never become a header. */
    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP4 111111111111111111111111111111111111111111111111"
                 "111111111111111111111111111111111111111111111111"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* 106 bytes without a CRLF may still get one. */
    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP4 111111111111111111111111111111111111111111111111"
                 "11111111111111111111111111111111111111111111111"),
      NXT_AGAIN, 0, 0, 0, nxt_null_string },

    /* Field count. */
    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 1\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 1 2 3\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* Separators: two spaces, a trailing space, a tab. */
    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.4  5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 1 2 \r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.4\t5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* Addresses of the wrong shape or family. */
    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3 5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.4 5.6.7.256 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* An octet with a leading zero. */
    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 010.0.0.1 5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.4. 5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1..3.4 5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.1000 5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 ::1 5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP6 1.2.3.4 ::1 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP6 ::1 2001:db8:::1 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* Ports. */
    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 65536 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 1 99999\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 01 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 1a 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1, nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 -1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    { NXT_PROXY_PROTOCOL_V1,
      nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 123456 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },

    /* An unknown version accepts nothing. */
    { NXT_PROXY_PROTOCOL_OFF,
      nxt_string("PROXY TCP4 1.2.3.4 5.6.7.8 1 2\r\n"),
      NXT_ERROR, 0, 0, 0, nxt_null_string },
};


static nxt_int_t
nxt_proxy_protocol_test_remote(nxt_thread_t *thr, nxt_mp_t *mp,
    const nxt_proxy_protocol_test_case_t *t, nxt_proxy_protocol_t *pp)
{
    size_t          address_length;
    socklen_t       socklen;
    nxt_str_t       text;
    nxt_sockaddr_t  *sa;

    NXT_TEST_CHECK(thr->log,
                   nxt_proxy_protocol_sockaddr_size(pp, &socklen,
                                                    &address_length)
                   == NXT_OK,
                   "proxy protocol test \"%V\": no sockaddr size for family "
                   "%ui", &t->input, (nxt_uint_t) pp->family);

    sa = nxt_sockaddr_alloc(mp, socklen, address_length);
    NXT_TEST_CHECK(thr->log, sa != NULL, "proxy protocol test: no memory");

    nxt_proxy_protocol_sockaddr_set(pp, sa);
    nxt_sockaddr_text(sa);

    text.start = nxt_sockaddr_start(sa);
    text.length = sa->length;

    NXT_TEST_CHECK(thr->log, nxt_strstr_eq(&text, &t->remote),
                   "proxy protocol test \"%V\": remote \"%V\" expected \"%V\"",
                   &t->input, &text, &t->remote);

    NXT_TEST_CHECK(thr->log, sa->u.sockaddr.sa_family == pp->family,
                   "proxy protocol test \"%V\": sockaddr family %ui "
                   "expected %ui", &t->input,
                   (nxt_uint_t) sa->u.sockaddr.sa_family,
                   (nxt_uint_t) pp->family);

    return NXT_OK;
}


static nxt_int_t
nxt_proxy_protocol_test_case(nxt_thread_t *thr, nxt_mp_t *mp,
    const nxt_proxy_protocol_test_case_t *t)
{
    nxt_int_t             ret;
    nxt_proxy_protocol_t  pp;

    /* A pattern that a parse must not leave in a used field. */
    nxt_memset(&pp, 0xA5, sizeof(nxt_proxy_protocol_t));

    ret = nxt_proxy_protocol_parse(t->version, t->input.start,
                                   t->input.length, &pp);

    NXT_TEST_CHECK(thr->log, ret == t->ret,
                   "proxy protocol v%ui test \"%V\" (%uz bytes): "
                   "%i expected %i",
                   t->version, &t->input, t->input.length, ret, t->ret);

    if (ret != NXT_OK) {
        return NXT_OK;
    }

    NXT_TEST_CHECK(thr->log, pp.length == t->length,
                   "proxy protocol test \"%V\": length %uz expected %uz",
                   &t->input, pp.length, t->length);

    NXT_TEST_CHECK(thr->log, pp.local == t->local,
                   "proxy protocol test \"%V\": local %ui expected %ui",
                   &t->input, (nxt_uint_t) pp.local, (nxt_uint_t) t->local);

    NXT_TEST_CHECK(thr->log, pp.tls == t->tls,
                   "proxy protocol test \"%V\": tls %ui expected %ui",
                   &t->input, (nxt_uint_t) pp.tls, (nxt_uint_t) t->tls);

    if (t->local) {
        return NXT_OK;
    }

    return nxt_proxy_protocol_test_remote(thr, mp, t, &pp);
}


nxt_int_t
nxt_proxy_protocol_test(nxt_thread_t *thr)
{
    nxt_mp_t    *mp;
    nxt_uint_t  i;

    static const struct {
        nxt_str_t   name;
        nxt_uint_t  version;
    } versions[] = {
        { nxt_string("v1"),  NXT_PROXY_PROTOCOL_V1 },
        { nxt_string("v2"),  NXT_PROXY_PROTOCOL_V2 },
        { nxt_string("off"), NXT_PROXY_PROTOCOL_OFF },
        { nxt_string("V1"),  NXT_PROXY_PROTOCOL_OFF },
        { nxt_string("v3"),  NXT_PROXY_PROTOCOL_OFF },
        { nxt_string(""),    NXT_PROXY_PROTOCOL_OFF },
    };

    for (i = 0; i < nxt_nitems(versions); i++) {
        NXT_TEST_CHECK(thr->log,
                       nxt_proxy_protocol_version(&versions[i].name)
                       == versions[i].version,
                       "proxy protocol version \"%V\" test failed",
                       &versions[i].name);
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    for (i = 0; i < nxt_nitems(nxt_proxy_protocol_tests); i++) {
        if (nxt_proxy_protocol_test_case(thr, mp, &nxt_proxy_protocol_tests[i])
            != NXT_OK)
        {
            nxt_mp_destroy(mp);
            return NXT_ERROR;
        }
    }

    nxt_mp_destroy(mp);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "proxy protocol test passed");

    return NXT_OK;
}
