
/*
 * Copyright (C) FreeUnit Community
 */

#ifndef _NXT_HTTP_LIMIT_REQ_H_INCLUDED_
#define _NXT_HTTP_LIMIT_REQ_H_INCLUDED_


#include <nxt_limit_zone.h>


/*
 * "limit_req" of a route action: a request rate limit on the zone object
 * of nxt_limit_zone.c, keyed by the client address.  HTTP routing runs in
 * one process, the router, so the zone is in its heap under a spin lock;
 * every router thread takes the same lock and the same bucket.
 *
 * A zone is named by the actions that use it and is kept across a
 * reconfiguration by a count, as the access log is: with the same name and
 * the same size the buckets and the counters stay; a new size is a new
 * zone.
 */

/* "zone": "name" without a size. */
#define NXT_HTTP_LIMIT_REQ_KEYS_DEFAULT  4096

/*
 * The zones of one configuration: at most this many, with this many keys
 * in all.  A zone takes its memory when the configuration is applied: 52
 * bytes per key and a pool of 32 KiB, so the zones take about 84 MiB at
 * most.
 */
#define NXT_HTTP_LIMIT_REQ_ZONES_MAX     1024
#define NXT_HTTP_LIMIT_REQ_KEYS_TOTAL    NXT_LIMIT_KEYS_MAX


typedef enum {
    NXT_HTTP_LIMIT_REQ_HEADERS = 0,
    NXT_HTTP_LIMIT_REQ_BODY,
} nxt_http_limit_req_stage_t;


typedef struct {
    nxt_limit_zone_t            *zone;
    /* The zone, its arena and this object live here. */
    nxt_mp_t                    *mp;

    /* The link in the router's live zones and the count; under its lock. */
    nxt_queue_link_t            link;
    uint32_t                    count;
    /*
     * The applied configuration has the zone, so /status shows it; under
     * the router lock.  A zone of a configuration that is not applied yet,
     * or of an older one that a request still holds, is not shown.
     */
    uint8_t                     current;  /* 1 bit */

    nxt_str_t                   name;
} nxt_http_limit_zone_t;


/* A reference of a configuration to a zone, with the parameters it asks. */

typedef struct {
    nxt_http_limit_zone_t       *zone;
    uint32_t                    keys_max;
    uint32_t                    rate;
    uint32_t                    burst;
} nxt_http_limit_zone_conf_t;


struct nxt_http_limit_req_s {
    nxt_http_limit_zone_t       *zone;
    /* The zone in rtcf->limit_zones, and its bit in the request. */
    uint32_t                    index;
    uint8_t                     stage;  /* nxt_http_limit_req_stage_t */
};


nxt_int_t nxt_http_limit_req_zone_parse(const nxt_str_t *zone,
    nxt_str_t *name, uint32_t *keys_max);
size_t nxt_http_limit_req_key(const nxt_sockaddr_t *sa, u_char *key);

nxt_int_t nxt_http_limit_req_init(nxt_task_t *task,
    nxt_router_temp_conf_t *tmcf, nxt_http_action_t *action,
    nxt_conf_value_t *cv);
void nxt_http_limit_req_zones_apply(nxt_task_t *task,
    nxt_router_conf_t *rtcf);
void nxt_http_limit_req_zones_release(nxt_task_t *task,
    nxt_thread_spinlock_t *lock, nxt_router_conf_t *rtcf);

nxt_int_t nxt_http_limit_req_headers(nxt_task_t *task,
    nxt_http_request_t *r);
nxt_int_t nxt_http_limit_req_check(nxt_task_t *task, nxt_http_request_t *r,
    nxt_http_limit_req_t *limit);


#endif /* _NXT_HTTP_LIMIT_REQ_H_INCLUDED_ */
