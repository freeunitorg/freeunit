
/*
 * Copyright (C) FreeUnit Community
 */

/*
 * The parts of "limit_req" that need no router: the zone name parser, the
 * key of a client address, and the wait estimate the zone gives a rejected
 * request, which is what "Retry-After" is made of.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_http_limit_req.h>
#include "nxt_tests.h"


static nxt_int_t
nxt_http_limit_req_test_zone_parse(nxt_thread_t *thr)
{
    uint32_t   keys;
    nxt_str_t  name;
    nxt_int_t  ret;

    static const nxt_str_t  plain = nxt_string("login");
    static const nxt_str_t  sized = nxt_string("login:10");
    static const nxt_str_t  zero = nxt_string("login:0");
    static const nxt_str_t  big = nxt_string("login:1048577");
    static const nxt_str_t  text = nxt_string("login:10k");
    static const nxt_str_t  empty = nxt_string("login:");
    static const nxt_str_t  noname = nxt_string(":10");

    ret = nxt_http_limit_req_zone_parse(&plain, &name, &keys);
    NXT_TEST_CHECK(thr->log, ret == NXT_OK && name.length == 5
                             && keys == NXT_HTTP_LIMIT_REQ_KEYS_DEFAULT,
                   "zone parse: name only");

    ret = nxt_http_limit_req_zone_parse(&sized, &name, &keys);
    NXT_TEST_CHECK(thr->log, ret == NXT_OK && name.length == 5 && keys == 10,
                   "zone parse: name and size");

    ret = nxt_http_limit_req_zone_parse(&zero, &name, &keys);
    NXT_TEST_CHECK(thr->log, ret == NXT_DECLINED, "zone parse: size 0");

    ret = nxt_http_limit_req_zone_parse(&big, &name, &keys);
    NXT_TEST_CHECK(thr->log, ret == NXT_DECLINED, "zone parse: size above max");

    ret = nxt_http_limit_req_zone_parse(&text, &name, &keys);
    NXT_TEST_CHECK(thr->log, ret == NXT_DECLINED, "zone parse: size with unit");

    ret = nxt_http_limit_req_zone_parse(&empty, &name, &keys);
    NXT_TEST_CHECK(thr->log, ret == NXT_DECLINED, "zone parse: empty size");

    ret = nxt_http_limit_req_zone_parse(&noname, &name, &keys);
    NXT_TEST_CHECK(thr->log, ret == NXT_ERROR, "zone parse: empty name");

    return NXT_OK;
}


static nxt_int_t
nxt_http_limit_req_test_keys(nxt_thread_t *thr)
{
    size_t          len1, len2;
    u_char          k1[NXT_LIMIT_KEY_MAX], k2[NXT_LIMIT_KEY_MAX];
    nxt_sockaddr_t  sa1, sa2;

    nxt_memzero(&sa1, sizeof(nxt_sockaddr_t));
    nxt_memzero(&sa2, sizeof(nxt_sockaddr_t));

    /* IPv4: the port is not a part of the key. */

    sa1.u.sockaddr_in.sin_family = AF_INET;
    sa1.u.sockaddr_in.sin_addr.s_addr = 0x0100007f;
    sa1.u.sockaddr_in.sin_port = 1;

    sa2 = sa1;
    sa2.u.sockaddr_in.sin_port = 2;

    len1 = nxt_http_limit_req_key(&sa1, k1);
    len2 = nxt_http_limit_req_key(&sa2, k2);

    NXT_TEST_CHECK(thr->log, len1 == 4 && len2 == 4
                             && memcmp(k1, k2, 4) == 0,
                   "key: IPv4 port");

    sa2.u.sockaddr_in.sin_addr.s_addr = 0x0200007f;
    len2 = nxt_http_limit_req_key(&sa2, k2);

    NXT_TEST_CHECK(thr->log, memcmp(k1, k2, 4) != 0, "key: IPv4 address");

#if (NXT_INET6)

    /* IPv6: the first 64 bits are the key. */

    nxt_memzero(&sa1, sizeof(nxt_sockaddr_t));
    sa1.u.sockaddr_in6.sin6_family = AF_INET6;
    sa1.u.sockaddr_in6.sin6_addr.s6_addr[0] = 0x20;
    sa1.u.sockaddr_in6.sin6_addr.s6_addr[1] = 0x01;
    sa1.u.sockaddr_in6.sin6_addr.s6_addr[15] = 0x01;

    sa2 = sa1;
    sa2.u.sockaddr_in6.sin6_addr.s6_addr[15] = 0x02;
    sa2.u.sockaddr_in6.sin6_addr.s6_addr[8] = 0xff;

    len1 = nxt_http_limit_req_key(&sa1, k1);
    len2 = nxt_http_limit_req_key(&sa2, k2);

    NXT_TEST_CHECK(thr->log, len1 == 8 && len2 == 8
                             && memcmp(k1, k2, 8) == 0,
                   "key: IPv6 interface id");

    sa2.u.sockaddr_in6.sin6_addr.s6_addr[7] = 0x01;
    len2 = nxt_http_limit_req_key(&sa2, k2);

    NXT_TEST_CHECK(thr->log, memcmp(k1, k2, 8) != 0, "key: IPv6 prefix");

    /*
     * IPv4 mapped into IPv6 (::ffff:10.0.0.1): the key of the IPv4
     * address, not of the /64 that all the mapped addresses share.
     */

    nxt_memzero(&sa1, sizeof(nxt_sockaddr_t));
    sa1.u.sockaddr_in6.sin6_family = AF_INET6;
    sa1.u.sockaddr_in6.sin6_addr.s6_addr[10] = 0xff;
    sa1.u.sockaddr_in6.sin6_addr.s6_addr[11] = 0xff;
    sa1.u.sockaddr_in6.sin6_addr.s6_addr[12] = 10;
    sa1.u.sockaddr_in6.sin6_addr.s6_addr[15] = 1;

    nxt_memzero(&sa2, sizeof(nxt_sockaddr_t));
    sa2.u.sockaddr_in.sin_family = AF_INET;
    sa2.u.sockaddr_in.sin_addr.s_addr = htonl(0x0a000001);

    len1 = nxt_http_limit_req_key(&sa1, k1);
    len2 = nxt_http_limit_req_key(&sa2, k2);

    NXT_TEST_CHECK(thr->log, len1 == 4 && len2 == 4
                             && memcmp(k1, k2, 4) == 0,
                   "key: IPv4-mapped IPv6 is the IPv4 key");

    sa1.u.sockaddr_in6.sin6_addr.s6_addr[15] = 2;
    len1 = nxt_http_limit_req_key(&sa1, k1);

    NXT_TEST_CHECK(thr->log, len1 == 4 && memcmp(k1, k2, 4) != 0,
                   "key: two IPv4-mapped addresses");

#endif

    return NXT_OK;
}


static nxt_int_t
nxt_http_limit_req_test_wait(nxt_thread_t *thr)
{
    uint64_t          wait;
    nxt_mp_t          *mp;
    nxt_bool_t        allow;
    nxt_limit_zone_t  *zone;

    static const u_char  key[4] = { 10, 0, 0, 1 };

    mp = nxt_mp_create(32768, 128, 4096, 64);
    NXT_TEST_CHECK(thr->log, mp != NULL, "nxt_mp_create() failed");

    /* Two per second, two at once. */

    zone = nxt_limit_zone_create(mp, 4, 2, 2);
    NXT_TEST_CHECK(thr->log, zone != NULL, "nxt_limit_zone_create() failed");

    wait = 1;
    allow = nxt_limit_zone_check(zone, key, 4, 1000000, &wait);
    NXT_TEST_CHECK(thr->log, allow && wait == 0, "wait: first request");

    allow = nxt_limit_zone_check(zone, key, 4, 1000000, &wait);
    NXT_TEST_CHECK(thr->log, allow && wait == 0, "wait: burst request");

    /*
     * Empty: one request is 1000000 micro-tokens at 2 per microsecond.
     * The time and the wait are in microseconds.
     */

    allow = nxt_limit_zone_check(zone, key, 4, 1000000, &wait);
    NXT_TEST_CHECK(thr->log, !allow && wait == 500000, "wait: empty %uL",
                   wait);

    /* 100 ms later 200000 micro-tokens are in: 800000 to go. */

    allow = nxt_limit_zone_check(zone, key, 4, 1100000, &wait);
    NXT_TEST_CHECK(thr->log, !allow && wait == 400000, "wait: 100 ms %uL",
                   wait);

    allow = nxt_limit_zone_check(zone, key, 4, 1500000, &wait);
    NXT_TEST_CHECK(thr->log, allow && wait == 0, "wait: refilled");

    /* The estimate without the pointer is simply not given. */

    allow = nxt_limit_zone_check(zone, key, 4, 1500000, NULL);
    NXT_TEST_CHECK(thr->log, !allow, "wait: no pointer");

    nxt_mp_destroy(mp);

    return NXT_OK;
}


nxt_int_t
nxt_http_limit_req_test(nxt_thread_t *thr)
{
    if (nxt_http_limit_req_test_zone_parse(thr) != NXT_OK
        || nxt_http_limit_req_test_keys(thr) != NXT_OK
        || nxt_http_limit_req_test_wait(thr) != NXT_OK)
    {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "http limit_req test passed");

    return NXT_OK;
}
