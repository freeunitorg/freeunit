/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Regression test for M-4: when nxt_conn_io_accept() (nxt_conn_accept.c)
 * cannot make an accepted socket non-blocking, it closes the socket and
 * returns.  The spare conn (lev->next) must then keep fd -1.  Before the
 * fix it kept the number of the closed socket, and nxt_conn_free() of the
 * spare, at a listener release, asserts that the fd is -1.
 *
 * The test accepts a real TCP connection on 127.0.0.1 and makes the
 * non-blocking step fail with the NXT_TESTS hook.  It checks the spare fd,
 * that the rejected socket was not passed to nxt_conn_accept(), and that
 * the accepted socket was closed: the client reads end of file.
 *
 * Before the fix, the code went on into nxt_conn_accept().  The fixture
 * engine, listen socket and work queue let that path run, so the test
 * reports the failure instead of a crash.
 */

#include <nxt_main.h>
#include <nxt_event_engine.h>
#include <nxt_conn.h>
#include "nxt_tests.h"


#if (NXT_LINUX)

static void
nxt_conn_io_accept_test_handler(nxt_task_t *task, void *obj, void *data)
{
}


/*
 * Before the fix, the code went on into nxt_conn_accept() and then into
 * nxt_conn_accept_close_idle(), which can disable the listener.  With this
 * handler the M-4 test does not depend on the M-5 fix.
 */

static void
nxt_conn_io_accept_test_disable_read(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev)
{
    ev->read = NXT_EVENT_INACTIVE;
}

#endif


nxt_int_t
nxt_conn_io_accept_test(nxt_thread_t *thr)
{
#if (NXT_LINUX)
    int                     lfd, cfd;
    char                    ch;
    void                    *obj, *data;
    size_t                  size;
    ssize_t                 n;
    nxt_int_t               ret;
    socklen_t               len;
    nxt_conn_t              c;
    nxt_task_t              *t;
    nxt_sockaddr_t          *remote;
    nxt_work_queue_t        wq;
    nxt_event_engine_t      engine, *saved_engine;
    nxt_work_handler_t      handler;
    nxt_listen_event_t      lev;
    struct sockaddr_in      sin;
    nxt_listen_socket_t     ls;
    nxt_work_queue_cache_t  cache;

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "conn io accept test started");

    ret = NXT_ERROR;
    cfd = -1;
    remote = NULL;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        nxt_log_alert(thr->log, "conn io accept test: socket() failed %E",
                      nxt_errno);
        return NXT_ERROR;
    }

    /*
     * The fixture for nxt_conn_accept(): an engine at max_connections, so
     * no new spare conn is allocated, a listen socket with a handler that
     * does nothing, and a work queue.
     */
    nxt_memzero(&engine, sizeof(nxt_event_engine_t));

    if (nxt_timers_init(&engine.timers, 4) != NXT_OK) {
        (void) close(lfd);
        return NXT_ERROR;
    }

    engine.task = *thr->task;
    engine.event.disable_read = nxt_conn_io_accept_test_disable_read;
    nxt_queue_init(&engine.idle_connections);
    nxt_queue_init(&engine.active_connections);

    nxt_work_queue_cache_create(&cache, 0);

    engine.close_work_queue.cache = &cache;

    nxt_memzero(&wq, sizeof(nxt_work_queue_t));
    wq.cache = &cache;

    nxt_memzero(&ls, sizeof(nxt_listen_socket_t));
    ls.handler = nxt_conn_io_accept_test_handler;

    saved_engine = thr->engine;
    thr->engine = &engine;

    /* The cleanup at "done" reads c and lev, also after an early failure. */

    nxt_memzero(&c, sizeof(nxt_conn_t));
    c.socket.fd = -1;

    nxt_memzero(&lev, sizeof(nxt_listen_event_t));
    lev.timer.task = &engine.task;
    lev.timer.log = thr->log;

    nxt_memzero(&sin, sizeof(struct sockaddr_in));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    len = sizeof(struct sockaddr_in);

    if (bind(lfd, (struct sockaddr *) &sin, len) != 0
        || listen(lfd, 1) != 0
        || getsockname(lfd, (struct sockaddr *) &sin, &len) != 0)
    {
        nxt_log_alert(thr->log, "conn io accept test: listen failed %E",
                      nxt_errno);
        goto done;
    }

    cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd == -1
        || connect(cfd, (struct sockaddr *) &sin, len) != 0)
    {
        nxt_log_alert(thr->log, "conn io accept test: connect failed %E",
                      nxt_errno);
        goto done;
    }

    /* Room for the address text that nxt_conn_accept() writes. */
    size = offsetof(nxt_sockaddr_t, u) + sizeof(struct sockaddr_in)
           + NXT_INET_ADDR_STR_LEN;

    remote = nxt_zalloc(size);
    if (remote == NULL) {
        goto done;
    }

    remote->socklen = sizeof(struct sockaddr_in);
    remote->length = NXT_INET_ADDR_STR_LEN;

    c.remote = remote;

    lev.socket.fd = lfd;
    lev.listen = &ls;
    lev.work_queue = &wq;
    lev.next = &c;
    lev.ready = 1;

    nxt_conn_test_nonblocking_failures = 1;

    nxt_conn_io_accept(thr->task, &lev, NULL);

    if (nxt_conn_test_nonblocking_failures != 0) {
        nxt_log_alert(thr->log, "conn io accept test failed: the "
                      "non-blocking step was not reached");
        goto done;
    }

    if (wq.head != NULL || c.listen != NULL) {
        nxt_log_alert(thr->log, "conn io accept test failed: the rejected "
                      "socket was passed to nxt_conn_accept()");
        goto done;
    }

    if (c.socket.fd != -1) {
        nxt_log_alert(thr->log, "conn io accept test failed: the spare conn "
                      "kept fd %d of a closed socket", c.socket.fd);
        goto done;
    }

    /* The accepted socket is closed: the client reads end of file. */

    n = read(cfd, &ch, 1);
    if (n != 0) {
        nxt_log_alert(thr->log, "conn io accept test failed: the accepted "
                      "socket was not closed (read %z)", n);
        goto done;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "conn io accept test passed");

    ret = NXT_OK;

done:

    nxt_conn_test_nonblocking_failures = 0;

    /* Undo what nxt_conn_accept() did, if it ran. */

    nxt_conn_untrack(&engine, (&c));

    nxt_timer_delete(&engine, &lev.timer);

    while (wq.head != NULL) {
        (void) nxt_work_queue_pop(&wq, &t, &obj, &data);
    }

    while (engine.close_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine.close_work_queue, &t, &obj,
                                     &data);
        handler(t, obj, data);
    }

    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&cache);
    nxt_free(engine.timers.changes);

    if (remote != NULL) {
        nxt_free(remote);
    }

    if (cfd != -1) {
        (void) close(cfd);
    }

    (void) close(lfd);

    return ret;

#else

    return NXT_OK;

#endif
}
