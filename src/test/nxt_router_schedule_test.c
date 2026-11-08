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
 * The fixture of the tests below: the one of the test above, with the real
 * access log code and the real variables.  The rtcf has one schedule, and
 * a listener holds it, so a test can read its count at the end.
 */

typedef struct {
    nxt_event_engine_t      main;
    nxt_event_engine_t      worker;
    nxt_event_engine_t      *saved_engine;
    nxt_router_t            router;
    nxt_router_t            *saved_router;
    nxt_router_conf_t       empty;
    nxt_work_queue_cache_t  cache;
    nxt_mp_t                *mp;
    nxt_router_conf_t       *rtcf;
    nxt_router_schedules_t  *sc;
    nxt_router_schedule_t   *sched;
    nxt_bool_t              timers;
} nxt_router_schedule_fixture_t;


static nxt_int_t
nxt_router_schedule_fixture_init(nxt_thread_t *thr,
    nxt_router_schedule_fixture_t *f, const char *name, nxt_msec_t timeout)
{
    nxt_task_t              *task;
    nxt_router_conf_t       *rtcf;
    nxt_router_schedule_t   *sched;
    nxt_router_schedules_t  *sc;

    task = thr->task;

    nxt_memzero(f, sizeof(nxt_router_schedule_fixture_t));

    f->saved_engine = thr->engine;
    f->saved_router = nxt_router;

    if (nxt_timers_init(&f->main.timers, 4) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_timers_init(&f->worker.timers, 4) != NXT_OK) {
        nxt_free(f->main.timers.changes);
        return NXT_ERROR;
    }

    f->timers = 1;

    nxt_work_queue_cache_create(&f->cache, 0);

    f->main.task = *task;
    f->main.event.signal = nxt_router_schedule_test_signal;
    f->main.fast_work_queue.cache = &f->cache;

    f->worker.task = *task;
    f->worker.event.signal = nxt_router_schedule_test_signal;
    f->worker.fast_work_queue.cache = &f->cache;
    nxt_queue_init(&f->worker.joints);

    nxt_queue_init(&f->router.engines);
    nxt_queue_insert_tail(&f->router.engines, &f->worker.link0);
    nxt_queue_init(&f->router.access_logs);

    nxt_router = &f->router;
    f->empty.router = &f->router;

    /*
     * The router registers the variables at its start; this program does
     * not.  A second registration fails at its first name and changes
     * nothing, so the result does not matter: a compile fails if a name is
     * missing.
     */

    (void) nxt_http_register_variables();

    f->mp = nxt_mp_create(1024, 128, 256, 32);
    if (f->mp == NULL) {
        return NXT_ERROR;
    }

    rtcf = nxt_mp_zget(f->mp, sizeof(nxt_router_conf_t));
    sc = nxt_mp_zget(f->mp, sizeof(nxt_router_schedules_t));
    sched = nxt_mp_zget(f->mp, sizeof(nxt_router_schedule_t));

    if (rtcf == NULL || sc == NULL || sched == NULL) {
        return NXT_ERROR;
    }

    rtcf->mem_pool = f->mp;
    rtcf->router = &f->router;
    rtcf->count = 1;                                /* a listener */

    rtcf->tstr_state = nxt_tstr_state_new(f->mp, 0);
    if (rtcf->tstr_state == NULL) {
        return NXT_ERROR;
    }

    f->rtcf = rtcf;

    if (nxt_router_schedules_joint_init(task, rtcf, sc, NULL) != NXT_OK) {
        return NXT_ERROR;
    }

    sched->name.start = (u_char *) name;
    sched->name.length = nxt_strlen(name);
    nxt_str_set(&sched->uri, "/cron?x=1");
    sched->interval = 60000;
    sched->timeout = timeout;

    /* The check also builds the field table of the run's parse. */

    if (nxt_router_schedule_request_check(f->mp, &sched->name, &sched->uri,
                                          NULL)
        != NXT_OK
        || nxt_router_schedule_request_build(f->mp, sched, NULL) != NXT_OK)
    {
        return NXT_ERROR;
    }

    sc->schedule = sched;
    sc->nschedules = 1;
    rtcf->schedules = sc;

    f->sc = sc;
    f->sched = sched;

    return NXT_OK;
}


static void
nxt_router_schedule_fixture_apply(nxt_thread_t *thr,
    nxt_router_schedule_fixture_t *f, nxt_router_conf_t *rtcf)
{
    nxt_router_temp_conf_t  tmcf;

    nxt_memzero(&tmcf, sizeof(nxt_router_temp_conf_t));
    tmcf.router_conf = rtcf;

    thr->engine = &f->main;
    nxt_router_schedules_apply(thr->task, &tmcf);
}


/*
 * The removal of the schedule: the joint goes on the worker engine, and the
 * rtcf keeps only the reference of the listener.
 */

static nxt_int_t
nxt_router_schedule_fixture_remove(nxt_thread_t *thr,
    nxt_router_schedule_fixture_t *f, const char *test)
{
    void        *objs[4];
    nxt_uint_t  nstates;

    nxt_router_schedule_fixture_apply(thr, f, &f->empty);

    (void) nxt_router_schedule_test_posted(thr, &f->worker, objs,
                                           nxt_nitems(objs));
    nxt_router_schedule_test_timers(thr, &f->main, 1);

    (void) nxt_router_schedules_status_size(&nstates);

    if (nstates != 0 || f->rtcf->count != 1) {
        nxt_log_alert(thr->log, "schedule %s: %ui states, rtcf %uD at the "
                      "end", test, nstates, f->rtcf->count);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * After a failure the state can stay on nxt_router_schedule_states, and
 * nxt_router_schedules_current can point to the freed pool.  This is
 * harmless: the test program then fails.
 */

static void
nxt_router_schedule_fixture_free(nxt_thread_t *thr,
    nxt_router_schedule_fixture_t *f)
{
    if (f->rtcf != NULL) {
        nxt_router_access_log_release(thr->task, &f->router.lock,
                                      f->rtcf->access_log);
        nxt_tstr_state_release(f->rtcf->tstr_state);
    }

    if (f->mp != NULL) {
        nxt_mp_destroy(f->mp);
    }

    thr->engine = f->saved_engine;
    nxt_router = f->saved_router;

    if (f->timers) {
        nxt_work_queue_cache_destroy(&f->cache);
        nxt_free(f->main.timers.changes);
        nxt_free(f->worker.timers.changes);
    }
}


/*
 * A run whose request parse fails, as on no memory, before it sets the
 * method, the path and the arguments (the NXT_TESTS hook).  The request
 * still goes through the access log, and its format reads all three:
 * "$method", "$uri" and "$arg_x".  The log is a pipe.  No memory is an
 * internal error: the run gets 500, and the validator reports no memory,
 * not a "uri" that does not make a request.
 */

static nxt_int_t
nxt_router_schedule_parse_fail_test(nxt_thread_t *thr)
{
    int                            fds[2];
    void                           *objs[4];
    u_char                         name[32], buf[256];
    ssize_t                        n;
    nxt_int_t                      ret, check;
    nxt_str_t                      json;
    nxt_conf_value_t               *value;
    nxt_router_conf_t              *rtcf;
    nxt_status_schedule_t          stat;
    nxt_router_schedule_fixture_t  f;

    /* An empty value is logged as "-". */
    static const char  expect[] = "[-] [-] [-]\n";

    ret = NXT_ERROR;
    fds[0] = -1;
    fds[1] = -1;

    if (nxt_router_schedule_fixture_init(thr, &f, "parse", 60000) != NXT_OK) {
        goto done;
    }

    rtcf = f.rtcf;

    nxt_str_set(&json, "{\"path\": \"/dev/null\", "
                       "\"format\": \"[$method] [$uri] [$arg_x]\"}");

    value = nxt_conf_json_parse_str(f.mp, &json);

    if (value == NULL
        || nxt_router_access_log_create(thr->task, rtcf, value) != NXT_OK
        || nxt_tstr_state_done(rtcf->tstr_state, NULL) != NXT_OK
        || pipe(fds) != 0
        || fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0)
    {
        nxt_log_alert(thr->log, "schedule parse failure: no access log");
        goto done;
    }

    rtcf->access_log->fd = fds[1];
    fds[1] = -1;

    nxt_router_schedule_fixture_apply(thr, &f, rtcf);

    nxt_router_schedule_test_timers(thr, &f.main, f.sched->interval + 1);

    /* The insert and the run; the run's parse fails. */

    nxt_router_schedule_test_parse_failures = 1;

    (void) nxt_router_schedule_test_posted(thr, &f.worker, objs,
                                           nxt_nitems(objs));

    nxt_router_schedule_test_parse_failures = 0;

    /* The error response, the access log, the close and the run's end. */

    nxt_router_schedule_test_timers(thr, &f.worker, 0);
    nxt_router_schedule_test_timers(thr, &f.worker, 1);

    n = read(fds[0], buf, sizeof(buf));

    if (n != (ssize_t) nxt_length(expect) || memcmp(buf, expect, n) != 0) {
        nxt_log_alert(thr->log, "schedule parse failure: the access log got "
                      "\"%*s\", expected \"%s\"", (size_t) nxt_max(n, 0), buf,
                      expect);
        goto done;
    }

    if (f.sc->joint.count != 1 || rtcf->count != 2) {
        nxt_log_alert(thr->log, "schedule parse failure: joint %uD, rtcf %uD "
                      "after the run", f.sc->joint.count, rtcf->count);
        goto done;
    }

    /* The result, on the main engine. */

    (void) nxt_router_schedule_test_posted(thr, &f.main, objs,
                                           nxt_nitems(objs));

    nxt_router_schedules_status(&stat, name + sizeof(name), name);

    if (stat.running != 0 || stat.runs != 1 || stat.failed != 1
        || stat.last_status != NXT_HTTP_INTERNAL_SERVER_ERROR)
    {
        nxt_log_alert(thr->log, "schedule parse failure: running %uD, runs "
                      "%uD, failed %uD, last status %uD after the run",
                      stat.running, stat.runs, stat.failed,
                      stat.last_status);
        goto done;
    }

    nxt_router_schedule_test_parse_failures = 1;

    check = nxt_router_schedule_request_check(f.mp, &f.sched->name,
                                              &f.sched->uri, NULL);

    nxt_router_schedule_test_parse_failures = 0;

    if (check != NXT_ERROR) {
        nxt_log_alert(thr->log, "schedule parse failure: the check returned "
                      "%i, expected NXT_ERROR", check);
        goto done;
    }

    ret = nxt_router_schedule_fixture_remove(thr, &f, "parse failure");

done:

    if (fds[0] != -1) {
        (void) close(fds[0]);
    }

    if (fds[1] != -1) {
        (void) close(fds[1]);
    }

    nxt_router_schedule_fixture_free(thr, &f);

    return ret;
}


/*
 * A run waits in the queue of its worker engine before the engine takes
 * it.  The wait counts in its "timeout" and in "last_duration_ms": both
 * start when the run is due on the main engine.  The test holds the run on
 * the main engine for 300 ms of real time, then lets the worker engine take
 * it.  The run's action keeps the request, as an application that does not
 * answer yet does, so the test can read the deadline that the run armed.
 */

#define NXT_SCHEDULE_TEST_WAIT     300
#define NXT_SCHEDULE_TEST_TIMEOUT  1000


static nxt_http_request_t  *nxt_router_schedule_test_held;


static nxt_http_action_t *
nxt_router_schedule_test_hold(nxt_task_t *task, nxt_http_request_t *r,
    nxt_http_action_t *action)
{
    nxt_router_schedule_test_held = r;

    return NULL;
}


static nxt_int_t
nxt_router_schedule_queue_wait_test(nxt_thread_t *thr)
{
    void                           *objs[4];
    u_char                         name[32];
    nxt_int_t                      ret;
    nxt_msec_t                     deadline;
    nxt_http_request_t             *r;
    nxt_status_schedule_t          stat;
    nxt_router_schedule_fixture_t  f;

    static nxt_http_action_t  hold = {
        .handler = nxt_router_schedule_test_hold,
    };

    ret = NXT_ERROR;
    nxt_router_schedule_test_held = NULL;

    if (nxt_router_schedule_fixture_init(thr, &f, "wait",
                                         NXT_SCHEDULE_TEST_TIMEOUT)
        != NXT_OK)
    {
        goto done;
    }

    f.sched->action = &hold;

    nxt_router_schedule_fixture_apply(thr, &f, f.rtcf);

    nxt_router_schedule_test_timers(thr, &f.main, f.sched->interval + 1);

    /* The run is due and posted; the worker engine is busy. */

    nxt_nanosleep(NXT_SCHEDULE_TEST_WAIT * 1000000ULL);
    nxt_thread_time_update(thr);

    (void) nxt_router_schedule_test_posted(thr, &f.worker, objs,
                                           nxt_nitems(objs));

    r = nxt_router_schedule_test_held;

    if (r == NULL) {
        nxt_log_alert(thr->log, "schedule queue wait: the run did not reach "
                      "its action");
        goto done;
    }

    /* The run's deadline is the only timer of the worker engine. */

    deadline = nxt_timer_find(&f.worker);

    if (deadline > NXT_SCHEDULE_TEST_TIMEOUT - NXT_SCHEDULE_TEST_WAIT
                   + NXT_TIMER_DEFAULT_BIAS)
    {
        nxt_log_alert(thr->log, "schedule queue wait: the deadline is %M ms "
                      "after a wait of %d ms, expected at most %d ms",
                      deadline, NXT_SCHEDULE_TEST_WAIT,
                      NXT_SCHEDULE_TEST_TIMEOUT - NXT_SCHEDULE_TEST_WAIT
                      + NXT_TIMER_DEFAULT_BIAS);
        goto done;
    }

    /* The application answers; the run ends. */

    nxt_http_request_error(&r->task, r, NXT_HTTP_NO_CONTENT);

    nxt_router_schedule_test_timers(thr, &f.worker, 0);
    nxt_router_schedule_test_timers(thr, &f.worker, 1);

    (void) nxt_router_schedule_test_posted(thr, &f.main, objs,
                                           nxt_nitems(objs));

    nxt_router_schedules_status(&stat, name + sizeof(name), name);

    if (stat.running != 0 || stat.runs != 1
        || stat.last_duration < NXT_SCHEDULE_TEST_WAIT)
    {
        nxt_log_alert(thr->log, "schedule queue wait: running %uD, runs %uD, "
                      "last duration %uD ms after a wait of %d ms",
                      stat.running, stat.runs, stat.last_duration,
                      NXT_SCHEDULE_TEST_WAIT);
        goto done;
    }

    ret = nxt_router_schedule_fixture_remove(thr, &f, "queue wait");

done:

    nxt_router_schedule_fixture_free(thr, &f);

    return ret;
}


/*
 * The skcf of the schedules takes "settings/http" from the parsed
 * configuration, as a listener's does (nxt_router_socket_conf_http()).
 * The parsed tree is in tmcf->mem_pool, which nxt_router_conf_ready() frees
 * once the configuration is applied, so a string must be copied into the
 * rtcf pool.  "body_temp_path" was not.  The test overwrites the string in
 * the tree, as a reuse of the freed memory would.
 */

static nxt_int_t
nxt_router_schedule_temp_path_test(nxt_thread_t *thr)
{
    nxt_mp_t                *mp, *tree_mp;
    nxt_int_t               ret;
    nxt_str_t               json, tree_path;
    nxt_conf_value_t        *http, *member;
    nxt_router_conf_t       *rtcf;
    nxt_router_schedules_t  *sc;

    static nxt_str_t  name = nxt_string("body_temp_path");
    static nxt_str_t  path = nxt_string("/var/tmp/unit-schedule-body");

    ret = NXT_ERROR;

    mp = nxt_mp_create(1024, 128, 256, 32);
    tree_mp = nxt_mp_create(1024, 128, 256, 32);

    if (mp == NULL || tree_mp == NULL) {
        goto done;
    }

    rtcf = nxt_mp_zget(mp, sizeof(nxt_router_conf_t));
    sc = nxt_mp_zget(mp, sizeof(nxt_router_schedules_t));

    nxt_str_set(&json, "{\"body_temp_path\": \"/var/tmp/unit-schedule-body\"}");

    http = nxt_conf_json_parse_str(tree_mp, &json);

    if (rtcf == NULL || sc == NULL || http == NULL) {
        goto done;
    }

    rtcf->mem_pool = mp;

    if (nxt_router_schedules_joint_init(thr->task, rtcf, sc, http) != NXT_OK) {
        goto done;
    }

    member = nxt_conf_get_object_member(http, &name, NULL);
    nxt_conf_get_string(member, &tree_path);
    nxt_memset(tree_path.start, 'X', tree_path.length);

    if (!nxt_strstr_eq(&sc->skcf.body_temp_path, &path)) {
        nxt_log_alert(thr->log, "schedule body_temp_path: \"%V\" after the "
                      "parsed configuration changed, expected \"%V\"",
                      &sc->skcf.body_temp_path, &path);
        goto done;
    }

    ret = NXT_OK;

done:

    if (tree_mp != NULL) {
        nxt_mp_destroy(tree_mp);
    }

    if (mp != NULL) {
        nxt_mp_destroy(mp);
    }

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
        || nxt_router_schedule_parse_fail_test(thr) != NXT_OK
        || nxt_router_schedule_queue_wait_test(thr) != NXT_OK
        || nxt_router_schedule_temp_path_test(thr) != NXT_OK
        || nxt_router_schedule_resolve_test(thr) != NXT_OK)
    {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router schedule test passed");

    return NXT_OK;
}
