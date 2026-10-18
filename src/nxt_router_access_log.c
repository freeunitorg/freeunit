
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Valentin V. Bartenev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_router.h>
#include <nxt_conf.h>
#include <nxt_http.h>
#include <nxt_checked.h>


typedef struct {
    nxt_str_t                       path;
    nxt_conf_value_t                *format;
    nxt_conf_value_t                *expr;
} nxt_router_access_log_conf_t;


typedef struct {
    nxt_str_t                       text;
    uint8_t                         too_large;  /* 1 bit */
} nxt_router_access_log_ctx_t;


typedef struct {
    nxt_str_t                       name;
    nxt_tstr_t                      *tstr;
} nxt_router_access_log_member_t;


struct nxt_router_access_log_format_s {
    nxt_tstr_t                      *tstr;
    nxt_uint_t                      nmembers;
    nxt_router_access_log_member_t  *member;
};


static nxt_router_access_log_format_t *nxt_router_access_log_format_create(
    nxt_task_t *task, nxt_router_conf_t *rtcf, nxt_conf_value_t *value);
static void nxt_router_access_log_warn(nxt_task_t *task,
    const nxt_str_t *field, nxt_str_t *str);
static void nxt_router_access_log_writer(nxt_task_t *task,
    nxt_http_request_t *r, nxt_router_access_log_t *access_log,
    nxt_router_access_log_format_t *format);
static void nxt_router_access_log_record(nxt_task_t *task,
    nxt_router_access_log_t *access_log, u_char *start, size_t length);
static ssize_t nxt_router_access_log_write(nxt_fd_t fd, u_char *start,
    size_t length);
static void nxt_router_access_log_lost(nxt_task_t *task,
    nxt_router_access_log_t *access_log, nxt_bool_t torn, const char *reason);
static nxt_int_t nxt_router_access_log_mark(
    nxt_router_access_log_t *access_log);
static void nxt_router_access_log_reopened(
    nxt_router_access_log_t *access_log);
static nxt_int_t nxt_router_access_log_text(nxt_task_t *task,
    nxt_http_request_t *r, nxt_router_access_log_ctx_t *ctx, nxt_tstr_t *tstr);
static nxt_int_t nxt_router_access_log_json(nxt_task_t *task,
    nxt_http_request_t *r, nxt_router_access_log_ctx_t *ctx,
    nxt_router_access_log_format_t *format);
static void nxt_router_access_log_ready(nxt_task_t *task,
    nxt_port_recv_msg_t *msg, void *data);
static void nxt_router_access_log_error(nxt_task_t *task,
    nxt_port_recv_msg_t *msg, void *data);
static void nxt_router_access_log_reopen_completion(nxt_task_t *task, void *obj,
    void *data);
static void nxt_router_access_log_reopen_ready(nxt_task_t *task,
    nxt_port_recv_msg_t *msg, void *data);
static void nxt_router_access_log_reopen_error(nxt_task_t *task,
    nxt_port_recv_msg_t *msg, void *data);


static nxt_conf_map_t  nxt_router_access_log_conf[] = {
    {
        nxt_string("path"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_router_access_log_conf_t, path),
    },

    {
        nxt_string("format"),
        NXT_CONF_MAP_PTR,
        offsetof(nxt_router_access_log_conf_t, format),
    },

    {
        nxt_string("if"),
        NXT_CONF_MAP_PTR,
        offsetof(nxt_router_access_log_conf_t, expr),
    },
};


nxt_int_t
nxt_router_access_log_create(nxt_task_t *task, nxt_router_conf_t *rtcf,
    nxt_conf_value_t *value)
{
    nxt_int_t                     ret;
    nxt_str_t                     str;
    nxt_router_t                  *router;
    nxt_router_access_log_t       *access_log;
    nxt_router_access_log_conf_t  alcf;

    nxt_memzero(&alcf, sizeof(nxt_router_access_log_conf_t));

    if (nxt_conf_type(value) == NXT_CONF_STRING) {
        nxt_conf_get_string(value, &alcf.path);

    } else {
        ret = nxt_conf_map_object(rtcf->mem_pool, value,
                                  nxt_router_access_log_conf,
                                  nxt_nitems(nxt_router_access_log_conf),
                                  &alcf);
        if (ret != NXT_OK) {
            nxt_alert(task, "access log map error");
            return NXT_ERROR;
        }
    }

    router = nxt_router;

    access_log = router->access_log;

    if (access_log != NULL && nxt_strstr_eq(&alcf.path, &access_log->path)) {
        nxt_router_access_log_use(&router->lock, access_log);

    } else {
        access_log = nxt_malloc(sizeof(nxt_router_access_log_t)
                                + alcf.path.length);
        if (access_log == NULL) {
            nxt_alert(task, "failed to allocate access log structure");
            return NXT_ERROR;
        }

        nxt_memzero(access_log, sizeof(nxt_router_access_log_t));

        access_log->fd = -1;
        access_log->handler = &nxt_router_access_log_writer;
        access_log->count = 1;

        access_log->path.length = alcf.path.length;
        access_log->path.start = (u_char *) access_log
                                 + sizeof(nxt_router_access_log_t);

        nxt_memcpy(access_log->path.start, alcf.path.start, alcf.path.length);
    }

    rtcf->access_log = access_log;

    rtcf->log_format = nxt_router_access_log_format_create(task, rtcf,
                                                           alcf.format);
    if (nxt_slow_path(rtcf->log_format == NULL)) {
        return NXT_ERROR;
    }

    if (alcf.expr != NULL) {
        nxt_conf_get_string(alcf.expr, &str);

        ret = nxt_tstr_cond_compile(rtcf->tstr_state, &str, &rtcf->log_cond);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


static nxt_router_access_log_format_t *
nxt_router_access_log_format_create(nxt_task_t *task, nxt_router_conf_t *rtcf,
    nxt_conf_value_t *value)
{
    uint32_t                        i, n, next;
    nxt_str_t                       name, str, *dst;
    nxt_conf_value_t                *cv;
    nxt_router_access_log_member_t  *member;
    nxt_router_access_log_format_t  *format;

    static const nxt_str_t  default_format = nxt_string("$remote_addr - - "
        "[$time_local] \"$request_line\" $status $body_bytes_sent "
        "\"$header_referer\" \"$header_user_agent\"");

    format = nxt_mp_zalloc(rtcf->mem_pool,
                           sizeof(nxt_router_access_log_format_t));
    if (nxt_slow_path(format == NULL)) {
        return NULL;
    }

    if (value != NULL) {

        if (nxt_conf_type(value) == NXT_CONF_OBJECT) {
            n = nxt_conf_object_members_count(value);

            member = nxt_mp_alloc(rtcf->mem_pool,
                                  n * sizeof(nxt_router_access_log_member_t));
            if (nxt_slow_path(member == NULL)) {
                return NULL;
            }

            next = 0;

            for (i = 0; i < n; i++) {
                cv = nxt_conf_next_object_member(value, &name, &next);
                if (cv == NULL) {
                    break;
                }

                dst = nxt_str_dup(rtcf->mem_pool, &member[i].name, &name);
                if (nxt_slow_path(dst == NULL)) {
                    return NULL;
                }

                nxt_conf_get_string(cv, &str);

                nxt_router_access_log_warn(task, &name, &str);

                member[i].tstr = nxt_tstr_compile(rtcf->tstr_state, &str,
                                                  NXT_TSTR_LOGGING);
                if (nxt_slow_path(member[i].tstr == NULL)) {
                    return NULL;
                }
            }

            format->nmembers = n;
            format->member = member;

            return format;

        } else {
            nxt_conf_get_string(value, &str);

            nxt_router_access_log_warn(task, NULL, &str);
        }

    } else {
        str = default_format;
    }

    /*
     * NXT_TSTR_ESCAPE is set here and not for the object format above: the
     * object format prints through nxt_conf_json_print(), which escapes what
     * it prints, and escaping twice would show the escape instead of the
     * value.
     */

    format->tstr = nxt_tstr_compile(rtcf->tstr_state, &str,
                                    NXT_TSTR_LOGGING | NXT_TSTR_NEWLINE
                                    | NXT_TSTR_ESCAPE);
    if (nxt_slow_path(format->tstr == NULL)) {
        return NULL;
    }

    return format;
}


/*
 * A format that names a credential header writes a bearer token or a session
 * cookie into the log file.  It is accepted, as in earlier versions, with a
 * warning.  The warning is here and not in the validator, so that a
 * configuration that the control API refuses gives none.  The text describes
 * the format, so it stays true also when the router then fails to apply the
 * configuration, for example when the new log file cannot be opened.  An
 * njs template reads a header through the request object, and the format
 * string cannot show that.
 */

static void
nxt_router_access_log_warn(nxt_task_t *task, const nxt_str_t *field,
    nxt_str_t *str)
{
    u_char     *p, *end, *next;
    nxt_str_t  part;

    if (nxt_tstr_is_js(str)) {
        return;
    }

    p = str->start;
    end = p + str->length;

    while (p < end) {
        next = nxt_var_next_part(p, end, &part);

        if (next == NULL) {
            /* The validator has accepted the syntax. */
            return;
        }

        if (part.start != NULL && nxt_http_var_credential(&part)) {

            if (field != NULL) {
                nxt_log(task, NXT_LOG_WARN, "the access log format uses the "
                        "\"%V\" variable in the \"%V\" field.  It writes "
                        "the value of a header that can carry a credential.",
                        &part, field);

            } else {
                nxt_log(task, NXT_LOG_WARN, "the access log format uses the "
                        "\"%V\" variable.  It writes the value of a header "
                        "that can carry a credential.", &part);
            }
        }

        p = next;
    }
}


static void
nxt_router_access_log_writer(nxt_task_t *task, nxt_http_request_t *r,
    nxt_router_access_log_t *access_log, nxt_router_access_log_format_t *format)
{
    nxt_int_t                    ret;
    nxt_router_access_log_ctx_t  *ctx;

    ctx = nxt_mp_get(r->mem_pool, sizeof(nxt_router_access_log_ctx_t));
    if (nxt_slow_path(ctx == NULL)) {
        nxt_router_access_log_lost(task, access_log, 0,
                                   "no memory for the record context");
        goto close;
    }

    ctx->too_large = 0;

    if (format->tstr != NULL) {
        ret = nxt_router_access_log_text(task, r, ctx, format->tstr);

    } else {
        ret = nxt_router_access_log_json(task, r, ctx, format);
    }

    if (ret != NXT_OK) {
        nxt_router_access_log_lost(task, access_log, 0,
                                   ctx->too_large
                                   ? "the record is over the size bound"
                                   : "the record could not be built");

    } else {
        nxt_router_access_log_record(task, access_log, ctx->text.start,
                                     ctx->text.length);
    }

close:

    /*
     * nxt_http_request_close_handler() calls this function and returns.  The
     * request is closed here, also when the record fails, for example when a
     * JS format throws.  r->logged is set, so this call does not log again.
     */

    nxt_http_request_close_handler(task, r, r->proto.any);
}


/*
 * One record is one write(), and a short write is not resumed.  All router
 * threads share the descriptor.  O_APPEND makes one write() atomic, not a
 * sequence of them, so a resumed write can land after a record of another
 * thread.  On a regular file a short write means that the file system
 * refused the rest (ENOSPC, EDQUOT, EFBIG, EIO), so a retry fails too.  A
 * blocking write of up to PIPE_BUF bytes to a pipe is never short.
 *
 * A short write is counted lost and leaves the log inside a line ("torn").
 * The next record first writes "\n" and the marker.  Until then, records are
 * dropped and counted.  A record that another thread started to write before
 * the tear can still join the torn line.
 */

static void
nxt_router_access_log_record(nxt_task_t *task,
    nxt_router_access_log_t *access_log, u_char *start, size_t length)
{
    ssize_t  n;

    if (nxt_slow_path(nxt_atomic_load(&access_log->gap) != 0)
        && nxt_router_access_log_mark(access_log) != NXT_OK)
    {
        nxt_router_access_log_lost(task, access_log, 0,
                                   "the log ends inside a lost record");
        return;
    }

    n = nxt_router_access_log_write(access_log->fd, start, length);

    if (nxt_slow_path(n < 0 || (size_t) n != length)) {
        nxt_router_access_log_lost(task, access_log, n > 0,
                                   "the write did not complete");
    }
}


#if (NXT_TESTS)

/* The next writes to fail without data, and the next writes to be short. */
nxt_uint_t  nxt_router_access_log_test_failed_writes;
nxt_uint_t  nxt_router_access_log_test_short_writes;


void
nxt_router_access_log_test_record(nxt_task_t *task,
    nxt_router_access_log_t *access_log, u_char *start, size_t length)
{
    nxt_router_access_log_record(task, access_log, start, length);
}


void
nxt_router_access_log_test_reopened(nxt_router_access_log_t *access_log)
{
    nxt_router_access_log_reopened(access_log);
}

#endif


static ssize_t
nxt_router_access_log_write(nxt_fd_t fd, u_char *start, size_t length)
{
    ssize_t  n;

#if (NXT_TESTS)
    if (nxt_router_access_log_test_failed_writes != 0) {
        nxt_router_access_log_test_failed_writes--;
        return -1;
    }

    if (nxt_router_access_log_test_short_writes != 0 && length > 1) {
        nxt_router_access_log_test_short_writes--;
        length /= 2;
    }
#endif

    do {
        n = write(fd, start, length);
    } while (n == -1 && nxt_errno == NXT_EINTR);

    return n;
}


/*
 * One warning per second, not one per lost record: a full disk must not turn
 * the access log loss into an error log flood.  The status counter carries
 * the exact number.  The counters are 64-bit also on 32-bit builds, so they
 * are kept under the spin lock.  It is held for a few instructions and never
 * across a write().
 */

static void
nxt_router_access_log_lost(nxt_task_t *task,
    nxt_router_access_log_t *access_log, nxt_bool_t torn, const char *reason)
{
    static nxt_log_moderation_t  nxt_access_log_lost_log_moderation = {
        NXT_LOG_WARN, 1, "access log records lost", NXT_LOG_MODERATION
    };

    nxt_thread_spin_lock(&access_log->lock);

    access_log->lost++;
    access_log->pending++;

    if (torn) {
        access_log->torn = 1;
    }

    nxt_atomic_store(&access_log->gap, 1);

    nxt_thread_spin_unlock(&access_log->lock);

    nxt_log_moderate(&nxt_access_log_lost_log_moderation, NXT_LOG_WARN,
                     task->log, "access log \"%V\": %s; the record is lost",
                     &access_log->path, reason);
}


uint64_t
nxt_router_access_log_lost_count(nxt_router_access_log_t *access_log)
{
    uint64_t  lost;

    nxt_thread_spin_lock(&access_log->lock);

    lost = access_log->lost;

    nxt_thread_spin_unlock(&access_log->lock);

    return lost;
}


/*
 * Writes "\n" after a torn record and the marker of the records lost since
 * the last marker, in one write().  One thread at a time does it.  Returns
 * NXT_OK when the log ends on a line boundary and a record can follow.
 */

static nxt_int_t
nxt_router_access_log_mark(nxt_router_access_log_t *access_log)
{
    u_char      *p, buf[64];
    size_t      length;
    ssize_t     n;
    uint64_t    pending;
    nxt_int_t   ret;
    nxt_bool_t  torn;

    nxt_thread_spin_lock(&access_log->lock);

    if (access_log->marking) {
        ret = (access_log->torn || access_log->marking_torn) ? NXT_ERROR
                                                              : NXT_OK;
        nxt_thread_spin_unlock(&access_log->lock);

        return ret;
    }

    pending = access_log->pending;
    torn = access_log->torn;

    access_log->pending = 0;
    access_log->torn = 0;
    access_log->marking = (pending != 0 || torn);
    access_log->marking_torn = torn;

    nxt_atomic_store(&access_log->gap, access_log->marking);

    nxt_thread_spin_unlock(&access_log->lock);

    if (pending == 0 && !torn) {
        return NXT_OK;
    }

    p = buf;

    if (torn) {
        *p++ = '\n';
    }

    if (pending != 0) {
        p = nxt_sprintf(p, buf + sizeof(buf),
                        "#freeunit: lost %uL record(s)\n", pending);
    }

    length = p - buf;

    n = nxt_router_access_log_write(access_log->fd, buf, length);

    nxt_thread_spin_lock(&access_log->lock);

    if (n < 0 || (size_t) n != length) {
        access_log->pending += pending;

        if (torn || n > 0) {
            access_log->torn = 1;
        }
    }

    access_log->marking = 0;
    access_log->marking_torn = 0;

    nxt_atomic_store(&access_log->gap,
                     (access_log->pending != 0 || access_log->torn));

    ret = access_log->torn ? NXT_ERROR : NXT_OK;

    nxt_thread_spin_unlock(&access_log->lock);

    return ret;
}


/* A reopened file is new, so it does not end inside a record. */

static void
nxt_router_access_log_reopened(nxt_router_access_log_t *access_log)
{
    nxt_thread_spin_lock(&access_log->lock);

    access_log->torn = 0;

    nxt_thread_spin_unlock(&access_log->lock);
}


static nxt_int_t
nxt_router_access_log_text(nxt_task_t *task, nxt_http_request_t *r,
    nxt_router_access_log_ctx_t *ctx, nxt_tstr_t *tstr)
{
    nxt_int_t          ret;
    nxt_router_conf_t  *rtcf;

    if (nxt_tstr_is_const(tstr)) {
        nxt_tstr_str(tstr, &ctx->text);

    } else {
        rtcf = r->conf->socket_conf->router_conf;

        ret = nxt_tstr_query_init(&r->tstr_query, rtcf->tstr_state,
                                  &r->tstr_cache, r, r->mem_pool);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }

        ret = nxt_tstr_query(task, r->tstr_query, tstr, &ctx->text);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


static nxt_int_t
nxt_router_access_log_json(nxt_task_t *task, nxt_http_request_t *r,
    nxt_router_access_log_ctx_t *ctx, nxt_router_access_log_format_t *format)
{
    u_char                          *p;
    size_t                          size;
    nxt_int_t                       ret;
    nxt_str_t                       str;
    nxt_uint_t                      i;
    nxt_conf_value_t                *value;
    nxt_router_conf_t               *rtcf;
    nxt_router_access_log_member_t  *member;

    rtcf = r->conf->socket_conf->router_conf;

    value = nxt_conf_create_object(r->mem_pool, format->nmembers);
    if (nxt_slow_path(value == NULL)) {
        return NXT_ERROR;
    }

    for (i = 0; i < format->nmembers; i++) {
        member = &format->member[i];

        if (nxt_tstr_is_const(member->tstr)) {
            nxt_tstr_str(member->tstr, &str);

        } else {
            ret = nxt_tstr_query_init(&r->tstr_query, rtcf->tstr_state,
                                      &r->tstr_cache, r, r->mem_pool);
            if (nxt_slow_path(ret != NXT_OK)) {
                return NXT_ERROR;
            }

            ret = nxt_tstr_query(task, r->tstr_query, member->tstr, &str);
            if (nxt_slow_path(ret != NXT_OK)) {
                return NXT_ERROR;
            }
        }

        /*
         * The value is whatever the variable resolved to, and for a request
         * header that is bytes the client chose: RFC 9110 Sect. 5.5 admits
         * obs-text, so 0x80-0xFF reach here unfiltered.  JSON text is UTF-8
         * (RFC 8259 Sect. 8.1), and nxt_conf_json_escape() escapes the quote
         * and the backslash but copies every other byte through -- so an
         * unencodable one leaves a record that is unforgeable and still
         * unreadable, which a strict consumer drops whole.
         *
         * Sanitize here rather than in the serializer.  The same serializer
         * writes state/conf.json and answers GET /config, where the bytes are
         * the operator's and have to round-trip exactly: a "share" path is
         * allowed to be non-UTF-8 on Linux, and rewriting one would point the
         * router at a different file after a restart.
         */

        ret = nxt_utf8_sanitize(r->mem_pool, &str, &str);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }

        nxt_conf_set_member_string(value, &member->name, &str, i);
    }

    /* The record and "\n". */
    if (nxt_slow_path(nxt_size_add(nxt_conf_json_length(value, NULL), 1, &size)
                      != 0))
    {
        return NXT_ERROR;
    }

    p = nxt_mp_nget(r->mem_pool, size);
    if (nxt_slow_path(p == NULL)) {
        return NXT_ERROR;
    }

    ctx->text.start = p;

    p = nxt_conf_json_print(p, value, NULL);
    *p++ = '\n';

    ctx->text.length = p - ctx->text.start;

    /*
     * The printed length, not the estimate: nxt_conf_json_length() counts
     * one "," too many.
     */
    if (ctx->text.length > NXT_ACCESS_LOG_MAX_RECORD) {
        ctx->too_large = 1;
        return NXT_ERROR;
    }

    return NXT_OK;
}


void
nxt_router_access_log_open(nxt_task_t *task, nxt_router_temp_conf_t *tmcf)
{
    uint32_t                 stream;
    nxt_int_t                ret;
    nxt_buf_t                *b;
    nxt_port_t               *main_port, *router_port;
    nxt_runtime_t            *rt;
    nxt_router_access_log_t  *access_log;

    access_log = tmcf->router_conf->access_log;

    b = nxt_buf_mem_alloc(tmcf->mem_pool, access_log->path.length + 1, 0);
    if (nxt_slow_path(b == NULL)) {
        goto fail;
    }

    b->completion_handler = nxt_buf_dummy_completion;

    nxt_buf_cpystr(b, &access_log->path);
    *b->mem.free++ = '\0';

    rt = task->thread->runtime;
    main_port = rt->port_by_type[NXT_PROCESS_MAIN];
    router_port = rt->port_by_type[NXT_PROCESS_ROUTER];

    stream = nxt_port_rpc_register_handler(task, router_port,
                                           nxt_router_access_log_ready,
                                           nxt_router_access_log_error,
                                           -1, tmcf);
    if (nxt_slow_path(stream == 0)) {
        goto fail;
    }

    ret = nxt_port_socket_write(task, main_port, NXT_PORT_MSG_ACCESS_LOG, -1,
                                stream, router_port->id, b);

    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_port_rpc_cancel(task, router_port, stream);
        goto fail;
    }

    return;

fail:

    nxt_router_conf_error(task, tmcf);
}


static void
nxt_router_access_log_ready(nxt_task_t *task, nxt_port_recv_msg_t *msg,
    void *data)
{
    nxt_router_temp_conf_t   *tmcf;
    nxt_router_access_log_t  *access_log;

    tmcf = data;

    access_log = tmcf->router_conf->access_log;

    access_log->fd = msg->fd[0];

    /* The access log owns the descriptor now. */
    msg->fd[0] = -1;

    nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                       nxt_router_conf_apply, task, tmcf, NULL);
}


static void
nxt_router_access_log_error(nxt_task_t *task, nxt_port_recv_msg_t *msg,
    void *data)
{
    nxt_router_temp_conf_t  *tmcf;

    tmcf = data;

    nxt_router_conf_error(task, tmcf);
}


void
nxt_router_access_log_use(nxt_thread_spinlock_t *lock,
    nxt_router_access_log_t *access_log)
{
    if (access_log == NULL) {
        return;
    }

    nxt_thread_spin_lock(lock);

    access_log->count++;

    nxt_thread_spin_unlock(lock);
}


void
nxt_router_access_log_release(nxt_task_t *task, nxt_thread_spinlock_t *lock,
    nxt_router_access_log_t *access_log)
{
    if (access_log == NULL) {
        return;
    }

    nxt_thread_spin_lock(lock);

    if (--access_log->count != 0) {
        access_log = NULL;
    }

    nxt_thread_spin_unlock(lock);

    if (access_log != NULL) {

        if (access_log->fd != -1) {
            nxt_fd_close(access_log->fd);
        }

        nxt_free(access_log);
    }
}


typedef struct {
    nxt_mp_t                 *mem_pool;
    nxt_router_access_log_t  *access_log;
} nxt_router_access_log_reopen_t;


void
nxt_router_access_log_reopen_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_mp_t                        *mp;
    uint32_t                        stream;
    nxt_int_t                       ret;
    nxt_buf_t                       *b;
    nxt_port_t                      *main_port, *router_port;
    nxt_runtime_t                   *rt;
    nxt_router_access_log_t         *access_log;
    nxt_router_access_log_reopen_t  *reopen;

    access_log = nxt_router->access_log;

    if (access_log == NULL) {
        return;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return;
    }

    reopen = nxt_mp_get(mp, sizeof(nxt_router_access_log_reopen_t));
    if (nxt_slow_path(reopen == NULL)) {
        goto fail;
    }

    reopen->mem_pool = mp;
    reopen->access_log = access_log;

    b = nxt_buf_mem_alloc(mp, access_log->path.length + 1, 0);
    if (nxt_slow_path(b == NULL)) {
        goto fail;
    }

    b->completion_handler = nxt_router_access_log_reopen_completion;

    nxt_buf_cpystr(b, &access_log->path);
    *b->mem.free++ = '\0';

    rt = task->thread->runtime;
    main_port = rt->port_by_type[NXT_PROCESS_MAIN];
    router_port = rt->port_by_type[NXT_PROCESS_ROUTER];

    stream = nxt_port_rpc_register_handler(task, router_port,
                                           nxt_router_access_log_reopen_ready,
                                           nxt_router_access_log_reopen_error,
                                           -1, reopen);
    if (nxt_slow_path(stream == 0)) {
        goto fail;
    }

    ret = nxt_port_socket_write(task, main_port, NXT_PORT_MSG_ACCESS_LOG, -1,
                                stream, router_port->id, b);

    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_port_rpc_cancel(task, router_port, stream);
        goto fail;
    }

    nxt_mp_retain(mp);

    return;

fail:

    nxt_mp_destroy(mp);
}


static void
nxt_router_access_log_reopen_completion(nxt_task_t *task, void *obj, void *data)
{
    nxt_mp_t   *mp;
    nxt_buf_t  *b;

    b = obj;
    mp = b->data;

    nxt_mp_release(mp);
}


static void
nxt_router_access_log_reopen_ready(nxt_task_t *task, nxt_port_recv_msg_t *msg,
    void *data)
{
    nxt_router_access_log_t         *access_log;
    nxt_router_access_log_reopen_t  *reopen;

    reopen = data;

    access_log = reopen->access_log;

    if (access_log == nxt_router->access_log) {

        if (nxt_slow_path(dup2(msg->fd[0], access_log->fd) == -1)) {
            nxt_alert(task, "dup2(%FD, %FD) failed %E",
                      msg->fd[0], access_log->fd, nxt_errno);

        } else {
            nxt_router_access_log_reopened(access_log);
        }
    }

    nxt_fd_close(msg->fd[0]);
    msg->fd[0] = -1;
    nxt_mp_release(reopen->mem_pool);
}


static void
nxt_router_access_log_reopen_error(nxt_task_t *task, nxt_port_recv_msg_t *msg,
    void *data)
{
    nxt_router_access_log_reopen_t  *reopen;

    reopen = data;

    nxt_mp_release(reopen->mem_pool);
}
