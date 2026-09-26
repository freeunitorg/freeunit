
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * "schedules": the router issues a periodic GET to an application, with no
 * client connection.  See docs/adr/0004-schedules.md; section numbers below
 * refer to it.
 */

#include <nxt_router.h>
#include <nxt_conf.h>
#include <nxt_http.h>
#include <nxt_http_devnull.h>
#include <nxt_router_schedule.h>
#include <nxt_otel.h>


/* "run_on_start": the first run, about a second after the apply. */
#define NXT_SCHEDULE_START_DELAY  1000

/* A timed-out request a worker claimed but has not acknowledged yet. */
#define NXT_SCHEDULE_CLAIM_RETRY  1000


/* Survives reconfiguration (section 4).  Main engine only. */
typedef struct {
    nxt_queue_link_t         link;        /* nxt_router_schedule_states */

    nxt_router_schedule_t    *conf;       /* current, NULL once removed */
    nxt_router_schedules_t   *schedules;  /* the set "conf" belongs to */

    nxt_timer_t              timer;
    nxt_msec_t               base;        /* timers.now when last armed */

    /* The counters and "running"; the name is stored after the struct. */
    nxt_status_schedule_t    stat;
} nxt_router_schedule_state_t;


/*
 * One run: created on the main engine, run on the worker engine that holds
 * the joint, and freed on the main engine.
 */
typedef struct {
    nxt_http_devnull_t           devnull;    /* r->proto.any */

    nxt_router_schedule_state_t  *state;     /* main engine only */
    nxt_router_schedule_t        *sched;     /* valid while the joint is */
    nxt_router_schedules_t       *schedules;
    nxt_event_engine_t           *main;

    nxt_timer_t                  timer;      /* worker engine */
    nxt_work_t                   work;

    nxt_nsec_t                   started;
    nxt_msec_t                   duration;
    uint32_t                     seq;
    uint8_t                      timed_out;  /* 1 bit */
    uint8_t                      failed;     /* 1 bit */
} nxt_router_schedule_run_t;


typedef struct {
    nxt_str_t                pass;
    nxt_str_t                uri;
    int64_t                  interval;
    int64_t                  jitter;
    int64_t                  timeout;
    uint8_t                  run_on_start;
    nxt_conf_value_t         *headers;
} nxt_router_schedule_conf_t;


static nxt_int_t nxt_router_schedule_create(nxt_task_t *task,
    nxt_router_temp_conf_t *tmcf, nxt_str_t *name, nxt_conf_value_t *value,
    nxt_router_schedule_t *sched);
static void nxt_router_schedule_post(nxt_event_engine_t *engine,
    nxt_work_t *work, nxt_work_handler_t handler, void *obj);
static void nxt_router_schedule_timer_init(nxt_timer_t *timer,
    nxt_event_engine_t *engine, nxt_work_handler_t handler);
static void nxt_router_schedules_insert_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_router_schedules_release_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_router_schedule_update(nxt_task_t *task,
    nxt_router_schedules_t *sc, nxt_router_schedule_t *sched);
static void nxt_router_schedule_remove(nxt_task_t *task,
    nxt_router_schedule_state_t *state);
static void nxt_router_schedule_state_free(nxt_task_t *task,
    nxt_router_schedule_state_t *state);
static void nxt_router_schedule_state_free_handler(nxt_task_t *task,
    void *obj, void *data);
static nxt_msec_t nxt_router_schedule_delay(nxt_task_t *task,
    nxt_msec_t interval, nxt_msec_t jitter);
static void nxt_router_schedule_arm(nxt_event_engine_t *engine,
    nxt_router_schedule_state_t *state, nxt_msec_t delay);
static void nxt_router_schedule_timer_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_router_schedule_run(nxt_task_t *task, void *obj, void *data);
static nxt_int_t nxt_router_schedule_fields_init(void);
static nxt_int_t nxt_router_schedule_request_init(nxt_http_request_t *r,
    nxt_router_schedule_t *sched, nxt_bool_t discard_unsafe_fields);
static void nxt_router_schedule_run_timeout(nxt_task_t *task, void *obj,
    void *data);
static void nxt_router_schedule_run_close(nxt_task_t *task,
    nxt_http_devnull_t *dn, nxt_socket_conf_joint_t *joint);
static void nxt_router_schedule_run_finish(nxt_task_t *task, void *obj,
    void *data);
static void nxt_router_schedule_done(nxt_task_t *task, void *obj, void *data);


static nxt_conf_map_t  nxt_router_schedule_conf[] = {
    {
        nxt_string("pass"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_router_schedule_conf_t, pass),
    },

    {
        nxt_string("uri"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_router_schedule_conf_t, uri),
    },

    {
        nxt_string("interval"),
        NXT_CONF_MAP_INT64,
        offsetof(nxt_router_schedule_conf_t, interval),
    },

    {
        nxt_string("jitter"),
        NXT_CONF_MAP_INT64,
        offsetof(nxt_router_schedule_conf_t, jitter),
    },

    {
        nxt_string("timeout"),
        NXT_CONF_MAP_INT64,
        offsetof(nxt_router_schedule_conf_t, timeout),
    },

    {
        nxt_string("run_on_start"),
        NXT_CONF_MAP_INT8,
        offsetof(nxt_router_schedule_conf_t, run_on_start),
    },

    {
        nxt_string("headers"),
        NXT_CONF_MAP_PTR,
        offsetof(nxt_router_schedule_conf_t, headers),
    },
};


/*
 * The protocol-neutral request fields of the h1 table (src/nxt_h1proto.c).
 * The others write into h1 connection state; the validator refuses them.
 */
static nxt_lvlhsh_t             nxt_router_schedule_fields_hash;
static nxt_bool_t               nxt_router_schedule_fields_ready;

static nxt_http_field_proc_t    nxt_router_schedule_fields[] = {
    { nxt_string("Host"),              &nxt_http_request_host, 0 },
    { nxt_string("Cookie"),            &nxt_http_request_field,
        offsetof(nxt_http_request_t, cookie) },
    { nxt_string("Referer"),           &nxt_http_request_field,
        offsetof(nxt_http_request_t, referer) },
    { nxt_string("User-Agent"),        &nxt_http_request_field,
        offsetof(nxt_http_request_t, user_agent) },
    { nxt_string("Content-Type"),      &nxt_http_request_field,
        offsetof(nxt_http_request_t, content_type) },
    { nxt_string("Authorization"),     &nxt_http_request_field,
        offsetof(nxt_http_request_t, authorization) },
#if (NXT_HAVE_OTEL)
    { nxt_string("Traceparent"),       &nxt_otel_parse_traceparent, 0 },
    { nxt_string("Tracestate"),        &nxt_otel_parse_tracestate,  0 },
#endif
};


/* The set of the configuration applied last; its joint holds it. */
static nxt_router_schedules_t   *nxt_router_schedules_current;

static nxt_queue_t              nxt_router_schedule_states = {
    { &nxt_router_schedule_states.head, &nxt_router_schedule_states.head }
};


/*
 * Called from nxt_router_conf_create() in place of nxt_http_routes_resolve(),
 * after the applications, which a "pass" resolves against.  Everything here
 * comes from rtcf->mem_pool, so a failed configuration leaves nothing behind.
 */

nxt_int_t
nxt_router_conf_resolve(nxt_task_t *task, nxt_router_temp_conf_t *tmcf,
    nxt_conf_value_t *root)
{
    uint32_t                next;
    nxt_mp_t                *mp;
    nxt_int_t               ret;
    nxt_str_t               name;
    nxt_uint_t              i, n;
    nxt_conf_value_t        *schedules, *value;
    nxt_router_conf_t       *rtcf;
    nxt_router_schedules_t  *sc;

    static const nxt_str_t  schedules_path = nxt_string("/schedules");
    static const nxt_str_t  http_path = nxt_string("/settings/http");

    ret = nxt_http_routes_resolve(task, tmcf);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    schedules = nxt_conf_get_path(root, &schedules_path);

    n = (schedules != NULL) ? nxt_conf_object_members_count(schedules) : 0;
    if (n == 0) {
        return NXT_OK;
    }

    if (nxt_slow_path(nxt_router_schedule_fields_init() != NXT_OK)) {
        return NXT_ERROR;
    }

    rtcf = tmcf->router_conf;
    mp = rtcf->mem_pool;

    sc = nxt_mp_zget(mp, sizeof(nxt_router_schedules_t));
    if (nxt_slow_path(sc == NULL)) {
        return NXT_ERROR;
    }

    sc->schedule = nxt_mp_zget(mp, n * sizeof(nxt_router_schedule_t));
    if (nxt_slow_path(sc->schedule == NULL)) {
        return NXT_ERROR;
    }

    sc->nschedules = n;
    next = 0;

    for (i = 0; i < n; i++) {
        value = nxt_conf_next_object_member(schedules, &name, &next);

        ret = nxt_router_schedule_create(task, tmcf, &name, value,
                                         &sc->schedule[i]);
        if (nxt_slow_path(ret != NXT_OK)) {
            return ret;
        }
    }

    ret = nxt_router_schedules_joint_init(task, rtcf, sc,
                                          nxt_conf_get_path(root, &http_path));
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    rtcf->schedules = sc;

    nxt_debug(task, "router conf %p: %uD schedules", rtcf, sc->nschedules);

    return NXT_OK;
}


static nxt_int_t
nxt_router_schedule_fields_init(void)
{
    nxt_int_t  ret;

    if (nxt_router_schedule_fields_ready) {
        return NXT_OK;
    }

    ret = nxt_http_fields_hash(&nxt_router_schedule_fields_hash,
                               nxt_router_schedule_fields,
                               nxt_nitems(nxt_router_schedule_fields));
    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

    nxt_router_schedule_fields_ready = 1;

    return NXT_OK;
}


/* The configuration is validated: the values are in range. */

static nxt_int_t
nxt_router_schedule_create(nxt_task_t *task, nxt_router_temp_conf_t *tmcf,
    nxt_str_t *name, nxt_conf_value_t *value, nxt_router_schedule_t *sched)
{
    nxt_mp_t                    *mp;
    nxt_int_t                   ret;
    nxt_router_schedule_conf_t  scf;

    mp = tmcf->router_conf->mem_pool;

    nxt_memzero(&scf, sizeof(scf));

    ret = nxt_conf_map_object(tmcf->mem_pool, value, nxt_router_schedule_conf,
                              nxt_nitems(nxt_router_schedule_conf), &scf);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

    if (nxt_slow_path(nxt_str_dup(mp, &sched->name, name) == NULL
                      || nxt_str_dup(mp, &sched->uri, &scf.uri) == NULL))
    {
        return NXT_ERROR;
    }

    sched->interval = (nxt_msec_t) scf.interval * 1000;
    sched->jitter = (nxt_msec_t) scf.jitter * 1000;
    sched->timeout = (nxt_msec_t) (scf.timeout != 0 ? scf.timeout
                                                    : scf.interval) * 1000;
    sched->run_on_start = scf.run_on_start;

    sched->action = nxt_http_action_create(task, tmcf, &scf.pass);
    if (nxt_slow_path(sched->action == NULL)) {
        return NXT_ERROR;
    }

    return nxt_router_schedule_request_build(mp, sched, scf.headers);
}


/*
 * The internal socket configuration and joint (section 6.3): a run's
 * request needs a real r->conf, with what a listener would have.
 */

nxt_int_t
nxt_router_schedules_joint_init(nxt_task_t *task, nxt_router_conf_t *rtcf,
    nxt_router_schedules_t *sc, nxt_conf_value_t *http)
{
    nxt_socket_conf_t        *skcf;
    nxt_socket_conf_joint_t  *joint;

    static nxt_str_t  remote = nxt_string("127.0.0.1");
    static nxt_str_t  local = nxt_string("127.0.0.1:80");

    skcf = &sc->skcf;

    if (nxt_slow_path(nxt_router_socket_conf_http(rtcf->mem_pool, skcf, http)
                      != NXT_OK))
    {
        return NXT_ERROR;
    }

    sc->remote = nxt_sockaddr_parse_optport(rtcf->mem_pool, &remote);
    sc->local = nxt_sockaddr_parse(rtcf->mem_pool, &local);

    if (nxt_slow_path(sc->remote == NULL || sc->local == NULL)) {
        return NXT_ERROR;
    }

    /*
     * nxt_router_conf_release() unlinks both with nxt_queue_remove(): they
     * must be self-linked, and are on no list.
     */
    nxt_queue_self(&skcf->link);

    joint = &sc->joint;
    nxt_queue_self(&joint->link);
    joint->socket_conf = skcf;

    /*
     * As a listener's: the joint's own reference, the skcf's for the joint,
     * the rtcf's for the skcf.  A failed configuration destroys all three.
     */
    joint->count = 1;
    skcf->count = 1;
    skcf->router_conf = rtcf;
    rtcf->count++;

    return NXT_OK;
}


/*
 * Activation (section 4), on the main engine once the configuration can no
 * longer fail.  The new joint goes on the first worker engine's joints, so
 * that engine cannot exit under a run; router->engines lists only worker
 * engines, and the main engine's port would take a reply for a
 * configuration.  States are matched by name, so "running" carries over.
 * The previous joint's own reference is dropped on its engine: runs in
 * flight keep that configuration alive.
 */

void
nxt_router_schedules_apply(nxt_task_t *task, nxt_router_temp_conf_t *tmcf)
{
    nxt_uint_t                   i;
    nxt_queue_t                  *engines;
    nxt_event_engine_t           *engine;
    nxt_router_schedules_t       *sc, *old;
    nxt_router_schedule_state_t  *state;

    old = nxt_router_schedules_current;
    sc = tmcf->router_conf->schedules;

    if (sc != NULL) {
        engines = &tmcf->router_conf->router->engines;

        if (!nxt_queue_is_empty(engines)) {
            engine = nxt_queue_link_data(nxt_queue_first(engines),
                                         nxt_event_engine_t, link0);
            sc->joint.engine = engine;

            nxt_router_schedule_post(engine, &sc->insert_work,
                                     nxt_router_schedules_insert_handler,
                                     &sc->joint);

        } else {
            nxt_alert(task, "schedules: no worker engine, no run will start");
        }

        for (i = 0; i < sc->nschedules; i++) {
            nxt_router_schedule_update(task, sc, &sc->schedule[i]);
        }
    }

    nxt_router_schedules_current = sc;

    nxt_queue_each(state, &nxt_router_schedule_states,
                   nxt_router_schedule_state_t, link)
    {
        if (state->conf != NULL && state->schedules != sc) {
            nxt_router_schedule_remove(task, state);
        }

    } nxt_queue_loop;

    if (old == NULL) {
        return;
    }

    if (old->joint.engine == NULL) {
        nxt_router_conf_release(task, &old->joint);
        return;
    }

    /* Posted after the insert job, so it runs after it on that engine. */

    nxt_router_schedule_post(old->joint.engine, &old->release_work,
                             nxt_router_schedules_release_handler,
                             &old->joint);
}


static void
nxt_router_schedule_post(nxt_event_engine_t *engine, nxt_work_t *work,
    nxt_work_handler_t handler, void *obj)
{
    work->next = NULL;
    work->handler = handler;
    work->task = &engine->task;
    work->obj = obj;
    work->data = NULL;

    nxt_event_engine_post(engine, work);
}


static void
nxt_router_schedule_timer_init(nxt_timer_t *timer, nxt_event_engine_t *engine,
    nxt_work_handler_t handler)
{
    timer->bias = NXT_TIMER_DEFAULT_BIAS;
    timer->work_queue = &engine->fast_work_queue;
    timer->handler = handler;
    timer->task = &engine->task;
    timer->log = engine->task.log;
}


static void
nxt_router_schedules_insert_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_socket_conf_joint_t  *joint;

    joint = obj;

    nxt_debug(task, "schedules joint %p inserted", joint);

    nxt_queue_insert_tail(&task->thread->engine->joints, &joint->link);
}


static void
nxt_router_schedules_release_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_debug(task, "schedules joint %p released", obj);

    nxt_router_joint_release(task, obj);
}


static void
nxt_router_schedule_update(nxt_task_t *task, nxt_router_schedules_t *sc,
    nxt_router_schedule_t *sched)
{
    nxt_msec_t                   delay, elapsed;
    nxt_event_engine_t           *engine;
    nxt_router_schedule_t        *prev;
    nxt_router_schedule_state_t  *state;

    engine = task->thread->engine;

    nxt_queue_each(state, &nxt_router_schedule_states,
                   nxt_router_schedule_state_t, link)
    {
        if (nxt_strstr_eq(&state->stat.name, &sched->name)) {
            goto found;
        }

    } nxt_queue_loop;

    state = nxt_zalloc(sizeof(nxt_router_schedule_state_t)
                       + sched->name.length);
    if (nxt_slow_path(state == NULL)) {
        nxt_alert(task, "schedule \"%V\": no memory, it will not run",
                  &sched->name);
        return;
    }

    state->stat.name.start = (u_char *) (state + 1);
    state->stat.name.length = sched->name.length;
    nxt_memcpy(state->stat.name.start, sched->name.start, sched->name.length);

    nxt_router_schedule_timer_init(&state->timer, engine,
                                   nxt_router_schedule_timer_handler);

    nxt_queue_insert_tail(&nxt_router_schedule_states, &state->link);

found:

    prev = state->conf;

    state->conf = sched;
    state->schedules = sc;

    if (prev == NULL) {
        /* New, or removed while running and now back. */

        delay = nxt_router_schedule_delay(task, sched->run_on_start
                                                ? NXT_SCHEDULE_START_DELAY
                                                : sched->interval,
                                          sched->jitter);

        nxt_router_schedule_arm(engine, state, delay);

        nxt_log(task, NXT_LOG_INFO, "schedule \"%V\": first run in %M ms",
                &state->stat.name, delay);

        return;
    }

    if (prev->interval == sched->interval && prev->jitter == sched->jitter) {
        return;
    }

    /* The new wait counts from when the current one started. */

    delay = nxt_router_schedule_delay(task, sched->interval, sched->jitter);
    elapsed = nxt_max(nxt_msec_diff(engine->timers.now, state->base), 0);

    nxt_timer_add(engine, &state->timer,
                  (delay > elapsed) ? delay - elapsed : 0);
}


static void
nxt_router_schedule_remove(nxt_task_t *task, nxt_router_schedule_state_t *state)
{
    nxt_log(task, NXT_LOG_INFO, "schedule \"%V\": removed%s",
            &state->stat.name,
            state->stat.running ? ", the run in progress will complete" : "");

    state->conf = NULL;
    state->schedules = NULL;

    if (state->stat.running) {
        /* Freed by nxt_router_schedule_done(). */
        (void) nxt_timer_delete(task->thread->engine, &state->timer);
        return;
    }

    nxt_router_schedule_state_free(task, state);
}


/*
 * As nxt_router_free_app() does: while a timer change is still queued, the
 * memory goes with a zero-delay expiry instead.
 */

static void
nxt_router_schedule_state_free(nxt_task_t *task,
    nxt_router_schedule_state_t *state)
{
    nxt_event_engine_t  *engine;

    engine = task->thread->engine;

    nxt_queue_remove(&state->link);

    if (nxt_timer_delete(engine, &state->timer)) {
        state->timer.handler = nxt_router_schedule_state_free_handler;
        nxt_timer_add(engine, &state->timer, 0);
        return;
    }

    nxt_free(state);
}


static void
nxt_router_schedule_state_free_handler(nxt_task_t *task, void *obj,
    void *data)
{
    nxt_free(nxt_timer_data(obj, nxt_router_schedule_state_t, timer));
}


/* The validator keeps "interval" + "jitter" below 2^31 ms. */

static nxt_msec_t
nxt_router_schedule_delay(nxt_task_t *task, nxt_msec_t interval,
    nxt_msec_t jitter)
{
    return interval + nxt_random(&task->thread->random) % (jitter + 1);
}


static void
nxt_router_schedule_arm(nxt_event_engine_t *engine,
    nxt_router_schedule_state_t *state, nxt_msec_t delay)
{
    state->base = engine->timers.now;

    nxt_timer_add(engine, &state->timer, delay);
}


/* Due (section 5): skip or post a run to the joint's worker engine. */

static void
nxt_router_schedule_timer_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_realtime_t               now;
    nxt_event_engine_t           *engine;
    nxt_router_schedule_t        *sched;
    nxt_router_schedule_run_t    *run;
    nxt_router_schedule_state_t  *state;

    state = nxt_timer_data(obj, nxt_router_schedule_state_t, timer);

    sched = state->conf;
    if (nxt_slow_path(sched == NULL)) {
        return;
    }

    nxt_router_schedule_arm(task->thread->engine, state,
                            nxt_router_schedule_delay(task, sched->interval,
                                                      sched->jitter));

    if (state->stat.running) {
        state->stat.skipped++;

        nxt_log(task, NXT_LOG_WARN, "schedule \"%V\": run skipped, the "
                "previous one is still running", &state->stat.name);
        return;
    }

    engine = state->schedules->joint.engine;

    run = (engine != NULL) ? nxt_zalloc(sizeof(nxt_router_schedule_run_t))
                           : NULL;
    if (nxt_slow_path(run == NULL)) {
        state->stat.failed++;
        return;
    }

    run->state = state;
    run->sched = sched;
    run->schedules = state->schedules;
    run->main = task->thread->engine;
    run->seq = ++state->stat.runs;

    state->stat.running = 1;

    nxt_realtime(&now);
    state->stat.last_start = now.sec;

    nxt_debug(task, "schedule \"%V\": run %uD posted to engine %p",
              &state->stat.name, run->seq, engine);

    nxt_router_schedule_post(engine, &run->work, nxt_router_schedule_run,
                             run);
}


/*
 * The run, on the worker engine (section 6.5): a devnull request with the
 * internal joint as r->conf, its header parsed as a client's would be, and
 * the schedule's own "pass" action.
 */

static void
nxt_router_schedule_run(nxt_task_t *task, void *obj, void *data)
{
    nxt_int_t                  ret;
    nxt_event_engine_t         *engine;
    nxt_http_request_t         *r;
    nxt_router_schedules_t     *sc;
    nxt_router_schedule_run_t  *run;

    run = obj;
    sc = run->schedules;
    engine = task->thread->engine;

    /* The run's reference; nxt_router_schedule_run_finish() drops it. */
    sc->joint.count++;

    run->started = nxt_thread_monotonic_time(task->thread);
    run->devnull.close = nxt_router_schedule_run_close;

    nxt_router_schedule_timer_init(&run->timer, engine,
                                   nxt_router_schedule_run_timeout);

    r = nxt_http_request_create(task);
    if (nxt_slow_path(r == NULL)) {
        run->timer.handler = nxt_router_schedule_run_finish;
        nxt_timer_add(engine, &run->timer, 0);
        return;
    }

    r->task = *task;
    task = &r->task;

    r->conf = &sc->joint;
    r->protocol = NXT_HTTP_PROTO_DEVNULL;
    r->proto.any = &run->devnull;
    run->devnull.request = r;

    r->remote = sc->remote;
    r->local = sc->local;
    r->log_route = sc->skcf.log_route;

    /* The schedule's deadline; r->timer carries the application's. */
    nxt_timer_add(engine, &run->timer, run->sched->timeout);

    ret = nxt_router_schedule_request_init(r, run->sched,
                                           sc->skcf.discard_unsafe_fields);
    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_http_request_error(task, r, (ret > 0) ? (nxt_http_status_t) ret
                                        : NXT_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    /*
     * The span transitions a client's request makes on its way to the
     * action: nxt_h1p_conn_request_header_parse(), nxt_http_request_start()
     * and nxt_http_request_ready().  The devnull header send collects it.
     */
    NXT_OTEL_TRACE();
    NXT_OTEL_TRACE();
    NXT_OTEL_TRACE();

    nxt_http_request_action(task, r, run->sched->action);
}


/* The configured request header, then the copy nxt_h1p_header_process() does. */

static nxt_int_t
nxt_router_schedule_request_init(nxt_http_request_t *r,
    nxt_router_schedule_t *sched, nxt_bool_t discard_unsafe_fields)
{
    nxt_http_request_parse_t  *rp;

    rp = nxt_mp_zget(r->mem_pool, sizeof(nxt_http_request_parse_t));
    if (nxt_slow_path(rp == NULL)) {
        return NXT_ERROR;
    }

    rp->discard_unsafe_fields = discard_unsafe_fields;

    if (nxt_router_schedule_parse(r->mem_pool, &sched->request, rp)
        != NXT_DONE)
    {
        return NXT_HTTP_BAD_REQUEST;
    }

    r->request_line.start = rp->method.start;
    r->request_line.length = rp->request_line_end - rp->method.start;

    r->target.start = rp->target_start;
    r->target.length = rp->target_end - rp->target_start;
    r->quoted_target = rp->quoted_target;

    r->version.start = rp->version.str;
    r->version.length = sizeof(rp->version.str);

    r->method = &rp->method;
    r->path = &rp->path;
    r->args = &rp->args;

    r->num_inline_fields = rp->num_inline_fields;
    nxt_memcpy(r->inline_fields, rp->inline_fields,
               sizeof(nxt_http_field_t) * r->num_inline_fields);
    r->fields = rp->fields;

    return nxt_http_fields_process(r->inline_fields, r->num_inline_fields,
                                   r->fields, &nxt_router_schedule_fields_hash,
                                   r);
}


/*
 * The schedule's "timeout" (section 7.2), through the same function as
 * "limits": {"timeout"}: a claimed request is left until it is
 * acknowledged, and the application is not stopped.
 */

static void
nxt_router_schedule_run_timeout(nxt_task_t *task, void *obj, void *data)
{
    nxt_http_request_t         *r;
    nxt_router_schedule_run_t  *run;

    run = nxt_timer_data(obj, nxt_router_schedule_run_t, timer);

    r = run->devnull.request;

    if (r == NULL || r->req_rpc_data == NULL) {
        return;
    }

    if (!nxt_router_request_expire(task, r, r->req_rpc_data)) {
        nxt_timer_add(task->thread->engine, &run->timer,
                      NXT_SCHEDULE_CLAIM_RETRY);
        return;
    }

    run->timed_out = 1;
}


/*
 * The devnull close (section 6.4).  The request pool goes as soon as this
 * returns, so the rest waits for a zero-delay expiry of the run's timer.
 */

static void
nxt_router_schedule_run_close(nxt_task_t *task, nxt_http_devnull_t *dn,
    nxt_socket_conf_joint_t *joint)
{
    nxt_router_schedule_run_t  *run;

    run = (nxt_router_schedule_run_t *) dn;

    run->duration = (nxt_msec_t) ((nxt_thread_monotonic_time(task->thread)
                                   - run->started) / 1000000);

    run->timer.handler = nxt_router_schedule_run_finish;
    nxt_timer_add(task->thread->engine, &run->timer, 0);
}


/*
 * Log the result while the configuration is still held, post it to the main
 * engine, which frees the run, then drop the run's joint reference, which
 * may free the configuration and end the thread.
 */

static void
nxt_router_schedule_run_finish(nxt_task_t *task, void *obj, void *data)
{
    size_t                     i, n;
    u_char                     *p;
    nxt_http_status_t          status;
    nxt_router_schedule_t      *sched;
    nxt_socket_conf_joint_t    *joint;
    nxt_router_schedule_run_t  *run;

    run = nxt_timer_data(obj, nxt_router_schedule_run_t, timer);

    sched = run->sched;
    joint = &run->schedules->joint;
    status = run->devnull.status;
    n = nxt_router_schedule_uri_public(&sched->uri);

    if (run->timed_out) {
        nxt_log(task, NXT_LOG_WARN, "schedule \"%V\" run %uD: GET %*s%s "
                "timed out after %M ms", &sched->name, run->seq, n,
                sched->uri.start, (n < sched->uri.length) ? "..." : "",
                run->duration);

    } else if (status == 0 || status >= NXT_HTTP_BAD_REQUEST
               || run->devnull.discarded)
    {
        run->failed = 1;

        p = run->devnull.head;

        for (i = 0; i < run->devnull.head_length; i++) {
            if (p[i] < 0x20 || p[i] >= 0x7F) {
                p[i] = '.';
            }
        }

        nxt_log(task, NXT_LOG_WARN, "schedule \"%V\" run %uD: GET %*s%s -> "
                "%d in %M ms: \"%*s\"", &sched->name, run->seq, n,
                sched->uri.start, (n < sched->uri.length) ? "..." : "",
                (int) status, run->duration, run->devnull.head_length, p);

    } else {
        nxt_log(task, NXT_LOG_INFO, "schedule \"%V\" run %uD: GET %*s%s -> "
                "%d in %M ms", &sched->name, run->seq, n, sched->uri.start,
                (n < sched->uri.length) ? "..." : "", (int) status,
                run->duration);
    }

    nxt_router_schedule_post(run->main, &run->work, nxt_router_schedule_done,
                             run);

    nxt_router_joint_release(task, joint);
}


/* The result, on the main engine (section 7). */

static void
nxt_router_schedule_done(nxt_task_t *task, void *obj, void *data)
{
    nxt_router_schedule_run_t    *run;
    nxt_router_schedule_state_t  *state;

    run = obj;
    state = run->state;

    state->stat.running = 0;
    state->stat.last_status = run->devnull.status;
    state->stat.last_duration = run->duration;
    state->stat.timed_out += run->timed_out;
    state->stat.failed += run->failed;

    nxt_free(run);

    if (state->conf == NULL) {
        nxt_router_schedule_state_free(task, state);
    }
}


/*
 * How much of "uri" an info-level log line may show: a cron URI usually
 * carries its key in the last path segment or in the query, so only the
 * path up to its last "/".
 */

size_t
nxt_router_schedule_uri_public(const nxt_str_t *uri)
{
    u_char  *query;
    size_t  n;

    query = memchr(uri->start, '?', uri->length);
    n = (query != NULL) ? (size_t) (query - uri->start) : uri->length;

    while (n > 0 && uri->start[n - 1] != '/') {
        n--;
    }

    return n;
}


/*
 * "GET <uri> HTTP/1.1", the headers, and "User-Agent:
 * FreeUnit-Schedule/<name>" unless one is configured.  Built once; each run
 * parses a copy, so it gets the target normalisation a client request would.
 * The validator bounds every length.
 */

nxt_int_t
nxt_router_schedule_request_build(nxt_mp_t *mp, nxt_router_schedule_t *sched,
    nxt_conf_value_t *headers)
{
    u_char            *p, *end;
    size_t            size;
    uint32_t          next;
    nxt_str_t         name, value;
    nxt_bool_t        user_agent;
    nxt_conf_value_t  *member;

    static const nxt_str_t  ua = nxt_string("User-Agent");

    size = sizeof("GET  HTTP/1.1\r\nUser-Agent: FreeUnit-Schedule/\r\n\r\n")
           + sched->uri.length + sched->name.length;
    user_agent = 0;

    for (next = 0; headers != NULL; /* void */) {
        member = nxt_conf_next_object_member(headers, &name, &next);
        if (member == NULL) {
            break;
        }

        nxt_conf_get_string(member, &value);

        size += name.length + nxt_length(": \r\n") + value.length;
        user_agent |= nxt_strcasestr_eq(&name, &ua);
    }

    p = nxt_mp_nget(mp, size);
    if (nxt_slow_path(p == NULL)) {
        return NXT_ERROR;
    }

    sched->request.start = p;
    end = p + size;

    p = nxt_sprintf(p, end, "GET %V HTTP/1.1\r\n", &sched->uri);

    for (next = 0; headers != NULL; /* void */) {
        member = nxt_conf_next_object_member(headers, &name, &next);
        if (member == NULL) {
            break;
        }

        nxt_conf_get_string(member, &value);

        p = nxt_sprintf(p, end, "%V: %V\r\n", &name, &value);
    }

    if (!user_agent) {
        p = nxt_sprintf(p, end, "User-Agent: FreeUnit-Schedule/%V\r\n",
                        &sched->name);
    }

    p = nxt_sprintf(p, end, "\r\n");

    sched->request.length = p - sched->request.start;

    return NXT_OK;
}


/*
 * Parse a copy of "request": the parser may rewrite a complex target in
 * place.  "rp" is zeroed by the caller.  Returns NXT_DONE on success.
 */

nxt_int_t
nxt_router_schedule_parse(nxt_mp_t *mp, nxt_str_t *request,
    nxt_http_request_parse_t *rp)
{
    u_char         *p;
    nxt_buf_mem_t  mem;

    p = nxt_mp_nget(mp, request->length);
    if (nxt_slow_path(p == NULL)) {
        return NXT_ERROR;
    }

    nxt_memcpy(p, request->start, request->length);

    mem.start = p;
    mem.pos = p;
    mem.free = p + request->length;
    mem.end = mem.free;

    (void) nxt_http_parse_request_init(rp, mp);

    return nxt_http_parse_request(rp, &mem);
}


/*
 * For the validator: the request a run would make, built and taken in as
 * nxt_router_schedule_run() does.  What the run would refuse with 400 -- a
 * "Host" that is not a host name, a field given twice, a name over 255
 * bytes -- is refused at configuration time instead.  Returns NXT_DECLINED
 * for such a request.
 */

nxt_int_t
nxt_router_schedule_request_check(nxt_mp_t *mp, nxt_str_t *name,
    nxt_str_t *uri, nxt_conf_value_t *headers)
{
    nxt_int_t              ret;
    nxt_http_request_t     r;
    nxt_router_schedule_t  sched;

    nxt_memzero(&sched, sizeof(nxt_router_schedule_t));
    sched.name = *name;
    sched.uri = *uri;

    if (nxt_slow_path(nxt_router_schedule_fields_init() != NXT_OK
                      || nxt_router_schedule_request_build(mp, &sched, headers)
                         != NXT_OK))
    {
        return NXT_ERROR;
    }

    nxt_memzero(&r, sizeof(nxt_http_request_t));
    r.mem_pool = mp;

    ret = nxt_router_schedule_request_init(&r, &sched, 0);

    return (ret == NXT_OK || ret == NXT_ERROR) ? ret : NXT_DECLINED;
}


/*
 * /status (docs/unit-openapi.yaml, statusSchedules), on the main engine
 * like the states themselves.  A removed schedule whose last run is still
 * in flight is included until that run ends.
 */

size_t
nxt_router_schedules_status_size(nxt_uint_t *n)
{
    size_t                       size;
    nxt_router_schedule_state_t  *state;

    *n = 0;
    size = 0;

    nxt_queue_each(state, &nxt_router_schedule_states,
                   nxt_router_schedule_state_t, link)
    {
        (*n)++;
        size += sizeof(nxt_status_schedule_t) + state->stat.name.length;

    } nxt_queue_loop;

    return size;
}


/* Names are copied down from "p"; their offsets are relative to "base". */

void
nxt_router_schedules_status(nxt_status_schedule_t *stat, u_char *p,
    u_char *base)
{
    nxt_router_schedule_state_t  *state;

    nxt_queue_each(state, &nxt_router_schedule_states,
                   nxt_router_schedule_state_t, link)
    {
        p -= state->stat.name.length;
        nxt_memcpy(p, state->stat.name.start, state->stat.name.length);

        *stat = state->stat;
        stat->name.start = (u_char *) (p - base);
        stat++;

    } nxt_queue_loop;
}
