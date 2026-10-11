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
#include <nxt_event_engine.h>
#include "nxt_tests.h"


/* A log line never shows the last path segment or the query. */

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
 * outlives both the joint's own reference and a run's.  In pass 0 the last
 * release frees it (for the sanitizer builds).  In pass 1 a listener holds
 * it too, so the test destroys it.  The self-linked skcf never touches
 * router->sockets.
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
 * A schedule removed while its run is still queued on the worker engine.
 * The real apply, timer and run code posts to two fixture engines, and the
 * test runs their queues by hand.  The removal posts the release of the
 * old joint behind the run, so the run takes its reference first, and the
 * configuration lives until the run ends.  The run gets no request (the
 * NXT_TESTS hook), so it ends at once through the zero-delay timer, as a
 * run whose request allocation fails does.  A listener holds the rtcf as
 * well, so the test can read its count at the end.
 */

static void
nxt_router_schedule_test_signal(nxt_event_engine_t *engine, nxt_uint_t signo)
{
}


static void
nxt_router_schedule_test_timers(nxt_thread_t *thr, nxt_event_engine_t *engine,
    nxt_msec_t timeout)
{
    void                *obj, *data;
    nxt_task_t          *task;
    nxt_work_handler_t  handler;

    thr->engine = engine;

    (void) nxt_timer_find(engine);
    nxt_timer_expire(engine, engine->timers.now + timeout);

    /* nxt_work_queue_pop() needs a work: it does not check for an end. */

    while (engine->fast_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine->fast_work_queue, &task, &obj,
                                     &data);
        handler(task, obj, data);
    }
}


static nxt_uint_t
nxt_router_schedule_test_posted(nxt_thread_t *thr, nxt_event_engine_t *engine,
    void **objs, nxt_uint_t max)
{
    void                *obj, *data;
    nxt_uint_t          n;
    nxt_task_t          *task;
    nxt_work_handler_t  handler;

    thr->engine = engine;

    for (n = 0; ; n++) {
        handler = nxt_locked_work_queue_pop(&engine->locked_work_queue, &task,
                                            &obj, &data);
        if (handler == NULL) {
            return n;
        }

        if (n < max) {
            objs[n] = obj;
        }

        handler(task, obj, data);
    }
}


static nxt_int_t
nxt_router_schedule_queued_test(nxt_thread_t *thr)
{
    void                    *objs[4];
    u_char                  name[32];
    nxt_mp_t                *mp;
    nxt_int_t               ret;
    nxt_uint_t              n;
    nxt_task_t              *task;
    nxt_router_t            router;
    nxt_event_engine_t      main_engine, worker, *saved;
    nxt_router_conf_t       *rtcf, empty;
    nxt_status_schedule_t   stat;
    nxt_router_temp_conf_t  tmcf;
    nxt_work_queue_cache_t  cache;
    nxt_router_schedule_t   *sched;
    nxt_router_schedules_t  *sc;

    task = thr->task;
    saved = thr->engine;
    ret = NXT_ERROR;

    nxt_memzero(&main_engine, sizeof(nxt_event_engine_t));
    nxt_memzero(&worker, sizeof(nxt_event_engine_t));
    nxt_memzero(&router, sizeof(nxt_router_t));
    nxt_memzero(&empty, sizeof(nxt_router_conf_t));
    nxt_memzero(&tmcf, sizeof(nxt_router_temp_conf_t));

    if (nxt_timers_init(&main_engine.timers, 4) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_timers_init(&worker.timers, 4) != NXT_OK) {
        nxt_free(main_engine.timers.changes);
        return NXT_ERROR;
    }

    nxt_work_queue_cache_create(&cache, 0);

    main_engine.task = *task;
    main_engine.event.signal = nxt_router_schedule_test_signal;
    main_engine.fast_work_queue.cache = &cache;

    worker.task = *task;
    worker.event.signal = nxt_router_schedule_test_signal;
    worker.fast_work_queue.cache = &cache;
    nxt_queue_init(&worker.joints);

    nxt_queue_init(&router.engines);
    nxt_queue_insert_tail(&router.engines, &worker.link0);

    empty.router = &router;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        goto free_timers;
    }

    rtcf = nxt_mp_zget(mp, sizeof(nxt_router_conf_t));
    sc = nxt_mp_zget(mp, sizeof(nxt_router_schedules_t));
    sched = nxt_mp_zget(mp, sizeof(nxt_router_schedule_t));

    if (rtcf == NULL || sc == NULL || sched == NULL) {
        goto destroy;
    }

    rtcf->mem_pool = mp;
    rtcf->router = &router;
    rtcf->count = 1;                                /* a listener */

    rtcf->tstr_state = nxt_tstr_state_new(mp, 0);
    if (rtcf->tstr_state == NULL) {
        goto destroy;
    }

    if (nxt_router_schedules_joint_init(task, rtcf, sc, NULL) != NXT_OK) {
        goto release;
    }

    nxt_str_set(&sched->name, "queued");
    nxt_str_set(&sched->uri, "/cron");
    sched->interval = 60000;
    sched->timeout = 60000;

    sc->schedule = sched;
    sc->nschedules = 1;
    rtcf->schedules = sc;

    /* The configuration with the schedule, on the main engine. */

    thr->engine = &main_engine;
    tmcf.router_conf = rtcf;
    nxt_router_schedules_apply(task, &tmcf);

    /* The schedule is due: its run is posted to the worker engine. */

    nxt_router_schedule_test_timers(thr, &main_engine, sched->interval + 1);

    /* The next configuration has no schedules. */

    thr->engine = &main_engine;
    tmcf.router_conf = &empty;
    nxt_router_schedules_apply(task, &tmcf);

    (void) nxt_router_schedules_status_size(&n);

    if (n != 1) {
        nxt_log_alert(thr->log, "schedule queued: %ui states after the "
                      "removal, expected 1", n);
        goto release;
    }

    /* The joint insert, the run and the release, in that order. */

    nxt_router_schedule_test_request_failures = 1;

    n = nxt_router_schedule_test_posted(thr, &worker, objs, nxt_nitems(objs));

    nxt_router_schedule_test_request_failures = 0;

    if (n != 3 || objs[0] != &sc->joint || objs[1] == &sc->joint
        || objs[2] != &sc->joint)
    {
        nxt_log_alert(thr->log, "schedule queued: %ui jobs on the worker "
                      "engine, expected the insert, the run and the release",
                      n);
        goto release;
    }

    if (sc->joint.count != 1 || rtcf->count != 2
        || nxt_queue_is_empty(&worker.joints))
    {
        nxt_log_alert(thr->log, "schedule queued: joint %uD, rtcf %uD before "
                      "the run ends", sc->joint.count, rtcf->count);
        goto release;
    }

    /* The run ends: its timer drops the last joint reference. */

    nxt_router_schedule_test_timers(thr, &worker, 1);

    if (rtcf->count != 1 || !nxt_queue_is_empty(&worker.joints)) {
        nxt_log_alert(thr->log, "schedule queued: rtcf %uD after the run",
                      rtcf->count);
        goto release;
    }

    nxt_router_schedules_status(&stat, name + sizeof(name), name);

    if (stat.running != 1 || stat.runs != 1) {
        nxt_log_alert(thr->log, "schedule queued: running %uD, runs %uD "
                      "before the result", stat.running, stat.runs);
        goto release;
    }

    /* The result, on the main engine, frees the removed state. */

    (void) nxt_router_schedule_test_posted(thr, &main_engine, objs,
                                           nxt_nitems(objs));
    nxt_router_schedule_test_timers(thr, &main_engine, 1);

    (void) nxt_router_schedules_status_size(&n);

    if (n != 0) {
        nxt_log_alert(thr->log, "schedule queued: %ui states at the end", n);
        goto release;
    }

    ret = NXT_OK;

release:

    /*
     * After a "goto release", the state can stay on
     * nxt_router_schedule_states, and nxt_router_schedules_current can point
     * to the freed pool.  This is harmless: the test program then fails.
     */

    nxt_tstr_state_release(rtcf->tstr_state);

destroy:

    nxt_mp_destroy(mp);

free_timers:

    thr->engine = saved;

    nxt_work_queue_cache_destroy(&cache);
    nxt_free(main_engine.timers.changes);
    nxt_free(worker.timers.changes);

    return ret;
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
        || nxt_router_schedule_queued_test(thr) != NXT_OK
        || nxt_router_schedule_resolve_test(thr) != NXT_OK)
    {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router schedule test passed");

    return NXT_OK;
}
