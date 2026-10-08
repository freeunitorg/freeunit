
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include <nxt_conf.h>
#include <nxt_http_route_addr.h>
#include "nxt_tests.h"


/*
 * JSON nesting-depth cap (hardening b10a68b4): a payload deeper than
 * NXT_CONF_JSON_MAX_DEPTH (100) must be rejected instead of recursing and
 * blowing the controller's stack.
 */
nxt_int_t
nxt_conf_json_depth_test(nxt_thread_t *thr)
{
    nxt_mp_t          *mp;
    nxt_uint_t        i, k, n;
    nxt_conf_value_t  *value;
    u_char            buf[512];

    /*
     * Exercise the exact NXT_CONF_JSON_MAX_DEPTH (100) boundary: 100 nested
     * arrays are accepted, 101 rejected -- so an off-by-one in the cap is
     * caught, not just gross over-nesting.
     */
    static const struct {
        nxt_uint_t  depth;
        nxt_bool_t  valid;
    } depths[] = {
        {  50, 1 },
        { 100, 1 },   /* at the cap */
        { 101, 0 },   /* one past the cap */
        { 150, 0 },
    };

    nxt_thread_time_update(thr);

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    for (k = 0; k < nxt_nitems(depths); k++) {
        n = depths[k].depth;

        for (i = 0; i < n; i++) {
            buf[i] = '[';
            buf[n + i] = ']';
        }

        value = nxt_conf_json_parse(mp, buf, buf + 2 * n, NULL);

        if ((value != NULL) != (depths[k].valid != 0)) {
            nxt_log_alert(thr->log, "nxt_conf_json_parse() %d-deep nesting: "
                          "got %s, expected %s", (int) n,
                          value != NULL ? "accept" : "reject",
                          depths[k].valid ? "accept" : "reject");
            goto fail;
        }
    }

    nxt_mp_destroy(mp);

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "nxt_conf_json_parse() depth-cap test passed");

    return NXT_OK;

fail:

    nxt_mp_destroy(mp);

    return NXT_ERROR;
}


/*
 * Address/port pattern parser (hardening 68f079b6): malformed port ranges
 * (a dash at either end) and out-of-range CIDR are rejected, while a /32
 * single-host and normal forms are accepted.
 */
nxt_int_t
nxt_http_route_addr_test(nxt_thread_t *thr)
{
    nxt_mp_t          *mp;
    nxt_int_t         ret;
    nxt_bool_t        ok;
    nxt_uint_t        i;
    nxt_conf_value_t  *cv;

    nxt_http_route_addr_pattern_t  pattern;

    static const struct {
        nxt_str_t   json;   /* a JSON string literal (quotes included) */
        nxt_bool_t  ok;     /* expect NXT_OK */
    } tests[] = {
        { nxt_string("\"127.0.0.1\""),      1 },
        { nxt_string("\"127.0.0.1:8080\""), 1 },
        { nxt_string("\"127.0.0.1/32\""),   1 },   /* /32 single-host */
        { nxt_string("\"*:0-65535\""),      1 },
        { nxt_string("\"127.0.0.1:8-\""),   0 },   /* trailing dash */
        { nxt_string("\"127.0.0.1:-8\""),   0 },   /* leading dash */
        { nxt_string("\"11.0.0.0/33\""),    0 },   /* CIDR out of range */
        { nxt_string("\"256.0.0.1\""),      0 },   /* invalid octet */
    };

    nxt_thread_time_update(thr);

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    for (i = 0; i < nxt_nitems(tests); i++) {
        cv = nxt_conf_json_parse(mp, tests[i].json.start,
                                 tests[i].json.start + tests[i].json.length,
                                 NULL);
        if (cv == NULL) {
            nxt_log_alert(thr->log, "route_addr test: JSON parse of %V failed",
                          &tests[i].json);
            nxt_mp_destroy(mp);
            return NXT_ERROR;
        }

        nxt_memzero(&pattern, sizeof(nxt_http_route_addr_pattern_t));

        ret = nxt_http_route_addr_pattern_parse(mp, &pattern, cv);
        ok = (ret == NXT_OK);

        if (ok != tests[i].ok) {
            nxt_log_alert(thr->log, "nxt_http_route_addr_pattern_parse(%V) "
                          "test failed: ret=%d, expected %s", &tests[i].json,
                          (int) ret, tests[i].ok ? "NXT_OK" : "error");
            nxt_mp_destroy(mp);
            return NXT_ERROR;
        }
    }

    nxt_mp_destroy(mp);

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "nxt_http_route_addr_pattern_parse() test passed");

    return NXT_OK;
}



/*
 * Packed: the fields after "flag" are misaligned, for UBSan to catch.  Clang
 * reports the nxt_str_t members with -Wunaligned-access on targets without
 * unaligned access, such as ARMv6.  That misalignment is the purpose here.
 * Clang 13 and older do not know the option and reject its name under
 * -Werror, so it is ignored only where it exists.
 */

#if defined(__clang__)
#pragma clang diagnostic push
#if __has_warning("-Wunaligned-access")
#pragma clang diagnostic ignored "-Wunaligned-access"
#endif
#endif

typedef struct {
    uint8_t     flag;
    int32_t     i32;
    int64_t     i64;
    int         i;
    ssize_t     size;
    off_t       off;
    nxt_msec_t  msec;
    double      dbl;
    nxt_str_t   str;
    char        *cstrz;
    nxt_str_t   rstr;
    void        *ptr;
    uint8_t     bad8;
    int32_t     bad32;
} nxt_packed nxt_conf_map_test_t;

#if defined(__clang__)
#pragma clang diagnostic pop
#endif


#define nxt_conf_map_test_field(field, type)                                  \
    { nxt_string(#field), type, offsetof(nxt_conf_map_test_t, field) }


nxt_int_t
nxt_conf_map_object_test(nxt_thread_t *thr)
{
    void                 *ptr;
    nxt_mp_t             *mp;
    nxt_str_t            str, rstr, orig;
    nxt_int_t            ret;
    nxt_conf_value_t     *cv, *member;
    nxt_conf_map_test_t  dst;

    static const nxt_str_t  str_name = nxt_string("str");
    static const nxt_str_t  rstr_name = nxt_string("rstr");
    static const nxt_str_t  ptr_name = nxt_string("ptr");

    static const nxt_str_t  json = nxt_string(
        "{\"flag\": true, \"i32\": -123456, \"i64\": -9876543210, \"i\": -7,"
        " \"size\": 424242, \"off\": 131072, \"msec\": 3, \"dbl\": 1.5,"
        " \"str\": \"hello\", \"cstrz\": \"world\", \"rstr\": \"raw\","
        " \"ptr\": [1], \"bad8\": 1, \"bad32\": \"x\"}");

    static const nxt_conf_map_t  map[] = {
        nxt_conf_map_test_field(flag, NXT_CONF_MAP_INT8),
        nxt_conf_map_test_field(i32, NXT_CONF_MAP_INT32),
        nxt_conf_map_test_field(i64, NXT_CONF_MAP_INT64),
        nxt_conf_map_test_field(i, NXT_CONF_MAP_INT),
        nxt_conf_map_test_field(size, NXT_CONF_MAP_SIZE),
        nxt_conf_map_test_field(off, NXT_CONF_MAP_OFF),
        nxt_conf_map_test_field(msec, NXT_CONF_MAP_MSEC),
        nxt_conf_map_test_field(dbl, NXT_CONF_MAP_DOUBLE),
        nxt_conf_map_test_field(str, NXT_CONF_MAP_STR_COPY),
        nxt_conf_map_test_field(cstrz, NXT_CONF_MAP_CSTRZ),
        nxt_conf_map_test_field(rstr, NXT_CONF_MAP_STR),
        nxt_conf_map_test_field(ptr, NXT_CONF_MAP_PTR),
        /* Wrong JSON types: the fields keep their sentinels. */
        nxt_conf_map_test_field(bad8, NXT_CONF_MAP_INT8),
        nxt_conf_map_test_field(bad32, NXT_CONF_MAP_INT32),
    };

    nxt_thread_time_update(thr);

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    ret = NXT_ERROR;
    nxt_memzero(&dst, sizeof(dst));
    dst.bad8 = 0xA5;
    dst.bad32 = 0x5A5A5A5A;

    cv = nxt_conf_json_parse(mp, json.start, json.start + json.length, NULL);

    if (cv == NULL
        || nxt_conf_map_object(mp, cv, map, nxt_nitems(map), &dst) != NXT_OK)
    {
        nxt_log_alert(thr->log, "nxt_conf_map_object() test failed");
        goto done;
    }

    if (dst.flag != 1
        || dst.i32 != -123456
        || dst.i64 != -9876543210LL
        || dst.i != -7
        || dst.size != 424242
        || dst.off != 131072
        || dst.msec != 3000
        || dst.dbl != 1.5
        || dst.str.length != 5
        || memcmp(dst.str.start, "hello", 5) != 0
        || dst.cstrz == NULL
        || strcmp(dst.cstrz, "world") != 0
        || dst.rstr.length != 3
        || memcmp(dst.rstr.start, "raw", 3) != 0
        || dst.ptr == NULL
        || nxt_conf_type(dst.ptr) != NXT_CONF_ARRAY
        || dst.bad8 != 0xA5
        || dst.bad32 != 0x5A5A5A5A)
    {
        nxt_log_alert(thr->log, "nxt_conf_map_object() mapped wrong values");
        goto done;
    }

    /*
     * The non-scalar copies, read back with memcpy() from their misaligned
     * fields.  NXT_CONF_MAP_STR stores the member's own string, without a
     * copy.  NXT_CONF_MAP_STR_COPY stores a copy from the pool.
     * NXT_CONF_MAP_PTR stores the member's nxt_conf_value_t itself.
     */

    nxt_memcpy(&rstr, &dst.rstr, sizeof(rstr));
    nxt_memcpy(&str, &dst.str, sizeof(str));
    nxt_memcpy(&ptr, &dst.ptr, sizeof(ptr));

    member = nxt_conf_get_object_member(cv, &rstr_name, NULL);
    nxt_conf_get_string(member, &orig);

    if (rstr.start != orig.start || rstr.length != orig.length) {
        nxt_log_alert(thr->log, "nxt_conf_map_object() NXT_CONF_MAP_STR "
                      "did not store the member's own string");
        goto done;
    }

    member = nxt_conf_get_object_member(cv, &str_name, NULL);
    nxt_conf_get_string(member, &orig);

    if (str.start == orig.start
        || str.length != orig.length
        || memcmp(str.start, orig.start, orig.length) != 0)
    {
        nxt_log_alert(thr->log, "nxt_conf_map_object() NXT_CONF_MAP_STR_COPY "
                      "did not store a copy");
        goto done;
    }

    if (ptr != nxt_conf_get_object_member(cv, &ptr_name, NULL)) {
        nxt_log_alert(thr->log, "nxt_conf_map_object() NXT_CONF_MAP_PTR "
                      "did not store the member's value");
        goto done;
    }

    ret = NXT_OK;

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "nxt_conf_map_object() alignment test passed");

done:

    nxt_mp_destroy(mp);

    return ret;
}


typedef struct {
    nxt_conf_map_type_t  type;
    const char           *num;
    nxt_int_t            ret;
    int64_t              val;
} nxt_conf_map_bound_case_t;


static nxt_int_t
nxt_conf_map_bound_case(nxt_thread_t *thr, nxt_mp_t *mp,
    const nxt_conf_map_bound_case_t *tc)
{
    int64_t           got;
    nxt_str_t         json;
    nxt_int_t         rc;
    nxt_conf_map_t    map[1];
    nxt_conf_value_t  *cv;
    u_char            buf[64];

    union {
        int32_t     i32;
        int64_t     i64;
        int         i;
        off_t       off;
        nxt_msec_t  msec;
    } dst;

    json.start = buf;
    json.length = nxt_sprintf(buf, buf + sizeof(buf), "{\"v\": %s}", tc->num)
                  - buf;

    cv = nxt_conf_json_parse(mp, json.start, json.start + json.length, NULL);

    NXT_TEST_CHECK(thr->log, cv != NULL,
                   "map bound test: %s did not parse", tc->num);

    map[0].name = (nxt_str_t) nxt_string("v");
    map[0].type = tc->type;
    map[0].offset = 0;

    nxt_memzero(&dst, sizeof(dst));

    rc = nxt_conf_map_object(mp, cv, map, 1, &dst);

    switch (tc->type) {
    case NXT_CONF_MAP_INT32:
        got = dst.i32;
        break;
    case NXT_CONF_MAP_INT:
        got = dst.i;
        break;
    case NXT_CONF_MAP_OFF:
        got = dst.off;
        break;
    case NXT_CONF_MAP_MSEC:
        got = dst.msec;
        break;
    default:
        got = dst.i64;
        break;
    }

    NXT_TEST_CHECK(thr->log,
                   rc == tc->ret && (rc != NXT_OK || got == tc->val),
                   "map bound test failed: type %d, %s gave %i and %L, "
                   "not %i and %L", (int) tc->type, tc->num, rc, got,
                   tc->ret, tc->val);

    return NXT_OK;
}


/*
 * nxt_conf_map_object() returns NXT_ERROR for a number that has no value in
 * the integer destination type.  Each case maps one member and checks the
 * status, and for NXT_OK the value.  Numbers of 2^63 or more cannot be
 * parsed, so the 64-bit bounds have only in-range cases here.
 * NXT_CONF_MAP_MSEC does not fail: it maps a number out of its range to
 * 4294967 seconds.
 */
nxt_int_t
nxt_conf_map_bound_test(nxt_thread_t *thr)
{
    nxt_mp_t    *mp;
    nxt_int_t   ret;
    nxt_uint_t  i;

    static const nxt_conf_map_bound_case_t  cases[] = {
        { NXT_CONF_MAP_INT32, "2147483647", NXT_OK, 2147483647 },
        { NXT_CONF_MAP_INT32, "-2147483648", NXT_OK, -2147483647 - 1 },
        { NXT_CONF_MAP_INT32, "2147483648", NXT_ERROR, 0 },
        { NXT_CONF_MAP_INT32, "-2147483649", NXT_ERROR, 0 },
        { NXT_CONF_MAP_INT32, "1e10", NXT_ERROR, 0 },

        { NXT_CONF_MAP_INT, "2147483647", NXT_OK, 2147483647 },
        { NXT_CONF_MAP_INT, "-2147483648", NXT_OK, -2147483647 - 1 },
        { NXT_CONF_MAP_INT, "2147483648", NXT_ERROR, 0 },
        { NXT_CONF_MAP_INT, "-2147483649", NXT_ERROR, 0 },

        { NXT_CONF_MAP_INT64, "9.22337203e18", NXT_OK,
          9223372030000000000LL },
        { NXT_CONF_MAP_INT64, "-9.22337203e18", NXT_OK,
          -9223372030000000000LL },

#if (NXT_OFF_T_SIZE == 4)
        { NXT_CONF_MAP_OFF, "2147483647", NXT_OK, 2147483647 },
        { NXT_CONF_MAP_OFF, "-2147483648", NXT_OK, -2147483647 - 1 },
        { NXT_CONF_MAP_OFF, "2147483648", NXT_ERROR, 0 },
        { NXT_CONF_MAP_OFF, "-2147483649", NXT_ERROR, 0 },
#else
        { NXT_CONF_MAP_OFF, "2147483648", NXT_OK, 2147483648LL },
        { NXT_CONF_MAP_OFF, "9.22337203e18", NXT_OK, 9223372030000000000LL },
        { NXT_CONF_MAP_OFF, "-9.22337203e18", NXT_OK,
          -9223372030000000000LL },
#endif

        { NXT_CONF_MAP_MSEC, "0", NXT_OK, 0 },
        { NXT_CONF_MAP_MSEC, "4294967", NXT_OK, 4294967000LL },
        /* A stored timeout out of the range gives the largest one. */
        { NXT_CONF_MAP_MSEC, "4294968", NXT_OK, 4294967000LL },
        { NXT_CONF_MAP_MSEC, "-1", NXT_OK, 4294967000LL },
    };

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    ret = NXT_OK;

    for (i = 0; i < nxt_nitems(cases); i++) {
        ret = nxt_conf_map_bound_case(thr, mp, &cases[i]);
        if (ret != NXT_OK) {
            break;
        }
    }

    nxt_mp_destroy(mp);

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_conf_map_object() bound "
                      "test passed");
    }

    return ret;
}
