
/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_main_process.h>
#include <nxt_application.h>
#include <nxt_isolation.h>
#include <nxt_schedule.h>


/*
 * The process of one run of a schedule; see src/nxt_schedule.h.
 *
 * Main forks it with the process type of a prototype, so it goes the way
 * of a prototype up to its start: the namespaces, the cgroup and the
 * credential maps of the application's "isolation" (nxt_process_create(),
 * nxt_main_process_created_handler()), then the user and the group of the
 * application and the drop of the capabilities
 * (nxt_process_apply_creds()).  In between, the setup below sets the
 * environment, the root and the working directory of the application with
 * the functions that nxt_proto_setup() uses.
 *
 * Where a prototype waits for the router, this process executes the
 * command of the schedule.  It never forks a worker and it does not load
 * the language module of the application.
 */


static nxt_int_t nxt_schedule_process_setup(nxt_task_t *task,
    nxt_process_t *process);
static nxt_int_t nxt_schedule_process_start(nxt_task_t *task,
    nxt_process_data_t *data);
static void nxt_schedule_process_signals(void);
static void nxt_schedule_process_drop_handler(nxt_task_t *task,
    nxt_port_recv_msg_t *msg);


extern char  **environ;


/*
 * The messages that main can send between the fork and the PROCESS_READY
 * of this process.  NEW_PORT and REMOVE_PID are about the router, which
 * this process never talks to.
 */
static const nxt_port_handlers_t  nxt_schedule_process_port_handlers = {
    .quit         = nxt_signal_quit_handler,
    .change_file  = nxt_port_change_log_file_handler,
    .new_port     = nxt_schedule_process_drop_handler,
    .remove_pid   = nxt_schedule_process_drop_handler,
    .rpc_ready    = nxt_port_rpc_handler,
    .rpc_error    = nxt_port_rpc_handler,
};


const nxt_process_init_t  nxt_schedule_process = {
    .type           = NXT_PROCESS_PROTOTYPE,
    .prefork        = nxt_isolation_main_prefork,
    .restart        = 0,
    .setup          = nxt_schedule_process_setup,
    .start          = nxt_schedule_process_start,
    .port_handlers  = &nxt_schedule_process_port_handlers,
    .signals        = nxt_process_signals,
};


static nxt_int_t
nxt_schedule_process_setup(nxt_task_t *task, nxt_process_t *process)
{
    nxt_int_t              ret;
    nxt_common_app_conf_t  *app_conf;

    app_conf = process->data.app;

    /*
     * A process group of its own, before anything else: at the timeout
     * main kills the group with the pid of this process as its id (R2).
     * Whatever the command starts is in the group, unless it leaves.
     */
    if (nxt_slow_path(setpgid(0, 0) != 0)) {
        nxt_alert(task, "%s: setpgid(0, 0) failed %E", process->name,
                  nxt_errno);
        return NXT_ERROR;
    }

    ret = nxt_app_context_environment(task, app_conf);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    ret = nxt_app_context_root(task, process, app_conf);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    process->state = NXT_PROCESS_STATE_CREATED;

    return NXT_OK;
}


/*
 * Execute the command.  This runs after PROCESS_READY is sent, with the
 * credentials of the application.  It does not return when the command
 * starts.  When it cannot start, the process ends with the status that a
 * shell gives: 127 when there is no such file, 126 otherwise.
 *
 * The arguments are built as in nxt_external_start().
 */
static nxt_int_t
nxt_schedule_process_start(nxt_task_t *task, nxt_process_data_t *data)
{
    char                     **argv;
    u_char                   *p;
    size_t                   size;
    uint32_t                 index;
    nxt_err_t                err;
    nxt_str_t                str;
    nxt_uint_t               i, argc;
    nxt_conf_value_t         *value;
    nxt_common_app_conf_t    *conf;
    nxt_external_app_conf_t  *c;

    conf = data->app;
    c = &conf->u.external;

    argc = 2;
    size = 0;

    if (c->arguments != NULL) {

        for (index = 0; /* void */ ; index++) {
            value = nxt_conf_get_array_element(c->arguments, index);
            if (value == NULL) {
                break;
            }

            nxt_conf_get_string(value, &str);

            size += str.length + 1;
            argc++;
        }
    }

    argv = nxt_malloc(argc * sizeof(argv[0]) + size);
    if (nxt_slow_path(argv == NULL)) {
        nxt_alert(task, "failed to allocate arguments");
        return NXT_ERROR;
    }

    argv[0] = c->executable;
    i = 1;

    if (c->arguments != NULL) {
        p = (u_char *) &argv[argc];

        for (index = 0; /* void */ ; index++) {
            value = nxt_conf_get_array_element(c->arguments, index);
            if (value == NULL) {
                break;
            }

            argv[i++] = (char *) p;

            nxt_conf_get_string(value, &str);

            p = nxt_cpymem(p, str.start, str.length);
            *p++ = '\0';
        }
    }

    argv[i] = NULL;

    /* "stdout" and "stderr" of the application, as its workers have them. */
    (void) nxt_app_set_logs();

    nxt_schedule_process_signals();

    /*
     * The descriptors of unitd are close-on-exec.  This closes the ones
     * from 3 upwards all the same, as the state store child of main does:
     * the command is not a part of unitd and must not get one by an
     * oversight.  The log stays: it is the standard error.
     */
    nxt_main_store_close_fds();

    (void) execve(c->executable, argv, environ);

    err = nxt_errno;

    /* Not an alert: the path is from the configuration. */
    nxt_log(task, NXT_LOG_ERR, "schedule: execve(\"%s\") failed %E",
            c->executable, err);

    /* The descriptors of the engine are closed; do not go back to it. */
    _exit((err == NXT_ENOENT) ? 127 : 126);

    nxt_unreachable();

    return NXT_ERROR;
}


/*
 * unitd blocks the signals that it handles and ignores SIGPIPE and SIGSYS.
 * A blocked or an ignored signal stays so after execve().  The command
 * gets the default disposition of every signal and an empty mask, as from
 * a shell.
 */
static void
nxt_schedule_process_signals(void)
{
    int       signo;
    sigset_t  set;

#ifdef NSIG
#define NXT_SCHEDULE_NSIG  NSIG
#else
#define NXT_SCHEDULE_NSIG  65
#endif

    /* SIGKILL, SIGSTOP and the reserved numbers fail; that is fine. */

    for (signo = 1; signo < NXT_SCHEDULE_NSIG; signo++) {
        (void) signal(signo, SIG_DFL);
    }

    (void) sigemptyset(&set);
    (void) sigprocmask(SIG_SETMASK, &set, NULL);
}


static void
nxt_schedule_process_drop_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_port_recv_msg_close_fds(msg);
}
