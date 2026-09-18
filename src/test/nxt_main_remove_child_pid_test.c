/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for issue #310: under "isolation": {"namespaces": {"pid":
 * true}} a worker that died before PROCESS_CREATED left the main process
 * holding the record, the port and the descriptor it made at WHOAMI time
 * until the prototype itself exited.
 *
 * The prototype cannot report it with REMOVE_PID.  A pid is that message's
 * whole payload and the only pid the prototype has for such a worker is the
 * namespace-local one it got from fork(), which names an unrelated process in
 * every other namespace -- a prototype's local counter climbs straight into
 * the range the daemon's own pids occupy, so a broadcast would close a live
 * sibling's, the router's or the controller's ports.
 *
 * Main is the one process that can resolve it, because it is the one that
 * holds both names.  A worker's WHOAMI arrives with the kernel-translated
 * global pid in SCM_CREDENTIALS and with the worker's own namespace-local pid
 * in the message header, and nxt_main_process_name_child() keeps the pair.
 * NXT_PORT_MSG_REMOVE_CHILD_PID then carries the local name, and the handler
 * looks for it only among the children of the sender the kernel names.
 *
 * So the two halves are driven separately here.
 *
 * The naming half keeps the pair whenever the two pids are sourced
 * independently, which is what NXT_USE_CMSG_PID says, and keeps it even when
 * they are numerically equal: the two counters are independent, a global pid
 * that has wrapped can land on the small number a namespace-local one holds,
 * and refusing the pair on that equality would leave exactly the record this
 * path exists to retire.  What it must refuse is a name a live sibling
 * already holds, since the number is chosen by the sender -- otherwise a
 * compromised worker could have main retire a sibling's record on the next
 * report.
 *
 * The resolving half must refuse a sender that is not a prototype, a sender
 * main has no record of, a payload that cannot hold a pid, and a pid that is
 * not one of that sender's children; and on the one case it accepts it must
 * both retire the record locally and tell the router by the global pid, which
 * is the number the router would have been given by an ordinary REMOVE_PID.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include <nxt_main_process.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


/* The worker's pid inside the prototype's namespace, and a free neighbour. */
#define NXT_MAIN_RCP_NS_PID        7
#define NXT_MAIN_RCP_OTHER_NS_PID  8


static nxt_int_t nxt_main_rcp_report(nxt_thread_t *thr, nxt_task_t *task,
    nxt_mp_t *mp, nxt_pid_t sender, nxt_pid_t pid, nxt_bool_t with_payload);


/*
 * Drive the handler once.  The runtime ports carry no queue and leave
 * write_ready unset, so nxt_port_socket_write() takes the
 * nxt_port_msg_chk_insert() path and whatever the handler sends stays on
 * port->messages for the test to read -- the arrangement
 * src/test/nxt_proto_creating_wedge_test.c already relies on.
 */

static nxt_int_t
nxt_main_rcp_report(nxt_thread_t *thr, nxt_task_t *task, nxt_mp_t *mp,
    nxt_pid_t sender, nxt_pid_t pid, nxt_bool_t with_payload)
{
    nxt_buf_t            *buf;
    nxt_port_recv_msg_t  msg;

    buf = NULL;

    if (with_payload) {
        buf = nxt_buf_mem_alloc(mp, sizeof(nxt_pid_t), 0);
        if (nxt_slow_path(buf == NULL)) {
            nxt_log_alert(thr->log, "main remove child pid test: no payload "
                          "buffer");
            return NXT_ERROR;
        }

        buf->mem.free = nxt_cpymem(buf->mem.free, &pid, sizeof(nxt_pid_t));
    }

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.buf = buf;
    msg.fd[0] = -1;
    msg.fd[1] = -1;
    msg.port_msg.type = _NXT_PORT_MSG_REMOVE_CHILD_PID;
    msg.port_msg.pid = sender;

#if (NXT_USE_CMSG_PID)
    msg.cmsg_pid = sender;
#endif

    nxt_main_test_run_remove_child_pid_handler(task, &msg);

    if (buf != NULL) {
        nxt_mp_free(mp, buf);
    }

    return NXT_OK;
}


static nxt_int_t
nxt_main_rcp_silent(nxt_thread_t *thr, nxt_port_t *port, const char *label)
{
    nxt_port_send_msg_t  *sent;

    if (nxt_fast_path(nxt_queue_is_empty(&port->messages))) {
        return NXT_OK;
    }

    sent = nxt_queue_link_data(nxt_queue_first(&port->messages),
                               nxt_port_send_msg_t, link);

    nxt_log_alert(thr->log, "main remove child pid test: %s made the router a "
                  "message of type %d", label, (int) sent->port_msg.type);

    return NXT_ERROR;
}


nxt_int_t
nxt_main_remove_child_pid_test(nxt_thread_t *thr)
{
    nxt_mp_t             *mp;
    nxt_int_t            ret;
    nxt_pid_t            pid, proto_pid, child_pid, other_pid, twin_pid;
    nxt_task_t           *task;
    nxt_port_t           *router_port, *proto_port, *child_port, *other_port;
    nxt_port_t           *twin_port;
    nxt_runtime_t        *rt, *saved_rt;
    nxt_process_t        *proto, *child, *other, *twin;
    nxt_event_engine_t   engine, *saved_engine;
    nxt_port_send_msg_t  *sent;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "main remove child pid test started");

    ret = NXT_ERROR;
    router_port = NULL;
    proto_port = NULL;
    child_port = NULL;
    other_port = NULL;
    twin_port = NULL;

    task = thr->task;
    task->thread = thr;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    if (nxt_slow_path(rt == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    rt->mem_pool = mp;

    if (nxt_slow_path(nxt_thread_mutex_create(&rt->processes_mutex) != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    /*
     * nxt_port_remove_notify_others() allocates its payload from
     * task->thread->engine->mem_pool; only the members the reached code
     * touches are set, as in src/test/nxt_router_new_port_test.c.
     */
    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 1024);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");
    engine.mem_pool = mp;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;
    thr->runtime = rt;
    thr->engine = &engine;
    rt->main_engine = &engine;

    /*
     * This process stands in for main, so every fixture pid has to differ
     * from nxt_pid: nxt_port_remove_notify_others() skips the running
     * process, and a router sharing it would make the accepted case pass
     * without sending anything.
     */

    proto_pid = nxt_pid + 1;
    child_pid = nxt_pid + 2;
    other_pid = nxt_pid + 3;

    /*
     * The worker whose namespace-local name is its own global pid.  It must
     * differ from every other fixture pid, so that the walk cannot find it
     * by accident.
     */
    twin_pid = nxt_pid + 5;

    router_port = nxt_runtime_process_port_create(task, rt, nxt_pid + 4, 0,
                                                  NXT_PROCESS_ROUTER);
    if (nxt_slow_path(router_port == NULL)) {
        goto done;
    }

    router_port->pair[0] = -1;
    router_port->pair[1] = -1;
    router_port->socket.fd = -1;

    proto_port = nxt_runtime_process_port_create(task, rt, proto_pid, 0,
                                                 NXT_PROCESS_PROTOTYPE);
    if (nxt_slow_path(proto_port == NULL)) {
        goto done;
    }

    proto_port->pair[0] = -1;
    proto_port->pair[1] = -1;
    proto_port->socket.fd = -1;

    proto = proto_port->process;

    /*
     * Two workers of that prototype, as nxt_main_process_whoami_handler()
     * leaves them: a record keyed on the global pid, a port holding main's
     * end of the worker's port socket, and the record linked into the
     * prototype's children.
     */

    child_port = nxt_runtime_process_port_create(task, rt, child_pid, 0,
                                                 NXT_PROCESS_APP);
    if (nxt_slow_path(child_port == NULL)) {
        goto done;
    }

    child_port->pair[0] = -1;
    child_port->pair[1] = -1;
    child_port->socket.fd = -1;

    child = child_port->process;

    other_port = nxt_runtime_process_port_create(task, rt, other_pid, 0,
                                                 NXT_PROCESS_APP);
    if (nxt_slow_path(other_port == NULL)) {
        goto done;
    }

    other_port->pair[0] = -1;
    other_port->pair[1] = -1;
    other_port->socket.fd = -1;

    other = other_port->process;

    twin_port = nxt_runtime_process_port_create(task, rt, twin_pid, 0,
                                                NXT_PROCESS_APP);
    if (nxt_slow_path(twin_port == NULL)) {
        goto done;
    }

    twin_port->pair[0] = -1;
    twin_port->pair[1] = -1;
    twin_port->socket.fd = -1;

    twin = twin_port->process;

    nxt_queue_insert_tail(&proto->children, &child->link);
    nxt_queue_insert_tail(&proto->children, &other->link);
    nxt_queue_insert_tail(&proto->children, &twin->link);

    /*
     * A header pid off the wire that no namespace ever hands out.  Zero is
     * also how "no name" is stored, so the negative is the one that tells a
     * missing guard from a working one.
     */

    nxt_main_test_run_name_child(task, proto, child, 0, child_pid);
    nxt_main_test_run_name_child(task, proto, child, -1, child_pid);

    if (nxt_slow_path(child->parent_ns_pid != 0)) {
        nxt_log_alert(thr->log, "main remove child pid test: a worker was "
                      "given the name %PI, which no namespace hands out",
                      child->parent_ns_pid);
        goto done;
    }

    nxt_main_test_run_name_child(task, proto, child, NXT_MAIN_RCP_NS_PID,
                                 child_pid);

    if (nxt_slow_path(child->parent_ns_pid != NXT_MAIN_RCP_NS_PID)) {
        nxt_log_alert(thr->log, "main remove child pid test: a pid-isolated "
                      "worker was not given the name its prototype knows it "
                      "by");
        goto done;
    }

    /*
     * The header pid is chosen by the sender.  A second worker claiming the
     * first one's name must keep none of its own, or the next report would
     * retire whichever record the walk reached first.
     */

    nxt_main_test_run_name_child(task, proto, other, NXT_MAIN_RCP_NS_PID,
                                 other_pid);

    if (nxt_slow_path(other->parent_ns_pid != 0)) {
        nxt_log_alert(thr->log, "main remove child pid test: a worker was "
                      "allowed to claim a live sibling's name");
        goto done;
    }

    nxt_main_test_run_name_child(task, proto, other,
                                 NXT_MAIN_RCP_OTHER_NS_PID, other_pid);

    if (nxt_slow_path(other->parent_ns_pid != NXT_MAIN_RCP_OTHER_NS_PID)) {
        nxt_log_alert(thr->log, "main remove child pid test: a worker was "
                      "refused a name no sibling holds");
        goto done;
    }

    /* A payload that cannot hold a pid names nothing. */

    ret = nxt_main_rcp_report(thr, task, mp, proto_pid, NXT_MAIN_RCP_NS_PID,
                              0);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /* A sender main has no record of. */

    ret = nxt_main_rcp_report(thr, task, mp, nxt_pid + 9,
                              NXT_MAIN_RCP_NS_PID, 1);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    /*
     * A sender that is not a prototype, holding a child of its own so that
     * the walk would find something if the type were not checked.  Only a
     * prototype forks into a namespace of its own, so only a prototype has
     * a local pid to report, and a process that answers for another one's
     * children could retire records it never made.
     */

    nxt_queue_remove(&other->link);
    nxt_queue_insert_tail(&router_port->process->children, &other->link);

    ret = nxt_main_rcp_report(thr, task, mp, router_port->pid,
                              NXT_MAIN_RCP_OTHER_NS_PID, 1);

    nxt_queue_remove(&other->link);
    nxt_queue_insert_tail(&proto->children, &other->link);

    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    if (nxt_slow_path(nxt_runtime_process_find(rt, other_pid) != other)) {
        nxt_log_alert(thr->log, "main remove child pid test: a sender that is "
                      "not a prototype retired a record");
        goto done;
    }

    /* A pid no child of that prototype answers to. */

    ret = nxt_main_rcp_report(thr, task, mp, proto_pid,
                              NXT_MAIN_RCP_NS_PID + 100, 1);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = NXT_ERROR;

    if (nxt_slow_path(nxt_runtime_process_find(rt, child_pid) != child)) {
        nxt_log_alert(thr->log, "main remove child pid test: a refused report "
                      "retired the record anyway");
        goto done;
    }

    if (nxt_slow_path(nxt_main_rcp_silent(thr, router_port,
                                          "a refused report") != NXT_OK))
    {
        goto done;
    }

    /*
     * A worker whose namespace-local pid happens to equal the global pid the
     * credential carried.  The two counters are independent, so the equality
     * says nothing about namespaces, and the pair has to be kept and stay
     * resolvable: treating it as "no second name" would leave the record and
     * the descriptor behind in exactly the case this path exists for.
     */

    nxt_main_test_run_name_child(task, proto, twin, twin_pid, twin_pid);

    if (nxt_slow_path(twin->parent_ns_pid != twin_pid)) {
        nxt_log_alert(thr->log, "main remove child pid test: a worker whose "
                      "two pids are numerically equal was left unnamed");
        goto done;
    }

    ret = nxt_main_rcp_report(thr, task, mp, proto_pid, twin_pid, 1);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = NXT_ERROR;
    twin_port = NULL;

    if (nxt_slow_path(nxt_runtime_process_find(rt, twin_pid) != NULL)) {
        nxt_log_alert(thr->log, "main remove child pid test: a worker whose "
                      "two pids are numerically equal could not be resolved");
        goto done;
    }

    /* The router is told by the global pid, and the queue starts empty
       again for the case below. */

    if (nxt_slow_path(nxt_queue_is_empty(&router_port->messages))) {
        nxt_log_alert(thr->log, "main remove child pid test: the router was "
                      "not told about the worker with equal pids");
        goto done;
    }

    nxt_port_test_run_error_handler(task, router_port);

    /*
     * The report the prototype actually sends.  The record goes, and the
     * router is told by the global pid main resolved -- the number an
     * ordinary REMOVE_PID would have carried.
     */

    ret = nxt_main_rcp_report(thr, task, mp, proto_pid, NXT_MAIN_RCP_NS_PID,
                              1);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto done;
    }

    ret = NXT_ERROR;
    child_port = NULL;

    if (nxt_slow_path(nxt_runtime_process_find(rt, child_pid) != NULL)) {
        nxt_log_alert(thr->log, "main remove child pid test: the record of a "
                      "reported worker outlived the report");
        goto done;
    }

    if (nxt_slow_path(nxt_runtime_process_find(rt, other_pid) != other)) {
        nxt_log_alert(thr->log, "main remove child pid test: the report took "
                      "a sibling's record with it");
        goto done;
    }

    if (nxt_slow_path(nxt_queue_is_empty(&router_port->messages))) {
        nxt_log_alert(thr->log, "main remove child pid test: the router was "
                      "not told that the worker is gone");
        goto done;
    }

    sent = nxt_queue_link_data(nxt_queue_first(&router_port->messages),
                               nxt_port_send_msg_t, link);

    if (nxt_slow_path(sent->port_msg.type != _NXT_PORT_MSG_REMOVE_PID)) {
        nxt_log_alert(thr->log, "main remove child pid test: the router got a "
                      "message of type %d, not REMOVE_PID",
                      (int) sent->port_msg.type);
        goto done;
    }

    if (nxt_slow_path(sent->buf == NULL
                      || nxt_buf_used_size(sent->buf) != sizeof(nxt_pid_t)))
    {
        nxt_log_alert(thr->log, "main remove child pid test: the router got a "
                      "REMOVE_PID carrying no pid");
        goto done;
    }

    nxt_memcpy(&pid, sent->buf->mem.pos, sizeof(nxt_pid_t));

    if (nxt_slow_path(pid != child_pid)) {
        nxt_log_alert(thr->log, "main remove child pid test: the router was "
                      "told to remove pid %PI, not the global pid %PI", pid,
                      child_pid);
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "main remove child pid test passed");

    ret = NXT_OK;

done:

    /*
     * Hand each port back the way its owner would: they were made by
     * nxt_runtime_process_port_create() and carry pools of their own, which
     * the fixture's pool does not cover.  nxt_port_close() is a no-op on the
     * -1 descriptors the fixture set; nxt_runtime_port_remove() drops the
     * rt->ports reference, which is the last one.
     */

    if (router_port != NULL) {
        nxt_port_test_run_error_handler(task, router_port);

        nxt_port_close(task, router_port);
        nxt_runtime_port_remove(task, router_port);
    }

    if (child_port != NULL) {
        nxt_port_close(task, child_port);
        nxt_runtime_port_remove(task, child_port);
    }

    if (twin_port != NULL) {
        nxt_queue_remove(&twin->link);
        twin->link.next = NULL;

        nxt_port_close(task, twin_port);
        nxt_runtime_port_remove(task, twin_port);
    }

    if (other_port != NULL) {
        nxt_queue_remove(&other->link);
        other->link.next = NULL;

        nxt_port_close(task, other_port);
        nxt_runtime_port_remove(task, other_port);
    }

    if (proto_port != NULL) {
        nxt_port_close(task, proto_port);
        nxt_runtime_port_remove(task, proto_port);
    }

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);

    nxt_thread_mutex_destroy(&rt->processes_mutex);

    nxt_mp_destroy(mp);

    return ret;
}
