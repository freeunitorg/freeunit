/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * "schedules" (docs/adr/0004-schedules.md): what test/test_schedules.py
 * cannot observe from outside.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_conf.h>
#include <nxt_http.h>
#include <nxt_router_schedule.h>
#include "nxt_tests.h"


/* The info log never shows the last path segment or the query. */

static nxt_int_t
nxt_router_schedule_uri_public_test(nxt_thread_t *thr)
{
    size_t      n;
    nxt_uint_t  i;

    static const struct {
        nxt_str_t  uri;
        size_t     public;
    } tests[] = {
        { nxt_string("/cron/SECRET_KEY"),        6 },
        { nxt_string("/cron"),                   1 },
        { nxt_string("/"),                       1 },
        { nxt_string("/a/b/"),                   5 },
        { nxt_string("/a/b/?key=x/y"),           5 },
        { nxt_string("/cron?key=a/b/c"),         1 },
        { nxt_string("/core/cron.php?cron_key"), 6 },
        { nxt_string("x"),                       0 },
    };

    for (i = 0; i < nxt_nitems(tests); i++) {
        n = nxt_router_schedule_uri_public(&tests[i].uri);

        NXT_TEST_CHECK(thr->log, n == tests[i].public, "schedule uri public "
                       "\"%V\": %uz", &tests[i].uri, n);
    }

    return NXT_OK;
}


/*
 * A run's request parses to the same target, path and arguments as the
 * same request line from a client, dot segments and escapes included.
 */

static nxt_int_t
nxt_router_schedule_request_test(nxt_thread_t *thr)
{
    nxt_mp_t                  *mp;
    nxt_int_t                 ret;
    nxt_str_t                 json, line;
    nxt_conf_value_t          *headers;
    nxt_router_schedule_t     sched;
    nxt_http_request_parse_t  rs, rc;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    nxt_memzero(&sched, sizeof(sched));
    nxt_memzero(&rs, sizeof(rs));
    nxt_memzero(&rc, sizeof(rc));

    nxt_str_set(&sched.name, "cron");
    nxt_str_set(&sched.uri, "/a/%2E%2E/b/../c%2Fd/./e?x=1&y=%2F");
    nxt_str_set(&json, "{\"Host\": \"example.org\"}");
    nxt_str_set(&line, "GET /a/%2E%2E/b/../c%2Fd/./e?x=1&y=%2F HTTP/1.1\r\n"
                       "Host: example.org\r\n\r\n");

    headers = nxt_conf_json_parse_str(mp, &json);

    ret = (headers != NULL
           && nxt_router_schedule_request_build(mp, &sched, headers) == NXT_OK
           && nxt_router_schedule_parse(mp, &sched.request, &rs) == NXT_DONE
           && nxt_router_schedule_parse(mp, &line, &rc) == NXT_DONE
           && nxt_strstr_eq(&rs.method, &rc.method)
           && nxt_strstr_eq(&rs.path, &rc.path)
           && nxt_strstr_eq(&rs.args, &rc.args)
           && rs.target_end - rs.target_start
              == rc.target_end - rc.target_start
           && rs.complex_target == rc.complex_target
           && rs.quoted_target == rc.quoted_target
           && rs.version.ui64 == rc.version.ui64)
          ? NXT_OK : NXT_ERROR;

    if (ret != NXT_OK) {
        nxt_log_alert(thr->log, "schedule request \"%V\": path \"%V\" args "
                      "\"%V\"", &sched.request, &rs.path, &rs.args);
    }

    nxt_mp_destroy(mp);

    return ret;
}


/*
 * The reference chain run -> joint -> skcf -> rtcf: the configuration
 * outlives both the joint's own reference and a run's.  In pass 0 a listener
 * holds it too, and in pass 1 the last release frees it (for the sanitizer
 * builds).  The self-linked skcf never touches router->sockets.
 */

static nxt_int_t
nxt_router_schedule_joint_test(nxt_thread_t *thr)
{
    nxt_mp_t                 *mp;
    uint32_t                 held;
    nxt_task_t               *task;
    nxt_router_t             router;
    nxt_queue_link_t         sentinel;
    nxt_router_conf_t        *rtcf;
    nxt_router_schedules_t   *sc;

    task = thr->task;

    nxt_memzero(&router, sizeof(router));
    nxt_queue_init(&router.sockets);
    nxt_queue_insert_tail(&router.sockets, &sentinel);

    for (held = 0; held < 2; held++) {
        mp = nxt_mp_create(1024, 128, 256, 32);
        if (mp == NULL) {
            return NXT_ERROR;
        }

        rtcf = nxt_mp_zget(mp, sizeof(nxt_router_conf_t));
        sc = nxt_mp_zget(mp, sizeof(nxt_router_schedules_t));

        if (rtcf == NULL || sc == NULL) {
            return NXT_ERROR;
        }

        rtcf->mem_pool = mp;
        rtcf->router = &router;
        rtcf->count = held;

        rtcf->tstr_state = nxt_tstr_state_new(mp, 0);

        if (rtcf->tstr_state == NULL
            || nxt_router_schedules_joint_init(task, rtcf, sc, NULL) != NXT_OK
            || rtcf->count != held + 1)
        {
            nxt_log_alert(thr->log, "schedule joint init failed");
            return NXT_ERROR;
        }

        sc->joint.count++;                          /* a run */
        nxt_router_conf_release(task, &sc->joint);  /* the next conf */

        NXT_TEST_CHECK(thr->log,
                       sc->joint.count == 1 && rtcf->count == held + 1,
                       "schedule joint released too early");

        nxt_router_conf_release(task, &sc->joint);  /* the run ends */

        if (held) {
            NXT_TEST_CHECK(thr->log, rtcf->count == 1,
                           "schedule joint: rtcf count %uD", rtcf->count);

            nxt_tstr_state_release(rtcf->tstr_state);
            nxt_mp_destroy(mp);
        }
    }

    NXT_TEST_CHECK(thr->log, nxt_queue_first(&router.sockets) == &sentinel
                   && nxt_queue_last(&router.sockets) == &sentinel,
                   "schedule joint: router->sockets changed");

    return NXT_OK;
}


/*
 * A configuration whose schedule cannot be resolved fails as a whole, and
 * takes no reference on the rtcf.
 */

static nxt_int_t
nxt_router_schedule_resolve_test(nxt_thread_t *thr)
{
    nxt_int_t               ret;
    nxt_str_t               json;
    nxt_uint_t              i;
    nxt_task_t              *task;
    nxt_conf_value_t        *root;
    nxt_router_conf_t       *rtcf;
    nxt_router_temp_conf_t  *tmcf;

    static const struct {
        const char  *json;
        nxt_int_t   ret;
    } tests[] = {
        { "{}", NXT_OK },
        { "{\"schedules\": {}}", NXT_OK },
        { "{\"schedules\": {\"x\": {\"pass\": \"applications/missing\","
          "\"uri\": \"/\", \"interval\": 1}}}", NXT_ERROR },
    };

    task = thr->task;

    for (i = 0; i < nxt_nitems(tests); i++) {
        tmcf = nxt_router_test_temp_conf(task);
        if (tmcf == NULL) {
            return NXT_ERROR;
        }

        rtcf = tmcf->router_conf;

        json.start = (u_char *) tests[i].json;
        json.length = nxt_strlen(tests[i].json);

        root = nxt_conf_json_parse_str(tmcf->mem_pool, &json);
        if (root == NULL) {
            return NXT_ERROR;
        }

        ret = nxt_router_conf_resolve(task, tmcf, root);

        NXT_TEST_CHECK(thr->log, ret == tests[i].ret
                       && rtcf->schedules == NULL && rtcf->count == 0,
                       "schedule resolve \"%s\": %i, schedules %p, count "
                       "%uD", tests[i].json, ret, rtcf->schedules,
                       rtcf->count);

        nxt_tstr_state_release(rtcf->tstr_state);
        nxt_mp_destroy(rtcf->mem_pool);
        nxt_mp_destroy(tmcf->mem_pool);
    }

    return NXT_OK;
}


nxt_int_t
nxt_router_schedule_test(nxt_thread_t *thr)
{
    nxt_thread_time_update(thr);

    if (nxt_router_schedule_uri_public_test(thr) != NXT_OK
        || nxt_router_schedule_request_test(thr) != NXT_OK
        || nxt_router_schedule_joint_test(thr) != NXT_OK
        || nxt_router_schedule_resolve_test(thr) != NXT_OK)
    {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router schedule test passed");

    return NXT_OK;
}
