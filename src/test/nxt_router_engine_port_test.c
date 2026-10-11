/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for the publication of the port of a router engine
 * (nxt_router_thread_create() in src/nxt_router.c).
 *
 * The engine sends requests to applications with its own port as the reply
 * port.  libunit asks the main router thread for that port with GET_PORT,
 * and nxt_router_get_port_handler() looks it up in rt->ports.  The engine
 * thread used to create the port and post its addition to rt->ports to the
 * main thread, and then start to accept at once.  A GET_PORT read in the
 * same poll as that post was handled first.  It found no port and got no
 * reply, so the request waited for ever.
 *
 * The property: when nxt_router_thread_create() returns NXT_OK, the port of
 * the new engine is in rt->ports.  The calling thread does not run its own
 * engine here, so work posted to it never runs.  The test waits up to 2
 * seconds for engine->port, because the old code set it on the engine
 * thread.  Then it looks the port up.
 *
 * The engine thread runs nxt_event_engine_start() and never returns.  It is
 * not stopped.  The test runs in a child process, and the thread ends when
 * the child exits.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


#define NXT_ROUTER_ENGINE_PORT_TEST_WAIT  2000


static nxt_event_engine_t *
nxt_router_engine_port_test_engine(nxt_task_t *task)
{
    nxt_mp_t                     *mp;
    nxt_array_t                  *services;
    nxt_event_engine_t           *engine;
    const nxt_event_interface_t  *interface;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NULL;
    }

    services = nxt_services_init(mp);
    if (nxt_slow_path(services == NULL)) {
        nxt_mp_destroy(mp);
        return NULL;
    }

    interface = nxt_service_get(services, "engine", NULL);
    if (nxt_slow_path(interface == NULL)) {
        nxt_mp_destroy(mp);
        return NULL;
    }

    /* The router creates its engines in the same way, with no signals. */

    engine = nxt_event_engine_create(task, interface, NULL, 0, 0);

    nxt_mp_destroy(mp);

    return engine;
}


static int
nxt_router_engine_port_test_child(void *data)
{
    nxt_mp_t            *mp;
    nxt_int_t           ret;
    nxt_uint_t          i;
    nxt_task_t          *task;
    nxt_port_t          *port, *found;
    nxt_thread_t        *thr;
    nxt_runtime_t       *rt;
    nxt_event_engine_t  *main_engine, *engine;

    thr = data;
    task = thr->task;

    mp = nxt_mp_create(1024, 128, 256, 32);
    NXT_TEST_CHECK(thr->log, mp != NULL,
                   "router engine port test: no memory pool");

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    NXT_TEST_CHECK(thr->log, rt != NULL,
                   "router engine port test: no runtime");

    rt->mem_pool = mp;
    nxt_queue_init(&rt->engines);

    /*
     * The engine of the calling thread.  The old code posted the addition
     * of the port to it.  The test never runs it, so a post stays queued.
     */
    main_engine = nxt_router_engine_port_test_engine(task);
    NXT_TEST_CHECK(thr->log, main_engine != NULL,
                   "router engine port test: no main engine");

    thr->runtime = rt;
    thr->engine = main_engine;

    engine = nxt_router_engine_port_test_engine(task);
    NXT_TEST_CHECK(thr->log, engine != NULL,
                   "router engine port test: no engine");

    ret = nxt_router_test_thread_create(task, rt, engine);
    NXT_TEST_CHECK(thr->log, ret == NXT_OK,
                   "router engine port test: thread create returned %i", ret);

    port = NULL;

    for (i = 0; i < NXT_ROUTER_ENGINE_PORT_TEST_WAIT; i++) {
        port = __atomic_load_n(&engine->port, __ATOMIC_ACQUIRE);
        if (port != NULL) {
            break;
        }

        nxt_nanosleep(1000000);
    }

    NXT_TEST_CHECK(thr->log, port != NULL,
                   "router engine port test: the engine has no port after "
                   "%ui ms", i);

    found = nxt_runtime_port_find(rt, nxt_pid, port->id);

    NXT_TEST_CHECK(thr->log, found == port,
                   "router engine port test: port %PI:%d of the engine is "
                   "%p in rt->ports, expected %p", nxt_pid, (int) port->id,
                   found, port);

    return 0;
}


nxt_int_t
nxt_router_engine_port_test(nxt_thread_t *thr)
{
    int  rc;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "router engine port test started");

    thr->task->thread = thr;

    rc = nxt_test_in_child(thr, "router engine port test",
                           nxt_router_engine_port_test_child, thr);

    NXT_TEST_CHECK(thr->log, rc == 0,
                   "router engine port test failed, child status %d", rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router engine port test passed");

    return NXT_OK;
}
