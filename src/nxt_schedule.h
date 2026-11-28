
/*
 * Copyright (C) FreeUnit Community
 */

#ifndef _NXT_SCHEDULE_H_INCLUDED_
#define _NXT_SCHEDULE_H_INCLUDED_


/*
 * "schedules": a command that runs at an interval in the context of an
 * application.  Built only with "./configure --scheduler".
 *
 * Three processes take part:
 *
 *  - the router (src/nxt_schedule.c) keeps the configuration and the clock.
 *    When a slot of a schedule is due, it asks main for a run;
 *
 *  - main (src/nxt_schedule_main.c) keeps one record for each schedule.  It
 *    decides whether the slot is free, forks the run, owns its deadline,
 *    kills its process group at the timeout and reaps it;
 *
 *  - the run (src/nxt_schedule_process.c) is a child of main.  It sets up
 *    the context of the application, as a prototype does, and then
 *    executes the command.
 *
 * The rules that the code and the tests keep:
 *
 *  R1. A slot is busy from the fork() until waitpid() in main returns the
 *      process of the run, and no longer.  A kill does not free the slot.
 *      A process that the run left behind does not hold it.
 *
 *  R2. A timeout means SIGKILL to the process group of the run.  There is
 *      no other signal first and no grace period.
 *
 *  R3. For one schedule in one unitd instance, main never starts a run
 *      while the process of the previous run is not reaped.  This says
 *      nothing about a process that left the group of the run (setsid(),
 *      setpgid()) or that outlives a run which ended by itself: main does
 *      not track those.  It also holds only where the kill of R2 reaches
 *      the group, which the tests check on the platform of the build.
 */


/* The limits of one schedule; the validator and main use the same. */

#define NXT_SCHEDULE_NAME_MAX      64
#define NXT_SCHEDULE_ARGS_MAX      64
#define NXT_SCHEDULE_ARGS_SIZE     8192

/*
 * A timer is an nxt_msec_t that is compared as a signed 32-bit difference,
 * so a wait is below 2^31 ms.  2147483 s is the largest number of whole
 * seconds below that.
 */
#define NXT_SCHEDULE_INTERVAL_MAX  2147483


/* The first byte of a NXT_PORT_MSG_SCHEDULE body, from the router to main. */

typedef enum {
    /*
     * The names of all schedules of the configuration that is now in
     * effect, each with its NUL.  Main forgets the others.
     */
    NXT_SCHEDULE_OP_CONF = 0,

    /*
     * A slot is due.  The rest is a JSON object:
     *
     *   { "name": "<schedule>", "job": <schedule object>,
     *     "conf": <application object> }
     */
    NXT_SCHEDULE_OP_RUN,
} nxt_schedule_op_t;


/*
 * The state of one schedule.  Main keeps it, and sends it to the router as
 * the body of NXT_PORT_MSG_SCHEDULE_STATUS when it changes, followed by the
 * name of the schedule without a NUL.  The router shows the last one it
 * got at /status/schedules.
 */

typedef struct {
    /* Runs that main started: a process was forked for the slot. */
    uint64_t   runs;
    /* Slots that came while the previous run was not reaped. */
    uint64_t   skipped;
    /*
     * Runs that could not be started, that exited with a status other
     * than zero, or that a signal other than the kill of the timeout
     * ended.
     */
    uint64_t   failed;
    /* Runs that main killed at the timeout. */
    uint64_t   timed_out;

    /* Of the last run that started, in seconds since the Epoch. */
    int64_t    last_start;
    /* Of the last run that ended, in milliseconds. */
    uint32_t   last_duration;
    /* Its exit status, or -1 when a signal ended it. */
    int32_t    last_exit;
    /* That signal, or 0. */
    int32_t    last_signal;

    /* The process of the run that is not reaped yet, or 0. */
    nxt_pid_t  pid;
    uint8_t    running;
} nxt_schedule_status_t;


/*
 * The start of the slot that follows "now": the smallest multiple of
 * "interval" seconds since the Epoch that is greater than "now".  Both
 * times are in milliseconds since the Epoch.  A slot that starts exactly
 * at "now" is not the answer: it is the slot that is due.
 *
 * "interval" is from 1 to NXT_SCHEDULE_INTERVAL_MAX, so the wait from
 * "now" to the result fits a timer.
 */

nxt_inline int64_t
nxt_schedule_slot_next(int64_t now, uint32_t interval)
{
    int64_t  step;

    step = (int64_t) interval * 1000;

    if (nxt_slow_path(now < 0)) {
        /* Before the Epoch every slot has passed; the first one is at 0. */
        return 0;
    }

    return (now / step + 1) * step;
}


#if (NXT_HAVE_SCHEDULER)

#include <nxt_conf.h>


/* The router. */

typedef struct nxt_schedule_conf_s  nxt_schedule_conf_t;

nxt_int_t nxt_schedule_conf_create(nxt_task_t *task, nxt_mp_t *mp,
    nxt_conf_value_t *root, nxt_schedule_conf_t **conf);
void nxt_schedule_conf_apply(nxt_task_t *task, nxt_schedule_conf_t *conf);
void nxt_schedule_status_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg);
size_t nxt_schedule_status_size(nxt_uint_t *count);
u_char *nxt_schedule_status_report(void *report, u_char *start, u_char *p);

/* Main. */

void nxt_schedule_main_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg);
nxt_bool_t nxt_schedule_main_ready(nxt_task_t *task, nxt_port_recv_msg_t *msg);
nxt_bool_t nxt_schedule_main_killed(nxt_pid_t pid, int status);
nxt_bool_t nxt_schedule_main_exited(nxt_task_t *task, nxt_pid_t pid,
    int status);
void nxt_schedule_main_stop(nxt_task_t *task);
nxt_bool_t nxt_schedule_main_running(void);

#if (NXT_TESTS)
nxt_bool_t nxt_schedule_main_test_start(nxt_task_t *task,
    const nxt_str_t *name, uint32_t timeout, nxt_pid_t pid);
const nxt_schedule_status_t *nxt_schedule_main_test_status(
    const nxt_str_t *name);
void nxt_schedule_main_test_conf(nxt_task_t *task, u_char *start,
    u_char *end);
void nxt_schedule_main_test_set_stopping(nxt_bool_t stopping);
#endif

/* The process of a run. */

NXT_EXPORT extern const nxt_process_init_t  nxt_schedule_process;

#endif /* NXT_HAVE_SCHEDULER */


#endif /* _NXT_SCHEDULE_H_INCLUDED_ */
