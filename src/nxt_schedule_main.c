
/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_port.h>
#include <nxt_main_process.h>
#include <nxt_conf.h>
#include <nxt_application.h>
#include <nxt_schedule.h>


/*
 * The main side of "schedules"; see src/nxt_schedule.h for the rules R1 to
 * R3 that this file keeps.
 *
 * Main has one nxt_schedule_job_t for each schedule that the router has
 * asked a run for.  The record lives as long as the schedule is in the
 * configuration, so it outlives the router: a router that starts again
 * gets the counters back, and a run that is in progress still holds its
 * slot.
 *
 * A run is a child of main, forked as a prototype is: with the isolation
 * of the application (nxt_isolation_main_prefork()), through
 * nxt_process_start().  Main has a record and a port for it until the
 * child says it is ready, which is the last thing it does before it
 * executes the command.  From then on nobody reads the port, so main
 * closes it and takes the record out of the runtime.  The job keeps the
 * record only for the cleanup of the isolation when the run is reaped.
 */


typedef struct nxt_schedule_job_s  nxt_schedule_job_t;

struct nxt_schedule_job_s {
    nxt_schedule_job_t     *next;

    /*
     * status.pid is the process of the run that main has not reaped, or 0.
     * It is the one thing that says the slot is busy (R1).
     */
    nxt_schedule_status_t  status;

    /* The record of that process, with a reference of the job. */
    nxt_process_t          *process;
    /* Its start, on the monotonic clock. */
    nxt_nsec_t             started;
    /* "timeout" of its schedule in seconds, 0 for none. */
    uint32_t               timeout;

    /* The deadline of the run. */
    nxt_timer_t            timer;

    /* PROCESS_READY came: the port is closed, the command runs. */
    uint8_t                ready;
    /* Main sent SIGKILL at the timeout. */
    uint8_t                timed_out;
    /* Main sent SIGKILL because it exits. */
    uint8_t                stopped;
    /* Gone from the configuration; freed when the run is reaped. */
    uint8_t                removed;
    uint8_t                stale;

    nxt_str_t              name;
    u_char                 name_buf[NXT_SCHEDULE_NAME_MAX];
};


/* The command of a run, as "job" of the RUN message has it. */

typedef struct {
    nxt_str_t              application;
    char                   *executable;
    nxt_conf_value_t       *arguments;
    uint32_t               timeout;
} nxt_schedule_spec_t;


static void nxt_schedule_main_conf(nxt_task_t *task, u_char *start,
    u_char *end);
static nxt_int_t nxt_schedule_main_run(nxt_task_t *task, nxt_mp_t **mpp,
    u_char *start, u_char *end);
static nxt_int_t nxt_schedule_main_spec(nxt_task_t *task, nxt_mp_t *mp,
    nxt_conf_value_t *value, nxt_schedule_spec_t *spec);
static nxt_schedule_job_t *nxt_schedule_main_slot(nxt_task_t *task,
    const nxt_str_t *name);
static nxt_int_t nxt_schedule_main_start(nxt_task_t *task,
    nxt_schedule_job_t *job, nxt_mp_t **mpp, nxt_schedule_spec_t *spec,
    nxt_conf_value_t *conf);
static void nxt_schedule_main_started(nxt_task_t *task,
    nxt_schedule_job_t *job, nxt_process_t *process, nxt_pid_t pid);
static void nxt_schedule_main_deadline(nxt_task_t *task, void *obj,
    void *data);
static void nxt_schedule_main_kill(nxt_task_t *task, nxt_schedule_job_t *job);
static void nxt_schedule_main_status_send(nxt_task_t *task,
    nxt_schedule_job_t *job);
static nxt_schedule_job_t *nxt_schedule_main_find(const nxt_str_t *name);
static nxt_schedule_job_t *nxt_schedule_main_find_pid(nxt_pid_t pid);
static nxt_schedule_job_t *nxt_schedule_main_create(nxt_task_t *task,
    const nxt_str_t *name);
static void nxt_schedule_main_remove(nxt_task_t *task,
    nxt_schedule_job_t *job);
static void nxt_schedule_main_release(nxt_task_t *task, void *obj,
    void *data);


static nxt_schedule_job_t  *nxt_schedule_jobs;

/* Main exits: no run starts any more.  See nxt_schedule_main_stop(). */
static nxt_bool_t          nxt_schedule_main_stopping;


/* NXT_PORT_MSG_SCHEDULE: the router sends the names, or asks for a run. */

void
nxt_schedule_main_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    u_char         *start, *end;
    nxt_mp_t       *mp;
    nxt_buf_t      *b;
    nxt_port_t     *port;
    nxt_runtime_t  *rt;

    rt = task->thread->runtime;

    mp = NULL;

    port = rt->port_by_type[NXT_PROCESS_ROUTER];

    /*
     * Every child has the write end of the main port.  Only the router
     * may start a process in the context of an application, as for
     * START_PROCESS, and the kernel says who sent this.
     */
    if (nxt_slow_path(port == NULL
                      || nxt_recv_msg_cmsg_pid(msg) != port->pid))
    {
        nxt_alert(task, "process %PI cannot run schedules",
                  nxt_recv_msg_cmsg_pid(msg));
        goto done;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        goto done;
    }

    b = nxt_buf_chk_make_plain(mp, msg->buf, msg->size);

    if (nxt_slow_path(b == NULL || nxt_buf_mem_used_size(&b->mem) < 1)) {
        nxt_alert(task, "schedules: empty message from the router");
        goto done;
    }

    start = b->mem.pos + 1;
    end = b->mem.free;

    switch (b->mem.pos[0]) {

    case NXT_SCHEDULE_OP_CONF:
        nxt_schedule_main_conf(task, start, end);
        break;

    case NXT_SCHEDULE_OP_RUN:
        if (nxt_schedule_main_run(task, &mp, start, end) == NXT_AGAIN) {
            /*
             * This is the child.  It goes back to the event engine, and
             * the start of its process type does the rest.
             */
            return;
        }

        break;

    default:
        nxt_alert(task, "schedules: unknown operation %d from the router",
                  (int) b->mem.pos[0]);
        break;
    }

done:

    if (mp != NULL) {
        nxt_mp_destroy(mp);
    }

    nxt_port_recv_msg_close_fds(msg);
}


/*
 * The names of the schedules that are in the configuration now.  A record
 * with another name is forgotten, at once or when its run is reaped.  The
 * router that sends this can be a new one, so each record that stays is
 * sent to it.
 */
static void
nxt_schedule_main_conf(nxt_task_t *task, u_char *start, u_char *end)
{
    u_char              *p;
    nxt_str_t           name;
    nxt_schedule_job_t  *job, *next;

    for (job = nxt_schedule_jobs; job != NULL; job = job->next) {
        job->stale = 1;
    }

    while (start < end) {
        p = memchr(start, '\0', end - start);

        if (nxt_slow_path(p == NULL)) {
            nxt_alert(task, "schedules: bad list of names from the router");
            break;
        }

        name.start = start;
        name.length = p - start;

        start = p + 1;

        job = nxt_schedule_main_find(&name);

        if (job != NULL) {
            job->stale = 0;
            job->removed = 0;

            nxt_schedule_main_status_send(task, job);
        }
    }

    for (job = nxt_schedule_jobs; job != NULL; job = next) {
        next = job->next;

        if (!job->stale) {
            continue;
        }

        if (job->status.pid != 0) {
            /* R1: the run still holds the slot of this name. */
            job->removed = 1;
            continue;
        }

        nxt_schedule_main_remove(task, job);
    }
}


/*
 * A slot of a schedule is due.  Returns NXT_AGAIN in the child, and then
 * the caller must do nothing more.
 *
 * "*mpp" has the message.  It becomes the pool of the process record when
 * a run starts, and then "*mpp" is NULL.
 */
static nxt_int_t
nxt_schedule_main_run(nxt_task_t *task, nxt_mp_t **mpp, u_char *start,
    u_char *end)
{
    nxt_int_t            ret;
    nxt_str_t            name;
    nxt_conf_value_t     *root, *value, *job_conf, *app_conf;
    nxt_schedule_job_t   *job;
    nxt_schedule_spec_t  spec;

    static const nxt_str_t  name_str = nxt_string("name");
    static const nxt_str_t  job_str = nxt_string("job");
    static const nxt_str_t  conf_str = nxt_string("conf");

    /*
     * The router built the message from a configuration that the
     * controller has validated.  It is still checked here: this is the
     * main process, and what it reads decides what it executes.
     */

    root = nxt_conf_json_parse(*mpp, start, end, NULL);

    if (nxt_slow_path(root == NULL
                      || nxt_conf_type(root) != NXT_CONF_OBJECT))
    {
        goto invalid;
    }

    value = nxt_conf_get_object_member(root, &name_str, NULL);
    job_conf = nxt_conf_get_object_member(root, &job_str, NULL);
    app_conf = nxt_conf_get_object_member(root, &conf_str, NULL);

    if (nxt_slow_path(value == NULL
                      || nxt_conf_type(value) != NXT_CONF_STRING
                      || job_conf == NULL
                      || nxt_conf_type(job_conf) != NXT_CONF_OBJECT
                      || app_conf == NULL
                      || nxt_conf_type(app_conf) != NXT_CONF_OBJECT))
    {
        goto invalid;
    }

    nxt_conf_get_string(value, &name);

    if (nxt_slow_path(name.length == 0
                      || name.length > NXT_SCHEDULE_NAME_MAX))
    {
        goto invalid;
    }

    ret = nxt_schedule_main_spec(task, *mpp, job_conf, &spec);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto invalid;
    }

    if (nxt_slow_path(nxt_schedule_main_stopping)) {
        nxt_debug(task, "schedule \"%V\": main exits, no run", &name);
        return NXT_OK;
    }

    job = nxt_schedule_main_slot(task, &name);
    if (job == NULL) {
        return NXT_OK;
    }

    job->timeout = spec.timeout;

    ret = nxt_schedule_main_start(task, job, mpp, &spec, app_conf);

    if (ret == NXT_AGAIN) {
        return NXT_AGAIN;
    }

    if (nxt_slow_path(ret != NXT_OK)) {
        job->status.failed++;

        nxt_log(task, NXT_LOG_ERR, "schedule \"%V\": the run could not be "
                "started", &job->name);

        nxt_schedule_main_status_send(task, job);
    }

    return NXT_OK;

invalid:

    nxt_alert(task, "schedules: bad run message from the router");

    return NXT_ERROR;
}


static nxt_int_t
nxt_schedule_main_spec(nxt_task_t *task, nxt_mp_t *mp, nxt_conf_value_t *value,
    nxt_schedule_spec_t *spec)
{
    size_t            size;
    double            num;
    uint32_t          i;
    nxt_str_t         str;
    nxt_conf_value_t  *member, *arg;

    static const nxt_str_t  application_str = nxt_string("application");
    static const nxt_str_t  executable_str = nxt_string("executable");
    static const nxt_str_t  arguments_str = nxt_string("arguments");
    static const nxt_str_t  timeout_str = nxt_string("timeout");

    nxt_memzero(spec, sizeof(nxt_schedule_spec_t));

    member = nxt_conf_get_object_member(value, &application_str, NULL);

    if (nxt_slow_path(member == NULL
                      || nxt_conf_type(member) != NXT_CONF_STRING))
    {
        return NXT_ERROR;
    }

    nxt_conf_get_string(member, &spec->application);

    member = nxt_conf_get_object_member(value, &executable_str, NULL);

    if (nxt_slow_path(member == NULL
                      || nxt_conf_type(member) != NXT_CONF_STRING))
    {
        return NXT_ERROR;
    }

    nxt_conf_get_string(member, &str);

    if (nxt_slow_path(str.length == 0
                      || memchr(str.start, '\0', str.length) != NULL))
    {
        return NXT_ERROR;
    }

    spec->executable = nxt_mp_nget(mp, str.length + 1);
    if (nxt_slow_path(spec->executable == NULL)) {
        return NXT_ERROR;
    }

    nxt_memcpy(spec->executable, str.start, str.length);
    spec->executable[str.length] = '\0';

    member = nxt_conf_get_object_member(value, &arguments_str, NULL);

    if (member != NULL) {
        if (nxt_slow_path(nxt_conf_type(member) != NXT_CONF_ARRAY
                          || nxt_conf_array_elements_count(member)
                             > NXT_SCHEDULE_ARGS_MAX))
        {
            return NXT_ERROR;
        }

        size = 0;

        for (i = 0; /* void */ ; i++) {
            arg = nxt_conf_get_array_element(member, i);
            if (arg == NULL) {
                break;
            }

            if (nxt_slow_path(nxt_conf_type(arg) != NXT_CONF_STRING)) {
                return NXT_ERROR;
            }

            nxt_conf_get_string(arg, &str);

            /* memchr() on a null pointer is undefined, also for 0 bytes. */

            if (nxt_slow_path(str.length != 0
                              && memchr(str.start, '\0', str.length) != NULL))
            {
                return NXT_ERROR;
            }

            size += str.length + 1;
        }

        if (nxt_slow_path(size > NXT_SCHEDULE_ARGS_SIZE)) {
            return NXT_ERROR;
        }

        spec->arguments = member;
    }

    member = nxt_conf_get_object_member(value, &timeout_str, NULL);

    if (member != NULL) {
        if (nxt_slow_path(nxt_conf_type(member) != NXT_CONF_INTEGER)) {
            return NXT_ERROR;
        }

        num = nxt_conf_get_number(member);

        if (nxt_slow_path(num < 1 || num > NXT_SCHEDULE_INTERVAL_MAX)) {
            return NXT_ERROR;
        }

        spec->timeout = (uint32_t) num;
    }

    return NXT_OK;
}


/*
 * The record of the schedule, if its slot is free.  This is the one place
 * that decides whether a run may start (R1, R3): the slot is busy exactly
 * while status.pid is set, and only nxt_schedule_main_exited() clears it,
 * with the pid that waitpid() returned.
 */
static nxt_schedule_job_t *
nxt_schedule_main_slot(nxt_task_t *task, const nxt_str_t *name)
{
    nxt_schedule_job_t  *job;

    job = nxt_schedule_main_find(name);

    if (job == NULL) {
        job = nxt_schedule_main_create(task, name);

        if (nxt_slow_path(job == NULL)) {
            nxt_alert(task, "schedule \"%V\": no memory for its record", name);
            return NULL;
        }
    }

    /* The router asks for a run only of a schedule that it has. */
    job->removed = 0;

    if (job->status.pid != 0) {
        job->status.skipped++;

        nxt_log(task, NXT_LOG_WARN, "schedule \"%V\": slot skipped, process "
                "%PI of the previous run has not exited", &job->name,
                job->status.pid);

        nxt_schedule_main_status_send(task, job);

        return NULL;
    }

    return job;
}


/*
 * Fork the run, as nxt_main_start_process_handler() forks a prototype.
 * Returns NXT_OK in main when the child exists, NXT_AGAIN in the child and
 * NXT_ERROR in main otherwise.
 */
static nxt_int_t
nxt_schedule_main_start(nxt_task_t *task, nxt_schedule_job_t *job,
    nxt_mp_t **mpp, nxt_schedule_spec_t *spec, nxt_conf_value_t *conf)
{
    u_char                 *p;
    nxt_mp_t               *mp;
    nxt_int_t              ret;
    nxt_runtime_t          *rt;
    nxt_process_t          *process;
    nxt_process_init_t     *init;
    nxt_common_app_conf_t  *app_conf;

    rt = task->thread->runtime;

    process = nxt_process_new(rt);
    if (nxt_slow_path(process == NULL)) {
        return NXT_ERROR;
    }

    /* The record owns the pool from here on, and all that was parsed. */
    mp = *mpp;
    *mpp = NULL;

    process->mem_pool = mp;
    process->parent_port = rt->port_by_type[NXT_PROCESS_MAIN];

    init = nxt_process_init(process);

    *init = nxt_schedule_process;

    app_conf = nxt_mp_zalloc(mp, sizeof(nxt_common_app_conf_t));
    if (nxt_slow_path(app_conf == NULL)) {
        goto failed;
    }

    /* A run has no shared port: it does not serve requests. */
    app_conf->shared_port_fd = -1;
    app_conf->shared_queue_fd = -1;

    app_conf->name = spec->application;

    p = nxt_mp_nget(mp, job->name.length + sizeof("\"\" schedule"));
    if (nxt_slow_path(p == NULL)) {
        goto failed;
    }

    process->name = (const char *) p;
    init->name = process->name;

    *p++ = '"';
    p = nxt_cpymem(p, job->name.start, job->name.length);
    p = nxt_cpymem(p, "\" schedule", 10);
    *p = '\0';

    ret = nxt_main_app_conf_map(task, mp, conf, app_conf);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto failed;
    }

    /*
     * The run takes the context of the application, not its code.  The
     * part of the configuration that belongs to the application type is
     * not used, and the command of the schedule goes where an external
     * application has its own: nxt_schedule_process_start() executes it
     * as nxt_external_start() does.
     */
    nxt_memzero(&app_conf->u, sizeof(app_conf->u));

    app_conf->u.external.executable = spec->executable;
    app_conf->u.external.arguments = spec->arguments;

    process->data.app = app_conf;

    ret = nxt_process_start(task, process);

    if (ret == NXT_AGAIN) {
        return NXT_AGAIN;
    }

    if (nxt_slow_path(ret != NXT_OK)) {
        goto failed;
    }

    /* The reference of the job; nxt_schedule_main_exited() drops it. */
    nxt_process_use(task, process, 1);

    nxt_schedule_main_started(task, job, process, process->pid);

    return NXT_OK;

failed:

    nxt_process_use(task, process, -1);

    return NXT_ERROR;
}


static void
nxt_schedule_main_started(nxt_task_t *task, nxt_schedule_job_t *job,
    nxt_process_t *process, nxt_pid_t pid)
{
    nxt_thread_t  *thr;

    thr = task->thread;

    nxt_thread_time_update(thr);

    job->process = process;
    job->started = nxt_thread_monotonic_time(thr);

    job->ready = 0;
    job->timed_out = 0;
    job->stopped = 0;

    job->status.pid = pid;
    job->status.running = 1;
    job->status.runs++;
    job->status.last_start = nxt_thread_realtime(thr)->sec;

    if (job->timeout != 0) {
        nxt_timer_add(thr->engine, &job->timer, job->timeout * 1000);
    }

    nxt_log(task, NXT_LOG_INFO, "schedule \"%V\": run started, process %PI",
            &job->name, pid);

    nxt_schedule_main_status_send(task, job);
}


/*
 * The timeout of a run (R2).  The slot is not freed here (R1): the process
 * exists until waitpid() returns it, and a signal that was sent is not a
 * process that is gone.
 */
static void
nxt_schedule_main_deadline(nxt_task_t *task, void *obj, void *data)
{
    nxt_timer_t         *timer;
    nxt_schedule_job_t  *job;

    timer = obj;
    job = nxt_timer_data(timer, nxt_schedule_job_t, timer);

    /*
     * A process that main forks inherits the timers of main with the rest
     * of the engine.  Only main may act on a deadline.
     */
    if (nxt_slow_path(task->thread->runtime->type != NXT_PROCESS_MAIN)) {
        return;
    }

    if (job->status.pid == 0) {
        return;
    }

    job->timed_out = 1;

    nxt_log(task, NXT_LOG_WARN, "schedule \"%V\": run timed out after %uD s, "
            "killing the group of process %PI", &job->name, job->timeout,
            job->status.pid);

    nxt_schedule_main_kill(task, job);
}


/*
 * SIGKILL to the process group of the run, and to its process.
 *
 * The run makes itself the leader of a new group first of all
 * (nxt_schedule_process_setup()), so the group has the pid of the run as
 * its id, and everything the command starts is in it unless it leaves.
 * Until that setpgid() the child is still in the group of main and no
 * group has this id; then only the second kill() finds it.
 *
 * Main has not reaped the process, so the pid is not free for reuse, and
 * a group id is not reused while a process with that pid exists.  Neither
 * signal can reach a stranger.
 */
static void
nxt_schedule_main_kill(nxt_task_t *task, nxt_schedule_job_t *job)
{
    nxt_pid_t  pid;

    pid = job->status.pid;

    /* kill(0) and kill(-1) mean every process that main can signal. */

    if (nxt_slow_path(pid <= 1)) {
        return;
    }

    if (kill(-pid, SIGKILL) != 0 && nxt_errno != NXT_ESRCH) {
        nxt_alert(task, "schedule \"%V\": kill(-%PI, SIGKILL) failed %E",
                  &job->name, pid, nxt_errno);
    }

    if (kill(pid, SIGKILL) != 0 && nxt_errno != NXT_ESRCH) {
        nxt_alert(task, "schedule \"%V\": kill(%PI, SIGKILL) failed %E",
                  &job->name, pid, nxt_errno);
    }
}


/*
 * PROCESS_READY on the main port.  Returns 1 when a run sent it, and then
 * the message is done.
 *
 * It is the last message of the child before it executes the command.  The
 * new program does not read the port, and a message that main sends later
 * to a port without a reader is an alert.  So main closes the port now and
 * takes the record out of the runtime: nothing in main walks over it any
 * more.  The record is not announced to any process, so there is nobody to
 * tell that it is gone.
 */
nxt_bool_t
nxt_schedule_main_ready(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_process_t       *process;
    nxt_schedule_job_t  *job;

    job = nxt_schedule_main_find_pid(nxt_recv_msg_cmsg_pid(msg));

    if (job == NULL || job->process == NULL) {
        return 0;
    }

    nxt_port_recv_msg_close_fds(msg);

    if (nxt_slow_path(job->ready)) {
        nxt_log(task, NXT_LOG_WARN, "schedule \"%V\": repeated PROCESS_READY "
                "from process %PI", &job->name, job->status.pid);
        return 1;
    }

    job->ready = 1;

    process = job->process;

    nxt_debug(task, "schedule \"%V\": process %PI executes the command",
              &job->name, job->status.pid);

    /* Not registered only if nxt_runtime_process_add() failed at the fork. */

    if (nxt_fast_path(process->registered)) {
        nxt_runtime_process_remove(task->thread->runtime, process);
    }

    nxt_process_close_ports(task, process);

    return 1;
}


/* Whether main itself killed this process; its end is then not an alert. */

nxt_bool_t
nxt_schedule_main_killed(nxt_pid_t pid, int status)
{
    nxt_schedule_job_t  *job;

    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        return 0;
    }

    job = nxt_schedule_main_find_pid(pid);

    return (job != NULL && (job->timed_out || job->stopped));
}


/*
 * waitpid() in main returned "pid".  Returns 1 when it was a run, and then
 * the slot of its schedule is free: this is the only place that frees it
 * (R1).
 */
nxt_bool_t
nxt_schedule_main_exited(nxt_task_t *task, nxt_pid_t pid, int status)
{
    nxt_nsec_t          now;
    nxt_thread_t        *thr;
    nxt_process_t       *process;
    nxt_schedule_job_t  *job;

    job = nxt_schedule_main_find_pid(pid);

    if (job == NULL) {
        return 0;
    }

    thr = task->thread;

    /* The record stays, so a change that is still queued is harmless. */
    (void) nxt_timer_delete(thr->engine, &job->timer);

    nxt_thread_time_update(thr);

    now = nxt_thread_monotonic_time(thr);

    job->status.pid = 0;
    job->status.running = 0;
    job->status.last_duration = (uint32_t) ((now - job->started) / 1000000);

    if (WIFSIGNALED(status)) {
        job->status.last_exit = -1;
        job->status.last_signal = WTERMSIG(status);

    } else {
        job->status.last_exit = WEXITSTATUS(status);
        job->status.last_signal = 0;
    }

    if (job->timed_out && WIFSIGNALED(status)) {
        job->status.timed_out++;

        nxt_log(task, NXT_LOG_WARN, "schedule \"%V\": run of process %PI "
                "ended by the timeout after %uD ms", &job->name, pid,
                job->status.last_duration);

    } else if (job->stopped && WIFSIGNALED(status)) {
        nxt_log(task, NXT_LOG_INFO, "schedule \"%V\": run of process %PI "
                "stopped, the server exits", &job->name, pid);

    } else if (WIFSIGNALED(status)) {
        job->status.failed++;

        nxt_log(task, NXT_LOG_ERR, "schedule \"%V\": run of process %PI "
                "ended on signal %d after %uD ms", &job->name, pid,
                WTERMSIG(status), job->status.last_duration);

    } else if (WEXITSTATUS(status) != 0) {
        job->status.failed++;

        nxt_log(task, NXT_LOG_ERR, "schedule \"%V\": run of process %PI "
                "exited with code %d after %uD ms", &job->name, pid,
                WEXITSTATUS(status), job->status.last_duration);

    } else {
        nxt_log(task, NXT_LOG_INFO, "schedule \"%V\": run of process %PI "
                "exited with code 0 after %uD ms", &job->name, pid,
                job->status.last_duration);
    }

    process = job->process;
    job->process = NULL;

    if (process != NULL) {
        /* As nxt_main_process_cleanup() does for a prototype. */

        if (process->isolation.cleanup != NULL) {
            process->isolation.cleanup(task, process);
        }

        if (process->isolation.cgroup_cleanup != NULL) {
            process->isolation.cgroup_cleanup(task, process);
        }

        if (!job->ready) {
            /* It ended before it was ready: the port is still there. */
            nxt_process_close_ports(task, process);
        }

        nxt_process_use(task, process, -1);
    }

    nxt_schedule_main_status_send(task, job);

    if (job->removed) {
        nxt_schedule_main_remove(task, job);
    }

    return 1;
}


/*
 * Main starts to exit.  A run has no port to get a QUIT on, so each one
 * is killed as at its timeout, and no new one starts.  Main then waits
 * for them: see nxt_schedule_main_running().
 */
void
nxt_schedule_main_stop(nxt_task_t *task)
{
    nxt_schedule_job_t  *job;

    nxt_schedule_main_stopping = 1;

    for (job = nxt_schedule_jobs; job != NULL; job = job->next) {

        if (job->status.pid == 0 || job->stopped) {
            continue;
        }

        job->stopped = 1;

        nxt_debug(task, "schedule \"%V\": main exits, killing the group of "
                  "process %PI", &job->name, job->status.pid);

        nxt_schedule_main_kill(task, job);
    }
}


/* Whether a run is not reaped yet.  Main does not exit before that. */

nxt_bool_t
nxt_schedule_main_running(void)
{
    nxt_schedule_job_t  *job;

    for (job = nxt_schedule_jobs; job != NULL; job = job->next) {
        if (job->status.pid != 0) {
            return 1;
        }
    }

    return 0;
}


static void
nxt_schedule_main_status_send(nxt_task_t *task, nxt_schedule_job_t *job)
{
    size_t         size;
    nxt_buf_t      *b;
    nxt_port_t     *port;
    nxt_runtime_t  *rt;

    rt = task->thread->runtime;

    port = rt->port_by_type[NXT_PROCESS_ROUTER];

    if (port == NULL) {
        /* The router is gone; the next one asks with its configuration. */
        return;
    }

    size = sizeof(nxt_schedule_status_t) + job->name.length;

    b = nxt_buf_mem_alloc(task->thread->engine->mem_pool, size, 0);
    if (nxt_slow_path(b == NULL)) {
        return;
    }

    b->mem.free = nxt_cpymem(b->mem.free, &job->status,
                             sizeof(nxt_schedule_status_t));
    b->mem.free = nxt_cpymem(b->mem.free, job->name.start, job->name.length);

    if (nxt_slow_path(nxt_port_socket_write(task, port,
                                            NXT_PORT_MSG_SCHEDULE_STATUS,
                                            -1, 0, 0, b)
                      != NXT_OK))
    {
        /* Still ours: the port layer takes the buffer only on NXT_OK. */

        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           b->completion_handler, task, b, b->parent);
    }
}


static nxt_schedule_job_t *
nxt_schedule_main_find(const nxt_str_t *name)
{
    nxt_schedule_job_t  *job;

    for (job = nxt_schedule_jobs; job != NULL; job = job->next) {
        if (nxt_strstr_eq(&job->name, name)) {
            return job;
        }
    }

    return NULL;
}


static nxt_schedule_job_t *
nxt_schedule_main_find_pid(nxt_pid_t pid)
{
    nxt_schedule_job_t  *job;

    if (pid <= 0) {
        return NULL;
    }

    for (job = nxt_schedule_jobs; job != NULL; job = job->next) {
        if (job->status.pid == pid) {
            return job;
        }
    }

    return NULL;
}


static nxt_schedule_job_t *
nxt_schedule_main_create(nxt_task_t *task, const nxt_str_t *name)
{
    nxt_schedule_job_t  *job;
    nxt_event_engine_t  *engine;

    job = nxt_zalloc(sizeof(nxt_schedule_job_t));
    if (nxt_slow_path(job == NULL)) {
        return NULL;
    }

    /* The caller has checked the length. */
    job->name.length = name->length;
    job->name.start = job->name_buf;
    nxt_memcpy(job->name_buf, name->start, name->length);

    job->status.last_exit = -1;

    engine = task->thread->engine;

    /* No bias: a run is not killed before its timeout. */
    job->timer.bias = 0;
    job->timer.work_queue = &engine->fast_work_queue;
    job->timer.handler = nxt_schedule_main_deadline;
    job->timer.task = &engine->task;
    job->timer.log = engine->task.log;

    job->next = nxt_schedule_jobs;
    nxt_schedule_jobs = job;

    return job;
}


/*
 * Unlink and free a record that has no run.  nxt_timer_delete() can leave
 * a queued change that points to the timer; then the memory is freed by a
 * handler that runs after that change, as in nxt_schedule_free().
 */
static void
nxt_schedule_main_remove(nxt_task_t *task, nxt_schedule_job_t *job)
{
    nxt_schedule_job_t  **prev;
    nxt_event_engine_t  *engine;

    for (prev = &nxt_schedule_jobs; *prev != NULL; prev = &(*prev)->next) {
        if (*prev == job) {
            *prev = job->next;
            break;
        }
    }

    nxt_debug(task, "schedule \"%V\": record removed", &job->name);

    engine = task->thread->engine;

    if (nxt_timer_delete(engine, &job->timer)) {
        job->timer.handler = nxt_schedule_main_release;
        nxt_timer_add(engine, &job->timer, 0);

        return;
    }

    nxt_free(job);
}


static void
nxt_schedule_main_release(nxt_task_t *task, void *obj, void *data)
{
    nxt_timer_t  *timer;

    timer = obj;

    nxt_free(nxt_timer_data(timer, nxt_schedule_job_t, timer));
}


#if (NXT_TESTS)

/*
 * For src/test/nxt_schedule_main_test.c.  The test forks the process of a
 * run itself, a plain child with no record, and gives its pid to the same
 * code that main runs after nxt_process_start().  The slot, the deadline
 * (from the timer of the record) and the reap are the code of main.
 */

nxt_bool_t
nxt_schedule_main_test_start(nxt_task_t *task, const nxt_str_t *name,
    uint32_t timeout, nxt_pid_t pid)
{
    nxt_schedule_job_t  *job;

    job = nxt_schedule_main_slot(task, name);
    if (job == NULL) {
        return 0;
    }

    job->timeout = timeout;

    nxt_schedule_main_started(task, job, NULL, pid);

    return 1;
}


const nxt_schedule_status_t *
nxt_schedule_main_test_status(const nxt_str_t *name)
{
    nxt_schedule_job_t  *job;

    job = nxt_schedule_main_find(name);

    return (job != NULL) ? &job->status : NULL;
}


void
nxt_schedule_main_test_conf(nxt_task_t *task, u_char *start, u_char *end)
{
    nxt_schedule_main_conf(task, start, end);
}


void
nxt_schedule_main_test_set_stopping(nxt_bool_t stopping)
{
    nxt_schedule_main_stopping = stopping;
}

#endif
