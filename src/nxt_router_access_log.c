
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


/*
 * The writer contract: at most three write() calls per record, the first and
 * two retries.  A call interrupted by a signal is not counted.
 */
#define NXT_ACCESS_LOG_WRITE_ATTEMPTS  3


static nxt_router_access_log_format_t *nxt_router_access_log_format_create(
    nxt_task_t *task, nxt_router_conf_t *rtcf, nxt_conf_value_t *value);
static void nxt_router_access_log_warn(nxt_task_t *task,
    const nxt_str_t *field, nxt_str_t *str);
static void nxt_router_access_log_writer(nxt_task_t *task,
    nxt_http_request_t *r, nxt_router_access_log_t *access_log,
    nxt_router_access_log_format_t *format);
static void nxt_router_access_log_record(nxt_task_t *task,
    nxt_router_access_log_t *access_log, u_char *start, size_t length);
static nxt_int_t nxt_router_access_log_write(
    nxt_router_access_log_t *access_log, u_char *start, size_t length);
static ssize_t nxt_router_access_log_write_fd(nxt_fd_t fd, u_char *start,
    size_t length);
static nxt_int_t nxt_router_access_log_repair(
    nxt_router_access_log_t *access_log);
static nxt_int_t nxt_router_access_log_mark_lost(
    nxt_router_access_log_t *access_log);
static void nxt_router_access_log_count_lost(
    nxt_router_access_log_t *access_log);
static void nxt_router_access_log_lost(nxt_task_t *task,
    nxt_router_access_log_t *access_log, const char *reason);
static void nxt_router_access_log_lost_warn(nxt_task_t *task,
    nxt_router_access_log_t *access_log, const char *reason);
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

    access_log = nxt_router_access_log_get(task, router, &alcf.path);
    if (nxt_slow_path(access_log == NULL)) {
        return NXT_ERROR;
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


/*
 * A log stays alive while an old configuration has requests, so the reuse
 * looks at all live logs, not only at the current one.  Two objects for one
 * file have two mutexes, and the records of the old and the new
 * configuration interleave.  The match is on the path string, so two
 * spellings of one file are two logs.  Only the router main thread adds a
 * log.
 */

nxt_router_access_log_t *
nxt_router_access_log_get(nxt_task_t *task, nxt_router_t *router,
    nxt_str_t *path)
{
    nxt_queue_link_t         *lnk;
    nxt_router_access_log_t  *access_log;

    nxt_thread_spin_lock(&router->lock);

    for (lnk = nxt_queue_first(&router->access_logs);
         lnk != nxt_queue_tail(&router->access_logs);
         lnk = nxt_queue_next(lnk))
    {
        access_log = nxt_queue_link_data(lnk, nxt_router_access_log_t, link);

        if (nxt_strstr_eq(path, &access_log->path)) {
            access_log->count++;

            nxt_thread_spin_unlock(&router->lock);

            return access_log;
        }
    }

    nxt_thread_spin_unlock(&router->lock);

    access_log = nxt_malloc(sizeof(nxt_router_access_log_t) + path->length);
    if (access_log == NULL) {
        nxt_alert(task, "failed to allocate access log structure");
        return NULL;
    }

    access_log->fd = -1;
    access_log->handler = &nxt_router_access_log_writer;
    access_log->count = 1;
    access_log->lock = 0;
    access_log->lost = 0;
    access_log->pending = 0;
    access_log->torn = 0;

    if (nxt_slow_path(nxt_thread_mutex_create(&access_log->mutex) != NXT_OK)) {
        nxt_alert(task, "failed to create the access log mutex");
        nxt_free(access_log);
        return NULL;
    }

    access_log->path.length = path->length;
    access_log->path.start = (u_char *) access_log
                             + sizeof(nxt_router_access_log_t);

    nxt_memcpy(access_log->path.start, path->start, path->length);

    nxt_thread_spin_lock(&router->lock);

    nxt_queue_insert_tail(&router->access_logs, &access_log->link);

    nxt_thread_spin_unlock(&router->lock);

    return access_log;
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
        nxt_router_access_log_lost(task, access_log,
                                   "no memory for the record context");
        goto close;
    }

    ctx->too_large = 0;

    if (format->tstr != NULL) {
        ret = nxt_router_access_log_text(task, r, ctx, format->tstr);

    } else {
        ret = nxt_router_access_log_json(task, r, ctx, format);
    }

    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_router_access_log_lost(task, access_log,
                                   ctx->too_large
                                   ? "the record is over the size bound"
                                   : "the record could not be built");
        goto close;
    }

    nxt_router_access_log_record(task, access_log, ctx->text.start,
                                 ctx->text.length);

close:

    /*
     * nxt_http_request_close_handler() calls this function and returns.  The
     * request is closed here, also when the record fails, for example when a
     * JS format throws.  r->logged is set, so this call does not log again.
     */

    nxt_http_request_close_handler(task, r, r->proto.any);
}


/*
 * One critical section for the whole record: the repair of a torn line, the
 * marker of the records lost before this one, and the record with its
 * retries.  Two engines cannot interleave a short write with another record.
 * A lost record is counted in the same section, so the next record of any
 * engine marks it.
 *
 * The marker comes before the record, where the lost records would have
 * been.  A record whose repair or marker fails is lost as well: written, it
 * would join a torn line, or come before the marker of the gap it follows.
 */

static void
nxt_router_access_log_record(nxt_task_t *task,
    nxt_router_access_log_t *access_log, u_char *start, size_t length)
{
    const char  *reason;

    nxt_thread_mutex_lock(&access_log->mutex);

    if (nxt_router_access_log_repair(access_log) != NXT_OK) {
        reason = "the previous record is still torn";

    } else if (nxt_router_access_log_mark_lost(access_log) != NXT_OK) {
        reason = "the loss marker could not be written";

    } else if (nxt_router_access_log_write(access_log, start, length)
               != NXT_OK)
    {
        reason = "the write did not complete";

    } else {
        reason = NULL;
    }

    if (reason != NULL) {
        nxt_router_access_log_count_lost(access_log);
    }

    nxt_thread_mutex_unlock(&access_log->mutex);

    if (reason != NULL) {
        nxt_router_access_log_lost_warn(task, access_log, reason);
    }
}


/*
 * One record is one write() and a bounded retry loop: a short write is
 * completed with the same buffer, so the record is never split into two
 * records or resumed after a later record has been appended.  The descriptor
 * is blocking, so a short write is rare (a full disk, a quota, a signal after
 * some bytes), and the loop makes at most NXT_ACCESS_LOG_WRITE_ATTEMPTS calls
 * that are not interrupted by a signal.  The bound keeps the mutex held for a
 * bounded number of calls: a descriptor that keeps taking less than the rest
 * ends the record as a failed write.  A write of no byte or an error other
 * than EINTR ends it too.  When a part of the buffer was written, the log now
 * ends inside a line and is torn: the next write repairs it first.  A write
 * that fails before its first byte leaves the log on a line boundary.  The
 * caller holds access_log->mutex; the loop alone is not atomic.
 */

static nxt_int_t
nxt_router_access_log_write(nxt_router_access_log_t *access_log, u_char *start,
    size_t length)
{
    size_t      left;
    ssize_t     n;
    u_char      *p;
    nxt_uint_t  attempts;

    p = start;
    left = length;
    attempts = 0;

    for ( ;; ) {
        n = nxt_router_access_log_write_fd(access_log->fd, p, left);

        if (n == -1 && nxt_errno == NXT_EINTR) {
            continue;
        }

        if (n <= 0) {
            break;
        }

        p += n;
        left -= n;

        if (left == 0) {
            return NXT_OK;
        }

        if (++attempts == NXT_ACCESS_LOG_WRITE_ATTEMPTS) {
            break;
        }
    }

    if (p != start) {
        access_log->torn = 1;
    }

    return NXT_ERROR;
}


#if (NXT_TESTS)

/*
 * The faults of the next write() calls, for the access log test, one per
 * call in order: -1 fails the call with ENOSPC before any byte, -2 fails it
 * with EINTR before any byte, and n >= 0 lets the call write at most n
 * bytes.  nxt_router_access_log_test_faults is the number of faults left;
 * the calls after them are not changed.
 */

const ssize_t  *nxt_router_access_log_test_fault;
nxt_uint_t     nxt_router_access_log_test_faults;

#endif


static ssize_t
nxt_router_access_log_write_fd(nxt_fd_t fd, u_char *start, size_t length)
{
#if (NXT_TESTS)
    ssize_t  fault;

    if (nxt_router_access_log_test_faults != 0) {
        nxt_router_access_log_test_faults--;
        fault = *nxt_router_access_log_test_fault++;

        if (fault < 0) {
            nxt_set_errno((fault == -2) ? NXT_EINTR : NXT_ENOSPC);
            return -1;
        }

        length = nxt_min(length, (size_t) fault);
    }
#endif

    return write(fd, start, length);
}


/*
 * A record whose write ended early leaves its prefix in the file.  The next
 * record cannot simply follow it: a reader would join both lines.  Write the
 * missing newline first, under the same mutex that serializes the records.
 */

static nxt_int_t
nxt_router_access_log_repair(nxt_router_access_log_t *access_log)
{
    u_char  newline;

    if (!access_log->torn) {
        return NXT_OK;
    }

    newline = '\n';

    if (nxt_router_access_log_write(access_log, &newline, 1) != NXT_OK) {
        return NXT_ERROR;
    }

    access_log->torn = 0;

    return NXT_OK;
}


/*
 * The marker of the records lost since the last marker, so a reader of the
 * log itself sees where the gap is.  The caller holds the mutex.  The count
 * is read under the lock and taken off after the write: a record that an
 * engine fails to build meanwhile is left for the next marker.  A failed
 * marker keeps the count, and a marker that ends inside its line leaves the
 * log torn; the next record repairs the line and writes the marker again.
 */

static nxt_int_t
nxt_router_access_log_mark_lost(nxt_router_access_log_t *access_log)
{
    u_char    buf[64];
    u_char    *p;
    uint64_t  pending;

    nxt_thread_spin_lock(&access_log->lock);
    pending = access_log->pending;
    nxt_thread_spin_unlock(&access_log->lock);

    if (pending == 0) {
        return NXT_OK;
    }

    p = nxt_sprintf(buf, buf + sizeof(buf),
                    "#freeunit: lost %uL record(s)\n", pending);

    if (nxt_router_access_log_write(access_log, buf, p - buf) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_thread_spin_lock(&access_log->lock);
    access_log->pending -= pending;
    nxt_thread_spin_unlock(&access_log->lock);

    return NXT_OK;
}


static void
nxt_router_access_log_count_lost(nxt_router_access_log_t *access_log)
{
    nxt_thread_spin_lock(&access_log->lock);

    access_log->lost++;
    access_log->pending++;

    nxt_thread_spin_unlock(&access_log->lock);
}


/*
 * A record that could not be built is counted under the lock alone: nothing
 * is written, so it does not wait for the write of another engine.  The next
 * record that gets through marks it.
 */

static void
nxt_router_access_log_lost(nxt_task_t *task,
    nxt_router_access_log_t *access_log, const char *reason)
{
    nxt_router_access_log_count_lost(access_log);

    nxt_router_access_log_lost_warn(task, access_log, reason);
}


/*
 * One warning per second, not one per lost record: a full disk must not turn
 * the access log loss into an error log flood.  The status counter carries
 * the exact number.  The limit must be at least 1: with 0 the first message
 * is allowed and every later one is suppressed forever.
 */

static void
nxt_router_access_log_lost_warn(nxt_task_t *task,
    nxt_router_access_log_t *access_log, const char *reason)
{
    static nxt_log_moderation_t  nxt_access_log_lost_log_moderation = {
        NXT_LOG_WARN, 1, "access log records lost", NXT_LOG_MODERATION
    };

    nxt_log_moderate(&nxt_access_log_lost_log_moderation, NXT_LOG_WARN,
                     task->log, "access log \"%V\": %s; the record is lost",
                     &access_log->path, reason);
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

    /*
     * The record and "\n".  nxt_conf_json_length() reports an unrepresentable
     * length as SIZE_MAX; nxt_size_add() refuses the addition rather than
     * wrapping, so the record never reaches nxt_mp_nget() with that size.
     *
     * A record past NXT_ACCESS_LOG_MAX_RECORD is dropped whole and counted;
     * the per-field cap is still to come.  The bound is checked on the
     * printed record, not on nxt_conf_json_length(): that is an upper bound
     * (it counts a "," after the last member), so a record exactly at the
     * bound would be dropped.
     */

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

    } else {
        nxt_queue_remove(&access_log->link);
    }

    nxt_thread_spin_unlock(lock);

    if (access_log != NULL) {

        nxt_thread_mutex_destroy(&access_log->mutex);

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

    /*
     * The reply comes after other configurations may have been applied, so
     * the log is held until then: its address is not reused by another log.
     */
    reopen->mem_pool = mp;
    reopen->access_log = access_log;

    nxt_router_access_log_use(&nxt_router->lock, access_log);

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

    if (reopen != NULL) {
        nxt_router_access_log_release(task, &nxt_router->lock, access_log);
    }

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

        /*
         * Under the mutex no record is partly written, so a record and its
         * retries go to one file.  The old file gets the newline of a torn
         * record, and the new one starts on a line boundary.  The records
         * lost before the reopen are marked in the new file.
         *
         * The router main engine handles this reply, so it may wait here for
         * one in-flight record write of another engine: the repair, the
         * marker and the record, each of at most three write() calls.  The
         * wait is accepted; a swap that does not wait could split a record
         * between the two files.
         */

        nxt_thread_mutex_lock(&access_log->mutex);

        if (nxt_router_access_log_repair(access_log) != NXT_OK) {
            nxt_alert(task, "access log \"%V\": the torn line was not "
                      "repaired before the reopen", &access_log->path);
        }

        if (nxt_fast_path(dup2(msg->fd[0], access_log->fd) != -1)) {
            access_log->torn = 0;

        } else {
            nxt_alert(task, "dup2(%FD, %FD) failed %E",
                      msg->fd[0], access_log->fd, nxt_errno);
        }

        nxt_thread_mutex_unlock(&access_log->mutex);
    }

    nxt_fd_close(msg->fd[0]);
    msg->fd[0] = -1;

    nxt_router_access_log_release(task, &nxt_router->lock, access_log);
    nxt_mp_release(reopen->mem_pool);
}


static void
nxt_router_access_log_reopen_error(nxt_task_t *task, nxt_port_recv_msg_t *msg,
    void *data)
{
    nxt_router_access_log_reopen_t  *reopen;

    reopen = data;

    nxt_router_access_log_release(task, &nxt_router->lock,
                                  reopen->access_log);
    nxt_mp_release(reopen->mem_pool);
}


#if (NXT_TESTS)

void
nxt_router_access_log_test_record(nxt_task_t *task,
    nxt_router_access_log_t *access_log, u_char *start, size_t length)
{
    nxt_router_access_log_record(task, access_log, start, length);
}


void
nxt_router_access_log_test_lost(nxt_task_t *task,
    nxt_router_access_log_t *access_log)
{
    nxt_router_access_log_lost(task, access_log,
                               "the record could not be built");
}


/* The reopen reply with the new descriptor fd; the handler closes it. */

nxt_int_t
nxt_router_access_log_test_reopen_ready(nxt_task_t *task,
    nxt_router_access_log_t *access_log, nxt_fd_t fd)
{
    nxt_mp_t                        *mp;
    nxt_port_recv_msg_t             msg;
    nxt_router_access_log_reopen_t  *reopen;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        nxt_fd_close(fd);
        return NXT_ERROR;
    }

    reopen = nxt_mp_get(mp, sizeof(nxt_router_access_log_reopen_t));
    if (nxt_slow_path(reopen == NULL)) {
        nxt_mp_destroy(mp);
        nxt_fd_close(fd);
        return NXT_ERROR;
    }

    reopen->mem_pool = mp;
    reopen->access_log = access_log;

    nxt_router_access_log_use(&nxt_router->lock, access_log);

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.fd[0] = fd;
    msg.fd[1] = -1;

    nxt_router_access_log_reopen_ready(task, &msg, reopen);

    return NXT_OK;
}

#endif
