
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


/* The counters of one "limit_req" zone; the name is an offset, see below. */

typedef struct {
    nxt_str_t         name;
    uint64_t          keys;
    uint64_t          keys_max;
    uint64_t          passed;
    uint64_t          rejected;
    uint64_t          evicted;
} nxt_status_limit_zone_t;


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

    /*
     * The access log is one per configuration, not one per application.  Its
     * path is stored as an offset from this report, as the app names are, so
     * the whole report stays one buffer.  access_log_configured says whether
     * a log is configured at all: without it the object is omitted, as
     * "telemetry" is, rather than reported as zero losses.
     */
    uint64_t          access_log_lost;
    nxt_str_t         access_log_path;
    uint8_t           access_log_configured;

    /* The zones follow the apps, see nxt_status_limit_zones(). */
    size_t            limit_zones_count;

    size_t            apps_count;
    nxt_status_app_t  apps[];
} nxt_status_report_t;


/*
 * The report is one buffer: the apps, then the zones, then the names at
 * the end.  nxt_status_app_t is a multiple of 8 bytes, so the zones are
 * aligned.
 */

nxt_inline nxt_status_limit_zone_t *
nxt_status_limit_zones(nxt_status_report_t *report)
{
    return (nxt_status_limit_zone_t *) &report->apps[report->apps_count];
}


nxt_conf_value_t *nxt_status_get(nxt_status_report_t *report, nxt_mp_t *mp);


#endif /* _NXT_STATUS_H_INCLUDED_ */
