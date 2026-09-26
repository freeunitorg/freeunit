
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_STATUS_H_INCLUDED_
#define _NXT_STATUS_H_INCLUDED_


typedef struct {
    nxt_str_t         name;
    uint32_t          active_requests;
    uint32_t          pending_processes;
    uint32_t          processes;
    uint32_t          unaccounted_processes;
    uint32_t          idle_processes;
    uint32_t          detached_processes;
} nxt_status_app_t;


/*
 * One schedule's counters, kept by src/nxt_router_schedule.c.  "running" is
 * 0 or 1; "last_start" is in seconds since the Epoch, zero before the first
 * run; "last_duration" (ms) and "last_status" are of the last finished run.
 */
typedef struct {
    nxt_str_t         name;
    uint32_t          runs;
    uint32_t          skipped;
    uint32_t          failed;
    uint32_t          timed_out;
    uint32_t          running;
    uint32_t          last_status;
    uint32_t          last_duration;
    int64_t           last_start;
} nxt_status_schedule_t;


typedef struct {
    uint64_t          accepted_conns;
    uint64_t          idle_conns;
    uint64_t          closed_conns;
    uint64_t          requests;

    /*
     * OpenTelemetry span export health, filled in by the router.  The two
     * counters are only meaningful when otel_configured is set: a build
     * without OTel support, or one where "settings/telemetry" is absent,
     * leaves them zero and reports no "telemetry" object in /status at all.
     *
     * Counted in spans since the live tracer provider was installed, i.e.
     * reset by a telemetry reconfiguration.
     */
    uint64_t          otel_spans_exported;
    uint64_t          otel_spans_failed;
    uint8_t           otel_configured;

    size_t            apps_count;
    /* Entries after apps[], see nxt_status_report_schedules(). */
    size_t            schedules_count;
    nxt_status_app_t  apps[];
} nxt_status_report_t;


nxt_conf_value_t *nxt_status_get(nxt_status_report_t *report, nxt_mp_t *mp);


nxt_inline nxt_status_schedule_t *
nxt_status_report_schedules(nxt_status_report_t *report)
{
    return (nxt_status_schedule_t *) (report->apps + report->apps_count);
}


#endif /* _NXT_STATUS_H_INCLUDED_ */
