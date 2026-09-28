/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * When nxt_listen_event() cannot allocate the listen event, the router
 * create job acknowledges the configuration but leaves its joint in
 * engine->joints, with the listen socket reference that the job took.  A
 * later update or delete of that listener must release both.  Otherwise
 * the old configurations stay referenced, engine->joints never becomes
 * empty (so a worker thread that has to quit never exits), and a listen
 * socket that no engine could use is never closed.
 *
 * The stale joints are matched by the listen socket: when an engine is
 * removed, the delete job has the socket conf of the new configuration,
 * and the stale joint has the old one.
 *
 * The fixture is a set of joints in one engine.  The socket confs and the
 * listen sockets keep more than one reference, so a release only unlinks
 * the joint and drops one reference of each; the test reads the counts.
 *
 * nxt_router_stale_joint_jobs() then runs the real create and update jobs
 * on a fixture engine: a create whose listen event allocation fails must
 * keep its joint and take its listen socket reference, and the next update
 * of that listener must release both.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_router.h>
#include <nxt_event_engine.h>
#include <nxt_conn.h>
#include "nxt_tests.h"


#define NXT_STALE_JOINTS  5


static nxt_bool_t
nxt_router_stale_joint_linked(nxt_event_engine_t *engine,
    nxt_socket_conf_joint_t *joint)
{
    nxt_queue_link_t  *qlk;

    for (qlk = nxt_queue_first(&engine->joints);
         qlk != nxt_queue_tail(&engine->joints);
         qlk = nxt_queue_next(qlk))
    {
        if (qlk == &joint->link) {
            return 1;
        }
    }

    return 0;
}


static nxt_int_t
nxt_router_stale_joint_check(nxt_thread_t *thr, const char *step,
    nxt_event_engine_t *engine, nxt_socket_conf_joint_t *joint,
    const nxt_bool_t *linked)
{
    nxt_uint_t  i;

    for (i = 0; i < NXT_STALE_JOINTS; i++) {
        if (nxt_router_stale_joint_linked(engine, &joint[i]) != linked[i]) {
            nxt_log_alert(thr->log, "router stale joint test failed: %s: "
                          "joint %ui linked %d, expected %d", step, i,
                          !linked[i], linked[i]);
            return NXT_ERROR;
        }

        if (joint[i].socket_conf->count != (linked[i] ? 2 : 1)) {
            nxt_log_alert(thr->log, "router stale joint test failed: %s: "
                          "socket conf %ui count %D", step, i,
                          joint[i].socket_conf->count);
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


static nxt_int_t
nxt_router_stale_joint_last(nxt_thread_t *thr, nxt_router_conf_t *rtcf,
    nxt_event_engine_t *engine)
{
    int                      fd;
    nxt_sockaddr_t           sa;
    nxt_socket_conf_t        skcf;
    nxt_listen_socket_t      *ls;
    nxt_socket_conf_joint_t  joint;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        nxt_log_alert(thr->log, "router stale joint test: socket() failed %E",
                      nxt_errno);
        return NXT_ERROR;
    }

    ls = nxt_zalloc(sizeof(nxt_listen_socket_t));
    if (ls == NULL) {
        (void) close(fd);
        return NXT_ERROR;
    }

    nxt_memzero(&sa, sizeof(nxt_sockaddr_t));
    sa.u.sockaddr.sa_family = AF_INET;

    ls->socket = fd;
    ls->sockaddr = &sa;
    ls->count = 1;

    nxt_memzero(&skcf, sizeof(nxt_socket_conf_t));
    skcf.count = 2;
    skcf.router_conf = rtcf;
    skcf.listen = ls;

    nxt_memzero(&joint, sizeof(nxt_socket_conf_joint_t));
    joint.count = 1;
    joint.stale = 1;
    joint.socket_conf = &skcf;
    joint.engine = engine;

    nxt_queue_insert_tail(&engine->joints, &joint.link);

    /* ls is freed here; only the saved fd is used after the call. */
    nxt_router_test_listen_socket_release_stale(thr->task, engine, ls, NULL);

    if (nxt_router_stale_joint_linked(engine, &joint)) {
        nxt_log_alert(thr->log, "router stale joint test failed: last: "
                      "the joint is still linked");
        return NXT_ERROR;
    }

    if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
        nxt_log_alert(thr->log, "router stale joint test failed: last: "
                      "the listen socket %d was not closed", fd);
        (void) close(fd);
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_uint_t  nxt_router_stale_joint_signals;


static void
nxt_router_stale_joint_signal(nxt_event_engine_t *engine, nxt_uint_t signo)
{
    nxt_router_stale_joint_signals++;
}


static void
nxt_router_stale_joint_enable_accept(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev)
{
    ev->read = NXT_EVENT_ACTIVE;
}


static void
nxt_router_stale_joint_accept(nxt_task_t *task, void *obj, void *data)
{
}


static nxt_conn_io_t  nxt_router_stale_joint_io = {
    .accept = nxt_router_stale_joint_accept,
};


/*
 * Checks that the job was acknowledged: nxt_router_conf_wait_post() posts
 * the work of the job to the engine of the temporary configuration.
 */

static nxt_int_t
nxt_router_stale_joint_posted(nxt_thread_t *thr, const char *step,
    nxt_event_engine_t *conf_engine, nxt_joint_job_t *job)
{
    if (conf_engine->locked_work_queue.head != &job->work
        || nxt_router_stale_joint_signals != 1)
    {
        nxt_log_alert(thr->log, "router stale joint test failed: %s: the "
                      "job was not acknowledged", step);
        return NXT_ERROR;
    }

    conf_engine->locked_work_queue.head = NULL;
    conf_engine->locked_work_queue.tail = NULL;
    nxt_router_stale_joint_signals = 0;

    return NXT_OK;
}


static nxt_int_t
nxt_router_stale_joint_jobs(nxt_thread_t *thr, nxt_router_conf_t *rtcf)
{
    void                     *obj, *data;
    nxt_int_t                ret;
    nxt_uint_t               i;
    nxt_task_t               *task, *t;
    nxt_sockaddr_t           sa;
    nxt_joint_job_t          job[2];
    nxt_queue_link_t         *qlk;
    nxt_socket_conf_t        skcf[2];
    nxt_event_engine_t       engine, conf_engine, *saved_engine;
    nxt_listen_event_t       *lev;
    nxt_work_handler_t       handler;
    nxt_listen_socket_t      ls;
    nxt_work_queue_cache_t   cache;
    nxt_router_temp_conf_t   tmcf;
    nxt_socket_conf_joint_t  joint[2];

    ret = NXT_ERROR;
    lev = NULL;
    task = thr->task;

    nxt_memzero(&engine, sizeof(nxt_event_engine_t));
    nxt_memzero(&conf_engine, sizeof(nxt_event_engine_t));
    nxt_memzero(&tmcf, sizeof(nxt_router_temp_conf_t));
    nxt_memzero(job, sizeof(job));
    nxt_memzero(skcf, sizeof(skcf));
    nxt_memzero(joint, sizeof(joint));
    nxt_memzero(&ls, sizeof(nxt_listen_socket_t));
    nxt_memzero(&sa, sizeof(nxt_sockaddr_t));

    if (nxt_timers_init(&engine.timers, 4) != NXT_OK) {
        return NXT_ERROR;
    }

    engine.mem_pool = nxt_mp_create(1024, 128, 256, 32);
    if (engine.mem_pool == NULL) {
        nxt_free(engine.timers.changes);
        return NXT_ERROR;
    }

    engine.task = *task;
    engine.event.io = &nxt_router_stale_joint_io;
    engine.event.enable_accept = nxt_router_stale_joint_enable_accept;

    nxt_queue_init(&engine.joints);
    nxt_queue_init(&engine.listen_connections);
    nxt_queue_init(&engine.idle_connections);

    nxt_work_queue_cache_create(&cache, 0);

    engine.fast_work_queue.cache = &cache;
    engine.close_work_queue.cache = &cache;

    /* The engine that gets the acknowledgements of the jobs. */
    conf_engine.task = *task;
    conf_engine.event.signal = nxt_router_stale_joint_signal;
    tmcf.engine = &conf_engine;

    /*
     * The engine is at max_connections, so the listen event gets no spare
     * conn: it stays unarmed, and its listen timer retries the spare.  The
     * listen socket is not used for I/O.
     */
    sa.type = SOCK_STREAM;
    sa.u.sockaddr.sa_family = AF_INET;

    ls.socket = -1;
    ls.sockaddr = &sa;
    ls.socklen = sizeof(struct sockaddr_in);
    ls.count = 10;

    engine.max_connections = 0;

    for (i = 0; i < 2; i++) {
        job[i].task = *task;
        job[i].tmcf = &tmcf;

        skcf[i].count = 2;
        skcf[i].router_conf = rtcf;
        skcf[i].listen = &ls;

        joint[i].count = 1;
        joint[i].socket_conf = &skcf[i];
        joint[i].engine = &engine;
    }

    saved_engine = thr->engine;
    thr->engine = &engine;
    nxt_router_stale_joint_signals = 0;

    /*
     * A create whose listen event cannot be allocated: the joint stays
     * linked, the engine holds one listen socket reference, and the job is
     * acknowledged.
     */

    nxt_listen_event_test_alloc_failures = 1;

    nxt_router_test_listen_socket_create(task, &job[0], &joint[0]);

    if (!nxt_router_stale_joint_linked(&engine, &joint[0])
        || !nxt_queue_is_empty(&engine.listen_connections))
    {
        nxt_log_alert(thr->log, "router stale joint test failed: create: "
                      "the joint is not linked, or a listen event exists");
        goto done;
    }

    if (ls.count != 11 || !joint[0].stale) {
        nxt_log_alert(thr->log, "router stale joint test failed: create: "
                      "listen count %D (expected 11), stale flag %d",
                      ls.count, joint[0].stale);
        goto done;
    }

    if (nxt_router_stale_joint_posted(thr, "create", &conf_engine, &job[0])
        != NXT_OK)
    {
        goto done;
    }

    /*
     * The update of the same listener finds no listen event, so it creates
     * one, and releases the stale joint of the failed create with its
     * listen socket reference.
     */

    nxt_router_test_listen_socket_update(task, &job[1], &joint[1]);

    if (nxt_queue_is_empty(&engine.listen_connections)) {
        nxt_log_alert(thr->log, "router stale joint test failed: update: "
                      "no listen event was created");
        goto done;
    }

    qlk = nxt_queue_first(&engine.listen_connections);
    lev = nxt_queue_link_data(qlk, nxt_listen_event_t, link);

    if (nxt_router_stale_joint_linked(&engine, &joint[0])
        || !nxt_router_stale_joint_linked(&engine, &joint[1]))
    {
        nxt_log_alert(thr->log, "router stale joint test failed: update: "
                      "the stale joint is still linked, or the new one is "
                      "not");
        goto done;
    }

    if (ls.count != 11 || skcf[0].count != 1 || skcf[1].count != 2
        || joint[1].stale)
    {
        nxt_log_alert(thr->log, "router stale joint test failed: update: "
                      "listen count %D (expected 11), socket conf counts "
                      "%D %D (expected 1 2)", ls.count, skcf[0].count,
                      skcf[1].count);
        goto done;
    }

    if (nxt_router_stale_joint_posted(thr, "update", &conf_engine, &job[1])
        != NXT_OK)
    {
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_listen_event_test_alloc_failures = 0;

    if (lev != NULL) {
        nxt_timer_delete(&engine, &lev->timer);
        nxt_queue_remove(&lev->link);
        nxt_free(lev);
    }

    /* The idle-close pass that the missing spare conn queued. */
    while (engine.close_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine.close_work_queue, &t, &obj,
                                     &data);
        handler(t, obj, data);
    }

    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&cache);
    nxt_mp_destroy(engine.mem_pool);
    nxt_free(engine.timers.changes);

    return ret;
}


nxt_int_t
nxt_router_stale_joint_test(nxt_thread_t *thr)
{
    nxt_uint_t               i;
    nxt_router_t             router;
    nxt_router_conf_t        rtcf;
    nxt_event_engine_t       engine;
    nxt_listen_socket_t      ls[3];
    nxt_socket_conf_t        skcf[NXT_STALE_JOINTS];
    nxt_socket_conf_joint_t  joint[NXT_STALE_JOINTS];

    static const nxt_uint_t  listen_of[] = { 0, 1, 0, 1, 1 };
    static const nxt_bool_t  after_update[] = { 0, 1, 1, 1, 1 };
    static const nxt_bool_t  after_delete[] = { 0, 0, 1, 0, 0 };

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router stale joint test started");

    nxt_memzero(&router, sizeof(nxt_router_t));
    nxt_memzero(&rtcf, sizeof(nxt_router_conf_t));
    nxt_memzero(&engine, sizeof(nxt_event_engine_t));
    nxt_memzero(ls, sizeof(ls));
    nxt_memzero(skcf, sizeof(skcf));
    nxt_memzero(joint, sizeof(joint));

    rtcf.router = &router;
    rtcf.count = 100;

    nxt_queue_init(&engine.joints);

    for (i = 0; i < nxt_nitems(ls); i++) {
        ls[i].socket = -1;
        ls[i].count = 10;
    }

    /*
     * 0: the stale joint of a failed create of listener A.
     * 1: the stale joint of a failed create of listener B.
     * 2: the new joint of the update of listener A (the job's own joint).
     * 3, 4: more stale joints of listener B, each with its own socket conf,
     *    as after failed creates of B in older configurations.
     */

    for (i = 0; i < NXT_STALE_JOINTS; i++) {
        skcf[i].count = 2;
        skcf[i].router_conf = &rtcf;
        skcf[i].listen = &ls[listen_of[i]];

        joint[i].count = 1;
        joint[i].stale = (i != 2);
        joint[i].socket_conf = &skcf[i];
        joint[i].engine = &engine;

        nxt_queue_insert_tail(&engine.joints, &joint[i].link);
    }

    /*
     * An update of listener A keeps its own joint and releases only the
     * stale joint 0 and its reference on A.
     */

    nxt_router_test_listen_socket_release_stale(thr->task, &engine, &ls[0],
                                                &joint[2]);

    if (nxt_router_stale_joint_check(thr, "update", &engine, joint,
                                     after_update)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (ls[0].count != 9 || ls[1].count != 10) {
        nxt_log_alert(thr->log, "router stale joint test failed: update: "
                      "listen counts %D %D, expected 9 10",
                      ls[0].count, ls[1].count);
        return NXT_ERROR;
    }

    /*
     * A delete of listener B releases all its joints, whatever socket
     * conf they have, and one reference on B for each.
     */

    nxt_router_test_listen_socket_release_stale(thr->task, &engine, &ls[1],
                                                NULL);

    if (nxt_router_stale_joint_check(thr, "delete", &engine, joint,
                                     after_delete)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (ls[0].count != 9 || ls[1].count != 7) {
        nxt_log_alert(thr->log, "router stale joint test failed: delete: "
                      "listen counts %D %D, expected 9 7",
                      ls[0].count, ls[1].count);
        return NXT_ERROR;
    }

    /*
     * A joint without the stale flag stays, also when no joint is kept:
     * it can be the joint of a closed listener that a request still holds.
     */

    nxt_router_test_listen_socket_release_stale(thr->task, &engine, &ls[0],
                                                NULL);

    if (nxt_router_stale_joint_check(thr, "not stale", &engine, joint,
                                     after_delete)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    /* A listen socket without stale joints: nothing changes. */

    nxt_router_test_listen_socket_release_stale(thr->task, &engine, &ls[2],
                                                NULL);

    if (nxt_router_stale_joint_check(thr, "no match", &engine, joint,
                                     after_delete)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (ls[2].count != 10 || rtcf.count != 100) {
        nxt_log_alert(thr->log, "router stale joint test failed: no match: "
                      "listen count %D, router conf count %D",
                      ls[2].count, rtcf.count);
        return NXT_ERROR;
    }

    /*
     * The last reference closes the listen socket and frees it.  This is
     * the case of a listener whose listen event could not be allocated on
     * any engine: before, nothing closed it, and the address stayed bound.
     */

    if (nxt_router_stale_joint_last(thr, &rtcf, &engine) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_router_stale_joint_jobs(thr, &rtcf) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router stale joint test passed");

    return NXT_OK;
}
