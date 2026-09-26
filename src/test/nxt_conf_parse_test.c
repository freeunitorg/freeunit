
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



/* Packed: the fields after "flag" are misaligned, for UBSan to catch. */

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


#define nxt_conf_map_test_field(field, type)                                  \
    { nxt_string(#field), type, offsetof(nxt_conf_map_test_t, field) }


nxt_int_t
nxt_conf_map_object_test(nxt_thread_t *thr)
{
    nxt_mp_t             *mp;
    nxt_int_t            ret;
    nxt_conf_value_t     *cv;
    nxt_conf_map_test_t  dst;

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

    ret = NXT_OK;

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "nxt_conf_map_object() alignment test passed");

done:

    nxt_mp_destroy(mp);

    return ret;
}
