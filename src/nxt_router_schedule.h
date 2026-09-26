
/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_ROUTER_SCHEDULE_H_INCLUDED_
#define _NXT_ROUTER_SCHEDULE_H_INCLUDED_


#include <nxt_router.h>
#include <nxt_conf.h>
#include <nxt_status.h>


/*
 * "schedules": periodic internal requests to an application, see
 * docs/adr/0004-schedules.md.  The limits are the validator's.
 *
 * A wait is an nxt_msec_t compared as a signed 32-bit difference, so
 * "interval" + "jitter" must stay below 2^31 ms.
 */
#define NXT_SCHEDULE_SECONDS_MAX   2147483

#define NXT_SCHEDULE_NAME_MAX      128
#define NXT_SCHEDULE_URI_MAX       4096
#define NXT_SCHEDULE_HEADERS_MAX   8192


/* One per schedule, from rtcf->mem_pool. */
typedef struct {
    nxt_str_t                name;
    nxt_http_action_t        *action;     /* resolved "pass" */
    nxt_str_t                uri;
    nxt_str_t                request;     /* the whole request header */

    nxt_msec_t               interval;
    nxt_msec_t               jitter;
    nxt_msec_t               timeout;
    uint8_t                  run_on_start;
} nxt_router_schedule_t;


/*
 * The schedules of one configuration, and the internal socket configuration
 * and joint that give their requests a real r->conf (section 6.3).  The skcf
 * holds the rtcf, the joint holds the skcf, and each run holds the joint.
 * The joint's own reference is dropped when the next configuration is
 * applied, on the joint's engine.
 */
struct nxt_router_schedules_s {
    nxt_router_schedule_t    *schedule;
    uint32_t                 nschedules;

    nxt_socket_conf_t        skcf;
    nxt_socket_conf_joint_t  joint;

    nxt_sockaddr_t           *remote;
    nxt_sockaddr_t           *local;

    nxt_work_t               insert_work;   /* onto engine->joints */
    nxt_work_t               release_work;
};


nxt_int_t nxt_router_conf_resolve(nxt_task_t *task,
    nxt_router_temp_conf_t *tmcf, nxt_conf_value_t *root);
void nxt_router_schedules_apply(nxt_task_t *task,
    nxt_router_temp_conf_t *tmcf);

nxt_int_t nxt_router_schedules_joint_init(nxt_task_t *task,
    nxt_router_conf_t *rtcf, nxt_router_schedules_t *sc,
    nxt_conf_value_t *http);
size_t nxt_router_schedule_uri_public(const nxt_str_t *uri);
nxt_int_t nxt_router_schedule_request_build(nxt_mp_t *mp,
    nxt_router_schedule_t *sched, nxt_conf_value_t *headers);
nxt_int_t nxt_router_schedule_parse(nxt_mp_t *mp, nxt_str_t *request,
    nxt_http_request_parse_t *rp);
nxt_int_t nxt_router_schedule_request_check(nxt_mp_t *mp, nxt_str_t *name,
    nxt_str_t *uri, nxt_conf_value_t *headers);

size_t nxt_router_schedules_status_size(nxt_uint_t *n);
void nxt_router_schedules_status(nxt_status_schedule_t *stat, u_char *p,
    u_char *base);


#endif /* _NXT_ROUTER_SCHEDULE_H_INCLUDED_ */
