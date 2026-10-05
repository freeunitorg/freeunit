/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for the websocket accounting in
 * nxt_router_app_port_release() (src/nxt_router.c).
 *
 * A websocket upgrade counts the session on the worker's main port, and
 * nxt_router_app_port_idle() keeps a port with a session out of the idle
 * queues.  Nothing uncounted it, so a worker that had served one upgrade
 * never rejoined the idle economy: the reaper never saw it and its slot
 * counted against "processes": {"max"} for the life of the process.  The
 * upgrade now leaves NXT_APR_WEBSOCKET_CLOSE behind for the unlink, and
 * that action uncounts the session.
 *
 * The fixture is one application with one worker port that is its own
 * ->main_app_port.  ->spare_processes is 1 and ->idle_processes starts at
 * 0, so the idle transition inserts into spare_ports and never arms the
 * reaper timer, which keeps the release confined to the queues and the
 * counters this test reads.  ->pair[1] is not -1, so the transition is
 * reached at all.
 *
 * The table walks the life of a session: the release that follows the
 * upgrade, a second session on the same worker, and the two closes.  A
 * close of a plain request is in it as well, to show that the decrement
 * belongs to the new action and not to every close.
 *
 * The last session closes on a worker the router has also marked
 * ->detached, because both are conditions of the one guard in
 * nxt_router_app_port_idle(): uncounting the session must not hand back a
 * port the detached state still holds, and clearing that state must.  The
 * step sets the flag directly, since app->detached_processes is settled by
 * the edges that own it and is not read here.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


typedef struct {
    const char        *name;
    nxt_bool_t        upgrade;
    uint8_t           detached;
    nxt_apr_action_t  action;
    uint32_t          active_websockets;
    nxt_bool_t        idle;
    nxt_atomic_t      use_count;
} nxt_router_websocket_test_step_t;


static const nxt_router_websocket_test_step_t
    nxt_router_websocket_test_steps[] =
{
    { "the first upgrade", 1, 0, NXT_APR_UPGRADE, 1, 0, 5 },
    { "a second session on the same worker", 1, 0, NXT_APR_UPGRADE, 2, 0, 5 },
    { "the first session closes", 0, 0, NXT_APR_WEBSOCKET_CLOSE, 1, 0, 4 },
    { "a plain request closes", 0, 0, NXT_APR_CLOSE, 1, 0, 3 },
    { "the last session closes, detached", 0, 1, NXT_APR_WEBSOCKET_CLOSE,
      0, 0, 2 },
    { "the detached mark clears", 0, 0, NXT_APR_CLOSE, 0, 1, 1 },
};


static nxt_int_t
nxt_router_websocket_test_step(nxt_thread_t *thr, nxt_task_t *task,
    nxt_port_t *port, nxt_app_t *app,
    const nxt_router_websocket_test_step_t *step)
{
    nxt_bool_t  idle;

    if (step->upgrade) {
        /*
         * What the two handlers ahead of the release do: the acknowledgement
         * counts the request on the port, see
         * nxt_router_req_headers_ack_handler(), and the response counts the
         * session, see nxt_router_response_ready_handler().  NXT_APR_UPGRADE
         * gives the request back and leaves the session behind.
         */
        nxt_thread_mutex_lock(&app->mutex);

        port->active_requests++;
        port->active_websockets++;

        nxt_thread_mutex_unlock(&app->mutex);
    }

    nxt_thread_mutex_lock(&app->mutex);

    port->detached = step->detached;

    nxt_thread_mutex_unlock(&app->mutex);

    nxt_router_test_app_port_release(task, app, port, step->action);

    idle = (port->idle_link.next != NULL);

    if (port->active_requests != 0
        || port->active_websockets != step->active_websockets
        || idle != step->idle
        || app->idle_processes != (step->idle ? 1U : 0U)
        || port->use_count != step->use_count)
    {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "router websocket test: %s: websockets %uD, idle %d, "
                      "idle processes %uD, use count %d; expected "
                      "%uD, %d, %uD, %d",
                      step->name, port->active_websockets, (int) idle,
                      app->idle_processes, (int) port->use_count,
                      step->active_websockets, (int) step->idle,
                      step->idle ? 1U : 0U, (int) step->use_count);
        return NXT_ERROR;
    }

    return NXT_OK;
}


nxt_int_t
nxt_router_websocket_test(nxt_thread_t *thr)
{
    nxt_mp_t            *mp;
    nxt_app_t           *app;
    nxt_uint_t          i;
    nxt_int_t           ret;
    nxt_task_t          *task;
    nxt_port_t          *port;
    nxt_bool_t          app_mutex;
    nxt_event_engine_t  engine, *saved_engine;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router websocket test started");

    ret = NXT_ERROR;
    port = NULL;
    app_mutex = 0;

    task = thr->task;
    task->thread = thr;

    saved_engine = thr->engine;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    app = nxt_mp_zalloc(mp, sizeof(nxt_app_t));
    if (nxt_slow_path(app == NULL)) {
        goto done;
    }

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));

    thr->engine = &engine;

    if (nxt_slow_path(nxt_thread_mutex_create(&app->mutex) != NXT_OK)) {
        goto done;
    }

    app_mutex = 1;

    nxt_queue_init(&app->ports);
    nxt_queue_init(&app->spare_ports);
    nxt_queue_init(&app->idle_ports);
    nxt_queue_init(&app->ack_waiting_req);

    nxt_str_set(&app->name, "websocket-test");

    /* Never reached by the releases below, so nothing frees the application. */
    app->use_count = 8;

    /* One spare, so the idle transition never arms the reaper timer. */
    app->spare_processes = 1;

    port = nxt_port_new(task, 0, nxt_pid, NXT_PROCESS_APP);
    if (nxt_slow_path(port == NULL)) {
        goto done;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;
    port->app = app;
    port->main_app_port = port;

    /*
     * The idle transition and the app->ports insertion both read ->pair[1],
     * and nxt_port_close() must not act on the value, so set it after
     * nxt_port_new() and clear it again before the port is released.
     */
    port->pair[1] = 0;

    /*
     * One reference for the fixture and one for each of the four releases
     * below that drops one, so the port outlives the table.  The first
     * release takes one back when it puts the port into app->ports, which
     * is why the table starts at 5.
     */
    port->use_count = 4;

    for (i = 0; i < nxt_nitems(nxt_router_websocket_test_steps); i++) {
        if (nxt_slow_path(nxt_router_websocket_test_step(thr, task, port, app,
                              &nxt_router_websocket_test_steps[i]) != NXT_OK))
        {
            goto done;
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router websocket test passed");

    ret = NXT_OK;

done:

    if (port != NULL) {
        port->pair[1] = -1;

        if (port->app_link.next != NULL) {
            nxt_queue_remove(&port->app_link);
            port->app_link.next = NULL;
        }

        if (port->idle_link.next != NULL) {
            nxt_queue_remove(&port->idle_link);
            port->idle_link.next = NULL;
        }

        port->app = NULL;
        port->main_app_port = NULL;

        port->use_count = 1;

        nxt_port_use(task, port, -1);
    }

    thr->engine = saved_engine;

    if (app_mutex) {
        nxt_thread_mutex_destroy(&app->mutex);
    }

    nxt_mp_destroy(mp);

    return ret;
}
