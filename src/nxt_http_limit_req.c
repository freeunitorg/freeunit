
/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_http_limit_req.h>


typedef struct {
    nxt_str_t                   zone;
    int64_t                     rate;
    int64_t                     burst;
    nxt_str_t                   stage;
} nxt_http_limit_req_conf_t;


static nxt_http_limit_zone_t *nxt_http_limit_zone_get(nxt_task_t *task,
    nxt_router_t *router, nxt_str_t *name, nxt_http_limit_zone_conf_t *zcf);
static void nxt_http_limit_zone_release(nxt_task_t *task,
    nxt_thread_spinlock_t *lock, nxt_http_limit_zone_t *zone);
static nxt_http_limit_zone_conf_t *nxt_http_limit_zone_find(
    nxt_router_conf_t *rtcf, nxt_str_t *name);
static nxt_int_t nxt_http_limit_req_reject(nxt_task_t *task,
    nxt_http_request_t *r, nxt_http_limit_req_t *limit, uint64_t wait_ms);


static nxt_conf_map_t  nxt_http_limit_req_conf[] = {
    {
        nxt_string("zone"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_http_limit_req_conf_t, zone),
    },

    {
        nxt_string("rate"),
        NXT_CONF_MAP_INT64,
        offsetof(nxt_http_limit_req_conf_t, rate),
    },

    {
        nxt_string("burst"),
        NXT_CONF_MAP_INT64,
        offsetof(nxt_http_limit_req_conf_t, burst),
    },

    {
        nxt_string("stage"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_http_limit_req_conf_t, stage),
    },
};


/*
 * "name" or "name:keys".  NXT_ERROR: the form is wrong.  NXT_DECLINED: the
 * size is not an integer from 1 to NXT_LIMIT_KEYS_MAX.
 */

nxt_int_t
nxt_http_limit_req_zone_parse(const nxt_str_t *zone, nxt_str_t *name,
    uint32_t *keys_max)
{
    u_char     *p;
    size_t     length;
    nxt_int_t  n;

    p = memchr(zone->start, ':', zone->length);

    if (p == NULL) {
        *name = *zone;
        *keys_max = NXT_HTTP_LIMIT_REQ_KEYS_DEFAULT;

    } else {
        name->start = zone->start;
        name->length = p - zone->start;

        p++;
        length = zone->start + zone->length - p;

        n = (length != 0) ? nxt_int_parse(p, length) : -1;

        if (n < 1 || n > NXT_LIMIT_KEYS_MAX) {
            return NXT_DECLINED;
        }

        *keys_max = n;
    }

    if (name->length == 0) {
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * The key: the client address without its port, IPv4 as /32 and IPv6 as
 * /64.  The lengths differ, so the families cannot collide.  A UNIX socket
 * has no client address: all its clients share one bucket.
 */

size_t
nxt_http_limit_req_key(const nxt_sockaddr_t *sa, u_char *key)
{
    switch (sa->u.sockaddr.sa_family) {

    case AF_INET:
        nxt_memcpy(key, &sa->u.sockaddr_in.sin_addr, 4);
        return 4;

#if (NXT_INET6)
    case AF_INET6:
        nxt_memcpy(key, &sa->u.sockaddr_in6.sin6_addr, 8);
        return 8;
#endif

    default:
        key[0] = 0;
        return 1;
    }
}


/* The router configuration. */

/*
 * A zone stays alive while an old configuration has requests, so the reuse
 * looks at all live zones, as the access log does.  The match is on the
 * name and the size: a zone of the same name and another size is a new
 * object, and the old one is marked when the configuration is applied.
 * Only the router main thread adds a zone.
 */

static nxt_http_limit_zone_t *
nxt_http_limit_zone_get(nxt_task_t *task, nxt_router_t *router,
    nxt_str_t *name, nxt_http_limit_zone_conf_t *zcf)
{
    nxt_mp_t               *mp;
    nxt_queue_link_t       *lnk;
    nxt_http_limit_zone_t  *zone;

    nxt_thread_spin_lock(&router->lock);

    for (lnk = nxt_queue_first(&router->limit_zones);
         lnk != nxt_queue_tail(&router->limit_zones);
         lnk = nxt_queue_next(lnk))
    {
        zone = nxt_queue_link_data(lnk, nxt_http_limit_zone_t, link);

        if (nxt_strstr_eq(name, &zone->name)
            && zone->zone->keys_max == zcf->keys_max)
        {
            zone->count++;

            nxt_thread_spin_unlock(&router->lock);

            return zone;
        }
    }

    nxt_thread_spin_unlock(&router->lock);

    /* The pool is used by this thread only, to create and to destroy. */

    mp = nxt_mp_create(32768, 128, 4096, 64);
    if (nxt_slow_path(mp == NULL)) {
        return NULL;
    }

    zone = nxt_mp_zget(mp, sizeof(nxt_http_limit_zone_t) + name->length);
    if (nxt_slow_path(zone == NULL)) {
        goto fail;
    }

    zone->zone = nxt_limit_zone_create(mp, zcf->keys_max, zcf->rate,
                                       zcf->burst);
    if (nxt_slow_path(zone->zone == NULL)) {
        goto fail;
    }

    zone->mp = mp;
    zone->count = 1;

    zone->name.length = name->length;
    zone->name.start = nxt_pointer_to(zone, sizeof(nxt_http_limit_zone_t));

    nxt_memcpy(zone->name.start, name->start, name->length);

    nxt_thread_spin_lock(&router->lock);

    nxt_queue_insert_tail(&router->limit_zones, &zone->link);

    nxt_thread_spin_unlock(&router->lock);

    return zone;

fail:

    nxt_alert(task, "failed to allocate the limit_req zone \"%V\" of %uD "
              "keys", name, zcf->keys_max);

    nxt_mp_destroy(mp);

    return NULL;
}


static void
nxt_http_limit_zone_release(nxt_task_t *task, nxt_thread_spinlock_t *lock,
    nxt_http_limit_zone_t *zone)
{
    nxt_thread_spin_lock(lock);

    if (--zone->count != 0) {
        zone = NULL;

    } else {
        nxt_queue_remove(&zone->link);
    }

    nxt_thread_spin_unlock(lock);

    if (zone != NULL) {
        /*
         * The last configuration that used the zone may end on a router
         * worker thread, as nxt_router_conf_release() does; the pool was
         * created by the main thread.
         */
        nxt_mp_thread_adopt(zone->mp);
        nxt_mp_destroy(zone->mp);
    }
}


static nxt_http_limit_zone_conf_t *
nxt_http_limit_zone_find(nxt_router_conf_t *rtcf, nxt_str_t *name)
{
    nxt_http_limit_zone_conf_t  *zcf, *end;

    if (rtcf->limit_zones == NULL) {
        return NULL;
    }

    zcf = rtcf->limit_zones->elts;
    end = zcf + rtcf->limit_zones->nelts;

    for ( ; zcf < end; zcf++) {
        if (nxt_strstr_eq(&zcf->zone->name, name)) {
            return zcf;
        }
    }

    return NULL;
}


nxt_int_t
nxt_http_limit_req_init(nxt_task_t *task, nxt_router_temp_conf_t *tmcf,
    nxt_http_action_t *action, nxt_conf_value_t *cv)
{
    uint32_t                    keys_max;
    nxt_str_t                   name;
    nxt_int_t                   ret;
    nxt_router_conf_t           *rtcf;
    nxt_http_limit_req_t        *limit;
    nxt_http_limit_req_conf_t   lrcf;
    nxt_http_limit_zone_conf_t  *zcf;

    nxt_memzero(&lrcf, sizeof(nxt_http_limit_req_conf_t));

    lrcf.rate = 1;
    lrcf.burst = 1;

    ret = nxt_conf_map_object(tmcf->mem_pool, cv, nxt_http_limit_req_conf,
                              nxt_nitems(nxt_http_limit_req_conf), &lrcf);
    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_alert(task, "limit_req map error");
        return NXT_ERROR;
    }

    ret = nxt_http_limit_req_zone_parse(&lrcf.zone, &name, &keys_max);
    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_alert(task, "limit_req zone \"%V\" is invalid", &lrcf.zone);
        return NXT_ERROR;
    }

    /*
     * The validator refuses these; a restored configuration may hold
     * them, so they are clamped rather than trusted.
     */

    lrcf.rate = nxt_max(lrcf.rate, 1);
    lrcf.rate = nxt_min(lrcf.rate, NXT_LIMIT_RATE_MAX);
    lrcf.burst = nxt_max(lrcf.burst, 1);
    lrcf.burst = nxt_min(lrcf.burst, NXT_LIMIT_BURST_MAX);

    rtcf = tmcf->router_conf;

    zcf = nxt_http_limit_zone_find(rtcf, &name);

    if (zcf != NULL) {
        /* One zone, one set of parameters; the validator says so first. */

        if (zcf->keys_max != keys_max || zcf->rate != lrcf.rate
            || zcf->burst != lrcf.burst)
        {
            nxt_alert(task, "limit_req zone \"%V\" is used with other "
                      "parameters elsewhere", &name);
            return NXT_ERROR;
        }

    } else {
        if (rtcf->limit_zones == NULL) {
            rtcf->limit_zones = nxt_array_create(rtcf->mem_pool, 2,
                                        sizeof(nxt_http_limit_zone_conf_t));
            if (nxt_slow_path(rtcf->limit_zones == NULL)) {
                return NXT_ERROR;
            }
        }

        zcf = nxt_array_zero_add(rtcf->limit_zones);
        if (nxt_slow_path(zcf == NULL)) {
            return NXT_ERROR;
        }

        zcf->keys_max = keys_max;
        zcf->rate = lrcf.rate;
        zcf->burst = lrcf.burst;

        zcf->zone = nxt_http_limit_zone_get(task, rtcf->router, &name, zcf);
        if (nxt_slow_path(zcf->zone == NULL)) {
            /* The slot stays: the release skips it. */
            return NXT_ERROR;
        }
    }

    limit = nxt_mp_zalloc(rtcf->mem_pool, sizeof(nxt_http_limit_req_t));
    if (nxt_slow_path(limit == NULL)) {
        return NXT_ERROR;
    }

    limit->zone = zcf->zone;
    limit->stage = nxt_str_eq(&lrcf.stage, "body", 4)
                   ? NXT_HTTP_LIMIT_REQ_BODY : NXT_HTTP_LIMIT_REQ_HEADERS;

    action->limit_req = limit;

    return NXT_OK;
}


/*
 * The configuration is accepted: a new "rate" or "burst" applies in place
 * to the live buckets of a zone it shares with an older configuration,
 * and a zone of the same name that an older configuration keeps with
 * another size leaves the status report.
 */

void
nxt_http_limit_req_zones_apply(nxt_task_t *task, nxt_router_conf_t *rtcf)
{
    nxt_router_t                *router;
    nxt_queue_link_t            *lnk;
    nxt_http_limit_zone_t       *zone, *other;
    nxt_http_limit_zone_conf_t  *zcf, *end;

    if (rtcf->limit_zones == NULL) {
        return;
    }

    router = rtcf->router;

    zcf = rtcf->limit_zones->elts;
    end = zcf + rtcf->limit_zones->nelts;

    for ( ; zcf < end; zcf++) {
        zone = zcf->zone;

        nxt_thread_spin_lock(&zone->zone->lock);

        zone->zone->rate = zcf->rate;
        zone->zone->burst = zcf->burst;

        nxt_thread_spin_unlock(&zone->zone->lock);

        nxt_thread_spin_lock(&router->lock);

        zone->superseded = 0;

        for (lnk = nxt_queue_first(&router->limit_zones);
             lnk != nxt_queue_tail(&router->limit_zones);
             lnk = nxt_queue_next(lnk))
        {
            other = nxt_queue_link_data(lnk, nxt_http_limit_zone_t, link);

            if (other != zone && nxt_strstr_eq(&other->name, &zone->name)) {
                other->superseded = 1;
            }
        }

        nxt_thread_spin_unlock(&router->lock);
    }
}


void
nxt_http_limit_req_zones_release(nxt_task_t *task,
    nxt_thread_spinlock_t *lock, nxt_router_conf_t *rtcf)
{
    nxt_http_limit_zone_conf_t  *zcf, *end;

    if (rtcf->limit_zones == NULL) {
        return;
    }

    zcf = rtcf->limit_zones->elts;
    end = zcf + rtcf->limit_zones->nelts;

    for ( ; zcf < end; zcf++) {
        if (zcf->zone != NULL) {
            nxt_http_limit_zone_release(task, lock, zcf->zone);
        }
    }

    rtcf->limit_zones = NULL;
}


/* The request path. */

static nxt_int_t
nxt_http_limit_req_reject(nxt_task_t *task, nxt_http_request_t *r,
    nxt_http_limit_req_t *limit, uint64_t wait_ms)
{
    uint32_t  retry_after;

    static nxt_log_moderation_t  nxt_http_limit_req_log_moderation = {
        NXT_LOG_INFO, 2, "limit_req rejected requests", NXT_LOG_MODERATION
    };

    nxt_log_moderate(&nxt_http_limit_req_log_moderation, NXT_LOG_INFO,
                     task->log, "limit_req: zone \"%V\" rejected a request "
                     "from %*s", &limit->zone->name,
                     (size_t) r->remote->address_length,
                     nxt_sockaddr_address(r->remote));

    /*
     * The headers stage answers before the body is read.  A body that is
     * on its way cannot be parsed as the next request, so the connection
     * is closed after the response rather than drained.
     */

    if (r->body == NULL
        && (r->content_length_n > 0 || r->chunked_field != NULL))
    {
        r->inconsistent = 1;
    }

    /* The time until one request fits, in whole seconds, at least 1. */

    retry_after = (uint32_t) nxt_min((wait_ms + 999) / 1000,
                                     (uint64_t) NXT_INT32_T_MAX);

    nxt_http_request_error_retry_after(task, r, NXT_HTTP_TOO_MANY_REQUESTS,
                                       nxt_max(retry_after, 1u));

    return NXT_DECLINED;
}


/*
 * One request is counted once, by the first limit on its path, whatever
 * the stage: the action chain is entered again by every "pass", and the
 * headers stage looks at the chain before the body stage does.
 */

nxt_int_t
nxt_http_limit_req_check(nxt_task_t *task, nxt_http_request_t *r,
    nxt_http_limit_req_t *limit)
{
    size_t      len;
    u_char      key[NXT_LIMIT_KEY_MAX];
    uint64_t    now_ms, wait_ms;
    nxt_bool_t  allow;

    if (r->limited) {
        return NXT_OK;
    }

    r->limited = 1;

    len = nxt_http_limit_req_key(r->remote, key);

    now_ms = nxt_thread_monotonic_time(task->thread) / 1000000;

    allow = nxt_limit_zone_check(limit->zone->zone, key, len, now_ms,
                                 &wait_ms);

    nxt_debug(task, "http limit_req \"%V\": %s", &limit->zone->name,
              allow ? "pass" : "reject");

    if (nxt_fast_path(allow)) {
        return NXT_OK;
    }

    return nxt_http_limit_req_reject(task, r, limit, wait_ms);
}


/*
 * The headers stage.  The routes are matched before the body is read, to
 * find the first limit on the path of the request.  The walk follows the
 * routes only: it stops at an action that is not a route, at one that
 * rewrites the URI, because the next route would match the new URI, and at
 * a "pass" with a variable.  A headers stage limit behind such an action
 * takes effect at the body stage, which looks at every action.
 */

nxt_int_t
nxt_http_limit_req_headers(nxt_task_t *task, nxt_http_request_t *r)
{
    nxt_uint_t            n;
    nxt_http_action_t     *action;
    nxt_http_limit_req_t  *limit;

    action = r->conf->socket_conf->action;

    for (n = 0; action != NULL && n < 256; n++) {
        limit = action->limit_req;

        if (limit != NULL && limit->stage == NXT_HTTP_LIMIT_REQ_HEADERS) {
            return nxt_http_limit_req_check(task, r, limit);
        }

        if (action->rewrite != NULL) {
            break;
        }

        action = nxt_http_route_peek(task, r, action);
    }

    return NXT_OK;
}
