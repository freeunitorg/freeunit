
/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_conf.h>
#include <nxt_status.h>
#include <nxt_schedule.h>


/*
 * The router side of "schedules"; see src/nxt_schedule.h.
 *
 * The router keeps one nxt_schedule_t for each schedule of the
 * configuration in effect, with a timer for its next slot.  Slots are
 * multiples of "interval" since the Epoch, so they do not depend on the
 * moment the configuration was applied or the router was started.  When a
 * slot is due the router sends the RUN message of the schedule to main and
 * waits for the next slot.  It does not look at the state of the previous
 * run: main decides whether the slot is free.
 *
 * Everything here runs on the main engine of the router: the apply of a
 * configuration, the timers, the port handlers and the status report.
 */


/* One schedule of a configuration that is being applied. */

typedef struct {
    nxt_str_t              name;
    uint32_t               interval;
    /* NXT_SCHEDULE_OP_RUN and the JSON object. */
    size_t                 size;
    u_char                 *msg;
} nxt_schedule_item_t;


struct nxt_schedule_conf_s {
    nxt_uint_t             nitems;
    nxt_schedule_item_t    *items;
};


typedef struct nxt_schedule_s  nxt_schedule_t;

struct nxt_schedule_s {
    nxt_schedule_t         *next;

    nxt_timer_t            timer;
    /* The slot that the timer waits for, in ms since the Epoch. */
    int64_t                slot;
    uint32_t               interval;

    nxt_bool_t             stale;

    /* The RUN message, nxt_malloc()ed. */
    size_t                 size;
    u_char                 *msg;

    /* The last state that main sent. */
    nxt_schedule_status_t  status;

    nxt_str_t              name;
    u_char                 name_buf[NXT_SCHEDULE_NAME_MAX];
};


static nxt_int_t nxt_schedule_update(nxt_task_t *task,
    nxt_schedule_item_t *item, int64_t now);
static nxt_schedule_t *nxt_schedule_find(const nxt_str_t *name);
static nxt_schedule_t *nxt_schedule_create(nxt_task_t *task,
    const nxt_str_t *name);
static void nxt_schedule_free(nxt_task_t *task, nxt_schedule_t *sched);
static void nxt_schedule_release_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_schedule_timer_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_schedule_send(nxt_task_t *task, const char *what,
    const u_char *msg, size_t size);
static void nxt_schedule_conf_send(nxt_task_t *task);
static int64_t nxt_schedule_now(void);


static nxt_schedule_t  *nxt_schedules;


/*
 * Parse "schedules" of the configuration "root" into "*conf".  Everything
 * is allocated from "mp", the pool of the temporary configuration, so a
 * configuration that fails later leaves nothing behind.  "*conf" is set
 * also when the configuration has no schedules: its apply then removes
 * the schedules of the previous one.
 *
 * The RUN message of a schedule carries the whole application object.
 * Main builds the context of the run from it, as it builds a prototype
 * from the same object in START_PROCESS.  So a run always gets the
 * application of the configuration that the schedule came with.
 */
nxt_int_t
nxt_schedule_conf_create(nxt_task_t *task, nxt_mp_t *mp, nxt_conf_value_t *root,
    nxt_schedule_conf_t **conf)
{
    size_t               length;
    u_char               *end;
    double               interval;
    uint32_t             next;
    nxt_str_t            name, app_name;
    nxt_uint_t           n;
    nxt_conf_value_t     *schedules, *applications, *value, *application;
    nxt_conf_value_t     *member, *obj;
    nxt_schedule_conf_t  *cf;
    nxt_schedule_item_t  *item;

    static const nxt_str_t  schedules_path = nxt_string("/schedules");
    static const nxt_str_t  applications_path = nxt_string("/applications");
    static const nxt_str_t  application_str = nxt_string("application");
    static const nxt_str_t  interval_str = nxt_string("interval");
    static const nxt_str_t  name_str = nxt_string("name");
    static const nxt_str_t  job_str = nxt_string("job");
    static const nxt_str_t  conf_str = nxt_string("conf");

    cf = nxt_mp_zget(mp, sizeof(nxt_schedule_conf_t));
    if (nxt_slow_path(cf == NULL)) {
        return NXT_ERROR;
    }

    *conf = cf;

    schedules = nxt_conf_get_path(root, &schedules_path);
    if (schedules == NULL) {
        return NXT_OK;
    }

    n = nxt_conf_object_members_count(schedules);
    if (n == 0) {
        return NXT_OK;
    }

    cf->items = nxt_mp_zget(mp, n * sizeof(nxt_schedule_item_t));
    if (nxt_slow_path(cf->items == NULL)) {
        return NXT_ERROR;
    }

    applications = nxt_conf_get_path(root, &applications_path);

    next = 0;

    for ( ;; ) {
        value = nxt_conf_next_object_member(schedules, &name, &next);
        if (value == NULL) {
            break;
        }

        /*
         * The validator has checked all of this.  A stored configuration
         * of another version is validated too, but these checks cost
         * nothing and the names end up in the main process.
         */

        member = nxt_conf_get_object_member(value, &application_str, NULL);

        if (nxt_slow_path(name.length == 0
                          || name.length > NXT_SCHEDULE_NAME_MAX
                          || applications == NULL
                          || member == NULL
                          || nxt_conf_type(member) != NXT_CONF_STRING))
        {
            goto invalid;
        }

        nxt_conf_get_string(member, &app_name);

        application = nxt_conf_get_object_member(applications, &app_name,
                                                 NULL);
        if (nxt_slow_path(application == NULL)) {
            goto invalid;
        }

        member = nxt_conf_get_object_member(value, &interval_str, NULL);

        if (nxt_slow_path(member == NULL
                          || nxt_conf_type(member) != NXT_CONF_INTEGER))
        {
            goto invalid;
        }

        interval = nxt_conf_get_number(member);

        if (nxt_slow_path(interval < 1
                          || interval > NXT_SCHEDULE_INTERVAL_MAX))
        {
            goto invalid;
        }

        item = &cf->items[cf->nitems];

        item->name = name;
        item->interval = (uint32_t) interval;

        obj = nxt_conf_create_object(mp, 3);
        if (nxt_slow_path(obj == NULL)) {
            return NXT_ERROR;
        }

        nxt_conf_set_member_string(obj, &name_str, &name, 0);
        nxt_conf_set_member(obj, &job_str, value, 1);
        nxt_conf_set_member(obj, &conf_str, application, 2);

        length = nxt_conf_json_length(obj, NULL);

        if (nxt_slow_path(length >= NXT_SIZE_T_MAX - 1)) {
            goto invalid;
        }

        item->msg = nxt_mp_nget(mp, 1 + length);
        if (nxt_slow_path(item->msg == NULL)) {
            return NXT_ERROR;
        }

        item->msg[0] = NXT_SCHEDULE_OP_RUN;

        end = nxt_conf_json_print(item->msg + 1, obj, NULL);

        item->size = end - item->msg;

        cf->nitems++;
    }

    return NXT_OK;

invalid:

    nxt_alert(task, "schedule \"%V\": invalid configuration", &name);

    return NXT_ERROR;
}


/*
 * Put the schedules of an applied configuration into effect.  The caller
 * runs this once the configuration can no longer fail.
 *
 * A schedule that keeps its name and its interval keeps its timer: its
 * next slot is the same.  A new interval gives a new grid of slots, and
 * the wait starts again from now.  A run in progress is not touched here
 * or in main: main lets it end, and only forgets a schedule that is gone
 * from the configuration when its run is reaped.
 */
void
nxt_schedule_conf_apply(nxt_task_t *task, nxt_schedule_conf_t *conf)
{
    int64_t              now;
    nxt_uint_t           i;
    nxt_schedule_t       *sched, **prev;
    nxt_schedule_item_t  *item;

    if (nxt_schedules == NULL && (conf == NULL || conf->nitems == 0)) {
        /*
         * No schedule before and none now, so main has nothing to learn.
         * The configuration does not change while the router is away, so
         * a router that starts again without schedules had none before.
         */
        return;
    }

    for (sched = nxt_schedules; sched != NULL; sched = sched->next) {
        sched->stale = 1;
    }

    now = nxt_schedule_now();

    for (i = 0; conf != NULL && i < conf->nitems; i++) {
        item = &conf->items[i];

        if (nxt_fast_path(nxt_schedule_update(task, item, now) == NXT_OK)) {
            continue;
        }

        /*
         * The configuration is already in effect and cannot fail here.
         * A schedule that exists keeps its previous message and timer.
         */
        sched = nxt_schedule_find(&item->name);

        nxt_alert(task, "schedule \"%V\": no memory, the schedule is %s",
                  &item->name, sched != NULL ? "not updated" : "not started");

        if (sched != NULL) {
            sched->stale = 0;
        }
    }

    prev = &nxt_schedules;

    for (sched = *prev; sched != NULL; sched = *prev) {

        if (!sched->stale) {
            prev = &sched->next;
            continue;
        }

        nxt_debug(task, "schedule \"%V\": removed", &sched->name);

        *prev = sched->next;

        nxt_schedule_free(task, sched);
    }

    nxt_schedule_conf_send(task);
}


static nxt_int_t
nxt_schedule_update(nxt_task_t *task, nxt_schedule_item_t *item, int64_t now)
{
    u_char          *msg;
    nxt_schedule_t  *sched;

    msg = nxt_malloc(item->size);
    if (nxt_slow_path(msg == NULL)) {
        return NXT_ERROR;
    }

    nxt_memcpy(msg, item->msg, item->size);

    sched = nxt_schedule_find(&item->name);

    if (sched == NULL) {
        sched = nxt_schedule_create(task, &item->name);
        if (nxt_slow_path(sched == NULL)) {
            nxt_free(msg);
            return NXT_ERROR;
        }

    } else {
        nxt_free(sched->msg);
    }

    sched->msg = msg;
    sched->size = item->size;
    sched->stale = 0;

    if (sched->interval != item->interval) {
        sched->interval = item->interval;
        sched->slot = nxt_schedule_slot_next(now, sched->interval);

        nxt_timer_add(task->thread->engine, &sched->timer,
                      (nxt_msec_t) (sched->slot - now));

        nxt_debug(task, "schedule \"%V\": every %uD s, next slot in %L ms",
                  &sched->name, sched->interval, sched->slot - now);
    }

    return NXT_OK;
}


static nxt_schedule_t *
nxt_schedule_find(const nxt_str_t *name)
{
    nxt_schedule_t  *sched;

    for (sched = nxt_schedules; sched != NULL; sched = sched->next) {
        if (nxt_strstr_eq(&sched->name, name)) {
            return sched;
        }
    }

    return NULL;
}


static nxt_schedule_t *
nxt_schedule_create(nxt_task_t *task, const nxt_str_t *name)
{
    nxt_schedule_t      *sched;
    nxt_event_engine_t  *engine;

    sched = nxt_zalloc(sizeof(nxt_schedule_t));
    if (nxt_slow_path(sched == NULL)) {
        return NULL;
    }

    /* nxt_schedule_conf_create() has checked the length. */
    sched->name.length = name->length;
    sched->name.start = sched->name_buf;
    nxt_memcpy(sched->name_buf, name->start, name->length);

    /* Not valid until the first state from main says otherwise. */
    sched->status.last_exit = -1;

    engine = task->thread->engine;

    /*
     * No bias: a slot must not start before its time, and a timer with a
     * bias can expire that much early.
     */
    sched->timer.bias = 0;
    sched->timer.work_queue = &engine->fast_work_queue;
    sched->timer.handler = nxt_schedule_timer_handler;
    sched->timer.task = &engine->task;
    sched->timer.log = engine->task.log;

    sched->next = nxt_schedules;
    nxt_schedules = sched;

    return sched;
}


/*
 * The schedule is unlinked.  nxt_timer_delete() can leave a queued change
 * that still points to the timer; then the memory is freed by a handler
 * that runs after that change, as nxt_router_start_timer_cancel() does.
 */
static void
nxt_schedule_free(nxt_task_t *task, nxt_schedule_t *sched)
{
    nxt_event_engine_t  *engine;

    engine = task->thread->engine;

    if (nxt_timer_delete(engine, &sched->timer)) {
        sched->timer.handler = nxt_schedule_release_handler;
        nxt_timer_add(engine, &sched->timer, 0);

        return;
    }

    nxt_free(sched->msg);
    nxt_free(sched);
}


static void
nxt_schedule_release_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_timer_t     *timer;
    nxt_schedule_t  *sched;

    timer = obj;
    sched = nxt_timer_data(timer, nxt_schedule_t, timer);

    nxt_free(sched->msg);
    nxt_free(sched);
}


static void
nxt_schedule_timer_handler(nxt_task_t *task, void *obj, void *data)
{
    int64_t             now, wait;
    nxt_timer_t         *timer;
    nxt_schedule_t      *sched;
    nxt_event_engine_t  *engine;

    timer = obj;
    sched = nxt_timer_data(timer, nxt_schedule_t, timer);

    engine = task->thread->engine;

    now = nxt_schedule_now();

    if (now < sched->slot) {
        /*
         * Too early.  The timer counts from the time that the engine
         * cached, which can be some milliseconds old, and the system
         * clock can be set back.  A slot never starts before its time.
         * After a step of more than one interval the old slot is far
         * away, so the wait is for the slot that now comes next.
         */
        wait = sched->slot - now;

        if (wait > (int64_t) sched->interval * 1000) {
            sched->slot = nxt_schedule_slot_next(now, sched->interval);
            wait = sched->slot - now;
        }

        nxt_timer_add(engine, timer, (nxt_msec_t) wait);

        return;
    }

    nxt_debug(task, "schedule \"%V\": slot %L is due", &sched->name,
              sched->slot / 1000);

    nxt_schedule_send(task, "a run", sched->msg, sched->size);

    /*
     * The next slot is the one after now, not the one after the slot
     * that was due.  When the router could not run for longer than the
     * interval, the slots in between are not made up.
     */
    sched->slot = nxt_schedule_slot_next(now, sched->interval);

    nxt_timer_add(engine, timer, (nxt_msec_t) (sched->slot - now));
}


/* Tell main the names of the schedules that are in the configuration. */

static void
nxt_schedule_conf_send(nxt_task_t *task)
{
    u_char          *msg, *p;
    size_t          size;
    nxt_schedule_t  *sched;

    size = 1;

    for (sched = nxt_schedules; sched != NULL; sched = sched->next) {
        size += sched->name.length + 1;
    }

    msg = nxt_malloc(size);
    if (nxt_slow_path(msg == NULL)) {
        nxt_alert(task, "schedules: no memory to send the names to the "
                  "main process");
        return;
    }

    p = msg;
    *p++ = NXT_SCHEDULE_OP_CONF;

    for (sched = nxt_schedules; sched != NULL; sched = sched->next) {
        p = nxt_cpymem(p, sched->name.start, sched->name.length);
        *p++ = '\0';
    }

    nxt_schedule_send(task, "the names", msg, size);

    nxt_free(msg);
}


static void
nxt_schedule_send(nxt_task_t *task, const char *what, const u_char *msg,
    size_t size)
{
    nxt_buf_t      *b;
    nxt_port_t     *port;
    nxt_runtime_t  *rt;

    rt = task->thread->runtime;

    port = rt->port_by_type[NXT_PROCESS_MAIN];

    b = nxt_buf_mem_alloc(task->thread->engine->mem_pool, size, 0);

    if (nxt_slow_path(port == NULL || b == NULL)) {
        nxt_alert(task, "schedules: cannot send %s to the main process",
                  what);
        goto fail;
    }

    b->mem.free = nxt_cpymem(b->mem.free, msg, size);

    if (nxt_fast_path(nxt_port_socket_write(task, port, NXT_PORT_MSG_SCHEDULE,
                                            -1, 0, 0, b)
                      == NXT_OK))
    {
        return;
    }

    nxt_alert(task, "schedules: failed to send %s to the main process", what);

fail:

    if (b != NULL) {
        /* Still ours: the port layer takes the buffer only on NXT_OK. */

        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           b->completion_handler, task, b, b->parent);
    }
}


/*
 * NXT_PORT_MSG_SCHEDULE_STATUS: main sends the state of a schedule when
 * it changes.  The gate of the router port has checked the sender.
 */
void
nxt_schedule_status_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    size_t          size;
    nxt_str_t       name;
    nxt_schedule_t  *sched;

    if (nxt_slow_path(msg->buf == NULL)) {
        return;
    }

    size = nxt_buf_mem_used_size(&msg->buf->mem);

    if (nxt_slow_path(size <= sizeof(nxt_schedule_status_t)
                      || size - sizeof(nxt_schedule_status_t)
                         > NXT_SCHEDULE_NAME_MAX))
    {
        nxt_alert(task, "schedules: bad state message of %uz bytes", size);
        return;
    }

    name.length = size - sizeof(nxt_schedule_status_t);
    name.start = msg->buf->mem.pos + sizeof(nxt_schedule_status_t);

    sched = nxt_schedule_find(&name);

    if (sched == NULL) {
        /* The state of a run that outlived its schedule. */
        nxt_debug(task, "schedule \"%V\": state of a removed schedule", &name);
        return;
    }

    nxt_memcpy(&sched->status, msg->buf->mem.pos,
               sizeof(nxt_schedule_status_t));

    nxt_debug(task, "schedule \"%V\": runs %uL, running %d, pid %PI",
              &sched->name, sched->status.runs, (int) sched->status.running,
              sched->status.pid);
}


/* The size that the schedules add to the status report, and their number. */

size_t
nxt_schedule_status_size(nxt_uint_t *count)
{
    size_t          size;
    nxt_uint_t      n;
    nxt_schedule_t  *sched;

    n = 0;
    size = 0;

    for (sched = nxt_schedules; sched != NULL; sched = sched->next) {
        size += sizeof(nxt_status_schedule_t) + sched->name.length;
        n++;
    }

    *count = n;

    return size;
}


/*
 * Fill the entries of the schedules in the status report.  "report" points
 * to the first entry, "start" is the start of the report, and the names go
 * below "p", as the names of the applications do.  Returns the new "p".
 */
u_char *
nxt_schedule_status_report(void *report, u_char *start, u_char *p)
{
    nxt_schedule_t         *sched;
    nxt_status_schedule_t  *stat;

    stat = report;

    for (sched = nxt_schedules; sched != NULL; sched = sched->next) {
        p -= sched->name.length;

        nxt_memcpy(p, sched->name.start, sched->name.length);

        stat->name.length = sched->name.length;
        stat->name.start = (u_char *) (p - start);
        stat->status = sched->status;

        stat++;
    }

    return p;
}


static int64_t
nxt_schedule_now(void)
{
    nxt_realtime_t  now;

    nxt_realtime(&now);

    return (int64_t) now.sec * 1000 + now.nsec / 1000000;
}
