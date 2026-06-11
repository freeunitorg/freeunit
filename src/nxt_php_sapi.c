/*
 * Copyright (C) Max Romanov
 * Copyright (C) Valentin V. Bartenev
 * Copyright (C) NGINX, Inc.
 */

#include "php.h"
#include "SAPI.h"
#include "php_main.h"
#include "php_variables.h"
#include "ext/standard/php_standard.h"

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_unit.h>
#include <nxt_unit_request.h>
#include <nxt_http.h>


#if (PHP_VERSION_ID >= 50400)
#define NXT_HAVE_PHP_IGNORE_CWD 1
#endif

#if (PHP_VERSION_ID >= 70100)
#define NXT_HAVE_PHP_LOG_MESSAGE_WITH_SYSLOG_TYPE 1
#else
#define NXT_HAVE_PHP_INTERRUPTS 1
#endif

#if (PHP_VERSION_ID >= 70000)
#define NXT_PHP7 1
#endif
#if (PHP_VERSION_ID >= 80000)
#define NXT_PHP8 1
#endif

/* PHP 8 */
#ifndef TSRMLS_CC
#define TSRMLS_CC
#define TSRMLS_DC
#define TSRMLS_D  void
#define TSRMLS_C
#endif


typedef struct {
    nxt_str_t  root;
    nxt_str_t  index;
    nxt_str_t  script_name;
    nxt_str_t  script_dirname;
    nxt_str_t  script_filename;
} nxt_php_target_t;


typedef struct {
    char                     *cookie;
    nxt_str_t                *root;
    nxt_str_t                *index;
    nxt_str_t                path_info;
    nxt_str_t                script_name;
    nxt_str_t                script_filename;
    nxt_str_t                script_dirname;
    nxt_unit_request_info_t  *req;

    uint8_t                  chdir;  /* 1 bit */
} nxt_php_run_ctx_t;


#if NXT_PHP8
typedef int (*nxt_php_disable_t)(const char *p, size_t size);
#elif NXT_PHP7
typedef int (*nxt_php_disable_t)(char *p, size_t size);
#else
typedef int (*nxt_php_disable_t)(char *p, uint TSRMLS_DC);
#endif

#if (PHP_VERSION_ID < 70200)
typedef void (*zif_handler)(INTERNAL_FUNCTION_PARAMETERS);
#endif


static nxt_int_t nxt_php_setup(nxt_task_t *task, nxt_process_t *process,
    nxt_common_app_conf_t *conf);
static nxt_int_t nxt_php_start(nxt_task_t *task, nxt_process_data_t *data);
static void nxt_php_cleanup_targets(void);
#if NXT_PHP_TRUEASYNC
static nxt_int_t nxt_php_async_load_entrypoint(nxt_task_t *task, nxt_str_t *entrypoint);
static bool nxt_php_activate_true_async(nxt_task_t *task);
static void nxt_php_suspend_coroutine(nxt_unit_ctx_t *ctx);
static int nxt_php_add_port(nxt_unit_ctx_t *ctx, nxt_unit_port_t *port);
static void nxt_php_remove_port(nxt_unit_t *unit, nxt_unit_ctx_t *ctx, nxt_unit_port_t *port);
static void nxt_php_quit_handler(nxt_unit_ctx_t *ctx);
static void nxt_php_shm_ack_handler(nxt_unit_ctx_t *ctx);
#endif /* NXT_PHP_TRUEASYNC */
static nxt_int_t nxt_php_set_target(nxt_task_t *task, nxt_php_target_t *target,
    nxt_conf_value_t *conf);
static nxt_int_t nxt_php_set_ini_path(nxt_task_t *task, nxt_str_t *path,
    char *workdir);
static void nxt_php_set_options(nxt_task_t *task, nxt_conf_value_t *options,
    int type);
static nxt_int_t nxt_php_alter_option(nxt_str_t *name, nxt_str_t *value,
    int type);
#ifdef NXT_PHP8
static void nxt_php_disable_functions(nxt_str_t *str);
#endif
#if (PHP_VERSION_ID < 80500)
static void nxt_php_disable(nxt_task_t *task, const char *type,
    nxt_str_t *value, char **ptr, nxt_php_disable_t disable);
#endif

static nxt_int_t nxt_php_dirname(const nxt_str_t *file, nxt_str_t *dir);
static void nxt_php_str_trim_trail(nxt_str_t *str, u_char t);
static void nxt_php_str_trim_lead(nxt_str_t *str, u_char t);
nxt_inline u_char *nxt_realpath(const void *c);

static nxt_int_t nxt_php_do_301(nxt_unit_request_info_t *req);
static nxt_int_t nxt_php_handle_fs_err(nxt_unit_request_info_t *req);

static void nxt_php_request_handler(nxt_unit_request_info_t *req);
#if NXT_PHP_TRUEASYNC
static void nxt_php_request_handler_async(nxt_unit_request_info_t *req);
#endif /* NXT_PHP_TRUEASYNC */
static void nxt_php_dynamic_request(nxt_php_run_ctx_t *ctx,
    nxt_unit_request_t *r);
#if NXT_PHP_TRUEASYNC
static void nxt_php_scope_init_superglobals(zend_async_scope_t *scope);
static void nxt_php_scope_populate_superglobals(zend_async_scope_t *scope);
#endif /* NXT_PHP_TRUEASYNC */
#if (PHP_VERSION_ID < 70400)
static void nxt_zend_stream_init_fp(zend_file_handle *handle, FILE *fp,
    const char *filename);
#endif
static void nxt_php_execute(nxt_php_run_ctx_t *ctx, nxt_unit_request_t *r);
nxt_inline void nxt_php_vcwd_chdir(nxt_unit_request_info_t *req, u_char *dir);

static int nxt_php_startup(sapi_module_struct *sapi_module);
static int nxt_php_send_headers(sapi_headers_struct *sapi_headers TSRMLS_DC);
static void *nxt_php_hash_str_find_ptr(const HashTable *ht,
    const nxt_str_t *str);
static char *nxt_php_read_cookies(TSRMLS_D);
static void nxt_php_set_sptr(nxt_unit_request_info_t *req, const char *name,
    nxt_unit_sptr_t *v, uint32_t len, zval *track_vars_array TSRMLS_DC);
nxt_inline void nxt_php_set_str(nxt_unit_request_info_t *req, const char *name,
    nxt_str_t *s, zval *track_vars_array TSRMLS_DC);
static void nxt_php_set_cstr(nxt_unit_request_info_t *req, const char *name,
    const char *str, uint32_t len, zval *track_vars_array TSRMLS_DC);
void nxt_php_register_variables(zval *track_vars_array TSRMLS_DC);
#if NXT_PHP_TRUEASYNC
static void nxt_php_register_variables_async(nxt_unit_request_info_t *req,
    nxt_php_run_ctx_t *ctx, zval *track_vars_array TSRMLS_DC);
#endif /* NXT_PHP_TRUEASYNC */
#if NXT_PHP8
static void nxt_php_log_message(const char *message, int syslog_type_int);
#else
#ifdef NXT_HAVE_PHP_LOG_MESSAGE_WITH_SYSLOG_TYPE
static void nxt_php_log_message(char *message, int syslog_type_int);
#else
static void nxt_php_log_message(char *message TSRMLS_DC);
#endif
#endif

#ifdef NXT_PHP7
static size_t nxt_php_unbuffered_write(const char *str,
    size_t str_length TSRMLS_DC);
static size_t nxt_php_read_post(char *buffer, size_t count_bytes TSRMLS_DC);
#else
static int nxt_php_unbuffered_write(const char *str, uint str_length TSRMLS_DC);
static int nxt_php_read_post(char *buffer, uint count_bytes TSRMLS_DC);
#endif


#ifdef NXT_PHP7
#if (PHP_VERSION_ID < 70200)
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fastcgi_finish_request, 0, 0,
                                        _IS_BOOL, NULL, 0)
#else
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fastcgi_finish_request, 0, 0,
                                        _IS_BOOL, 0)
#endif
#else /* PHP5 */
ZEND_BEGIN_ARG_INFO_EX(arginfo_fastcgi_finish_request, 0, 0, 0)
#endif
ZEND_END_ARG_INFO()

ZEND_FUNCTION(fastcgi_finish_request);

PHP_MINIT_FUNCTION(nxt_php_ext);
ZEND_NAMED_FUNCTION(nxt_php_chdir);

/* PHP extension functions */
static const zend_function_entry  nxt_php_ext_functions[] = {
    ZEND_FE(fastcgi_finish_request, arginfo_fastcgi_finish_request)
    ZEND_FE_END
};

zif_handler       nxt_php_chdir_handler;
zend_auto_global  *nxt_php_server_ag;


static zend_module_entry  nxt_php_unit_module = {
    STANDARD_MODULE_HEADER,
    "unit",
    nxt_php_ext_functions,       /* function table */
    PHP_MINIT(nxt_php_ext),      /* initialization */
    NULL,                        /* shutdown */
    NULL,                        /* request initialization */
    NULL,                        /* request shutdown */
    NULL,                        /* information */
    NXT_VERSION,
    STANDARD_MODULE_PROPERTIES
};


PHP_MINIT_FUNCTION(nxt_php_ext)
{
    zend_function    *func;

    static const nxt_str_t  chdir = nxt_string("chdir");

    func = nxt_php_hash_str_find_ptr(CG(function_table), &chdir);
    if (nxt_slow_path(func == NULL)) {
        return FAILURE;
    }

    nxt_php_chdir_handler = func->internal_function.handler;
    func->internal_function.handler = nxt_php_chdir;

#if NXT_PHP_TRUEASYNC
    /* Register NginxUnit PHP extension classes (TrueAsync only) */
    if (nxt_php_extension_init() != NXT_OK) {
        return FAILURE;
    }
#endif /* NXT_PHP_TRUEASYNC */

    return SUCCESS;
}


ZEND_NAMED_FUNCTION(nxt_php_chdir)
{
    nxt_php_run_ctx_t  *ctx;

    ctx = SG(server_context);

    if (nxt_fast_path(ctx != NULL)) {
        ctx->chdir = 1;
    }

    nxt_php_chdir_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}


PHP_FUNCTION(fastcgi_finish_request)
{
    zend_auto_global   *ag;
    nxt_php_run_ctx_t  *ctx;

    if (nxt_slow_path(zend_parse_parameters_none() == FAILURE)) {
#ifdef NXT_PHP8
        RETURN_THROWS();
#else
        return;
#endif
    }

    ctx = SG(server_context);

    if (nxt_slow_path(ctx == NULL || ctx->req == NULL)) {
        RETURN_FALSE;
    }

#ifdef NXT_PHP7
    php_output_end_all();
    php_header();
#else
#ifdef PHP_OUTPUT_NEWAPI
    php_output_end_all(TSRMLS_C);
#else
    php_end_ob_buffers(1 TSRMLS_CC);
#endif

    php_header(TSRMLS_C);
#endif

    ag = nxt_php_server_ag;

    if (ag->armed) {
#ifdef NXT_PHP7
        ag->armed = ag->auto_global_callback(ag->name);
#else
        ag->armed = ag->auto_global_callback(ag->name, ag->name_len TSRMLS_CC);
#endif
    }

    nxt_unit_request_done(ctx->req, NXT_UNIT_OK);
    ctx->req = NULL;

    PG(connection_status) = PHP_CONNECTION_ABORTED;
#ifdef NXT_PHP7
    php_output_set_status(PHP_OUTPUT_DISABLED);
#else
#ifdef PHP_OUTPUT_NEWAPI
    php_output_set_status(PHP_OUTPUT_DISABLED TSRMLS_CC);
#else
    php_output_set_status(0 TSRMLS_CC);
#endif
#endif

    RETURN_TRUE;
}


static sapi_module_struct  nxt_php_sapi_module =
{
    (char *) "cli-server",
    (char *) "unit",

    nxt_php_startup,             /* startup */
    php_module_shutdown_wrapper, /* shutdown */

    NULL,                        /* activate */
    NULL,                        /* deactivate */

    nxt_php_unbuffered_write,    /* unbuffered write */
    NULL,                        /* flush */
    NULL,                        /* get uid */
    NULL,                        /* getenv */

    php_error,                   /* error handler */

    NULL,                        /* header handler */
    nxt_php_send_headers,        /* send headers handler */
    NULL,                        /* send header handler */

    nxt_php_read_post,           /* read POST data */
    nxt_php_read_cookies,        /* read Cookies */

    nxt_php_register_variables,  /* register server variables */
    nxt_php_log_message,         /* log message */
    NULL,                        /* get request time */
    NULL,                        /* terminate process */

    NULL,                        /* php_ini_path_override */
#ifdef NXT_HAVE_PHP_INTERRUPTS
    NULL,                        /* block_interruptions */
    NULL,                        /* unblock_interruptions */
#endif
    NULL,                        /* default_post_reader */
    NULL,                        /* treat_data */
    NULL,                        /* executable_location */

    0,                           /* php_ini_ignore */
#ifdef NXT_HAVE_PHP_IGNORE_CWD
    1,                           /* php_ini_ignore_cwd */
#endif
    NULL,                        /* get_fd */

    NULL,                        /* force_http_10 */

    NULL,                        /* get_target_uid */
    NULL,                        /* get_target_gid */

    NULL,                        /* input_filter */

    NULL,                        /* ini_defaults */
    0,                           /* phpinfo_as_text */

    NULL,                        /* ini_entries */
    NULL,                        /* additional_functions */
    NULL,                        /* input_filter_init */
#if NXT_PHP_PRE_REQUEST_INIT
    NULL,                        /* pre_request_init */
#endif
};


static uint32_t  compat[] = {
    NXT_VERNUM, NXT_DEBUG,
};


NXT_EXPORT nxt_app_module_t  nxt_app_module = {
    sizeof(compat),
    compat,
    nxt_string("php"),
    PHP_VERSION,
    NULL,
    0,
    nxt_php_setup,
    nxt_php_start,
};


static nxt_php_target_t  *nxt_php_targets;
static nxt_uint_t        nxt_php_targets_count;
static nxt_int_t         nxt_php_last_target = -1;

/* Global Unit context - needed by nxt_php_extension.c */
nxt_unit_ctx_t              *nxt_php_unit_ctx;

#if defined(ZTS) && (PHP_VERSION_ID < 70400)
static void              ***tsrm_ls;
#endif


static nxt_int_t
nxt_php_setup(nxt_task_t *task, nxt_process_t *process,
    nxt_common_app_conf_t *conf)
{
    nxt_str_t           ini_path;
    nxt_int_t           ret;
    nxt_conf_value_t    *value;
    nxt_php_app_conf_t  *c;

    static const nxt_str_t  file_str = nxt_string("file");
    static const nxt_str_t  user_str = nxt_string("user");
    static const nxt_str_t  admin_str = nxt_string("admin");


    c = &conf->u.php;

#ifdef ZTS

#if (PHP_VERSION_ID >= 70400)
    php_tsrm_startup();
#else
    tsrm_startup(1, 1, 0, NULL);
    tsrm_ls = ts_resource(0);
#endif

#endif

#if defined(NXT_PHP7) && defined(ZEND_SIGNALS)

#if (NXT_ZEND_SIGNAL_STARTUP)
    zend_signal_startup();
#elif defined(ZTS)
#error PHP is built with thread safety and broken signals.
#endif

#endif

    sapi_startup(&nxt_php_sapi_module);

    if (c->options != NULL) {
        value = nxt_conf_get_object_member(c->options, &file_str, NULL);

        if (value != NULL) {
            nxt_conf_get_string(value, &ini_path);

            ret = nxt_php_set_ini_path(task, &ini_path,
                                       conf->working_directory);

            if (nxt_slow_path(ret != NXT_OK)) {
                return NXT_ERROR;
            }
        }
    }

    if (nxt_slow_path(nxt_php_startup(&nxt_php_sapi_module) == FAILURE)) {
        nxt_alert(task, "failed to initialize SAPI module and extension");
        return NXT_ERROR;
    }

    if (c->options != NULL) {
        value = nxt_conf_get_object_member(c->options, &admin_str, NULL);
        nxt_php_set_options(task, value, ZEND_INI_SYSTEM);

        value = nxt_conf_get_object_member(c->options, &user_str, NULL);
        nxt_php_set_options(task, value, ZEND_INI_USER);
    }

#ifdef NXT_PHP7
    nxt_php_server_ag = zend_hash_str_find_ptr(CG(auto_globals), "_SERVER",
                                               nxt_length("_SERVER"));
#else
    zend_hash_quick_find(CG(auto_globals), "_SERVER", sizeof("_SERVER"),
                         zend_hash_func("_SERVER", sizeof("_SERVER")),
                         (void **) &nxt_php_server_ag);
#endif
    if (nxt_slow_path(nxt_php_server_ag == NULL)) {
        nxt_alert(task, "failed to find $_SERVER auto global");
        return NXT_ERROR;
    }

    return NXT_OK;
}


#if NXT_PHP_TRUEASYNC

/**
 * Add pending write to drain_queue
 */
nxt_php_pending_write_t *
nxt_php_drain_queue_add(nxt_unit_ctx_t *ctx,
                        nxt_unit_request_info_t *req,
                        zend_string *str,
                        size_t offset)
{
    nxt_php_async_ctx_data_t *async_data = ctx->data;
    nxt_php_pending_write_t  *pw;

    if (async_data == NULL) {
        return NULL;
    }

    pw = malloc(sizeof(nxt_php_pending_write_t));
    if (pw == NULL) {
        return NULL;
    }

    pw->req = req;
    pw->offset = offset;

    /* Increase refcount - string won't be freed */
    pw->data = zend_string_copy(str);

    nxt_queue_insert_tail(&async_data->drain_queue, &pw->link);

    return pw;
}


/**
 * Remove and free pending write from drain_queue
 */
static void
nxt_php_drain_queue_remove(nxt_php_pending_write_t *pw)
{
    nxt_queue_remove(&pw->link);

    /* Decrease refcount - will be freed when refcount reaches 0 */
    zend_string_release(pw->data);

    free(pw);
}


/**
 * Callback function called when port fd becomes readable
 */
static void
nxt_php_port_event_callback(zend_async_event_t *event, zend_async_event_callback_t *callback, void *result, zend_object *exception)
{
    nxt_php_port_event_data_t  *event_data;

    /* Get pointer to our extra data using extra_offset */
    event_data = (nxt_php_port_event_data_t *)((char *)event + event->extra_offset);

    /* Process messages from this port */
    nxt_unit_process_port_msg(event_data->ctx, event_data->port);
}


/**
 * Callback to add port - called by Unit for each port
 */
static int
nxt_php_add_port(nxt_unit_ctx_t *ctx, nxt_unit_port_t *port)
{
    zend_async_poll_event_t      *poll_event;
    nxt_php_port_event_data_t    *event_data;

    nxt_php_unit_ctx = ctx;

    /* Skip ports without input fd */
    if (port->in_fd == -1) {
        return NXT_UNIT_OK;
    }

    /* Set port to non-blocking mode */
    if (fcntl(port->in_fd, F_SETFL, O_NONBLOCK) == -1) {
        nxt_unit_warn(ctx, "fcntl(%d, O_NONBLOCK) failed: %s (%d)",
                      port->in_fd, strerror(errno), errno);
        return NXT_UNIT_ERROR;
    }

    /* Create TrueAsync poll event with extra space for our data */
    poll_event = ZEND_ASYNC_NEW_POLL_EVENT_EX(
        port->in_fd, false, ASYNC_READABLE, sizeof(nxt_php_port_event_data_t)
    );

    if (poll_event == NULL) {
        nxt_unit_alert(ctx, "Failed to create TrueAsync poll event for port fd=%d", port->in_fd);
        return NXT_UNIT_ERROR;
    }

    /* Get pointer to our extra data */
    event_data = (nxt_php_port_event_data_t *)((char *)poll_event + poll_event->base.extra_offset);
    event_data->ctx = ctx;
    event_data->port = port;

    /* Register callback with poll event */
    if (!poll_event->base.add_callback(&poll_event->base, ZEND_ASYNC_EVENT_CALLBACK(nxt_php_port_event_callback))) {
        nxt_unit_alert(ctx, "Failed to add callback for port fd=%d", port->in_fd);
        poll_event->base.dispose(&poll_event->base);
        return NXT_UNIT_ERROR;
    }

    /* Start polling */
    if (!poll_event->base.start(&poll_event->base)) {
        nxt_unit_alert(ctx, "Failed to start polling for port fd=%d", port->in_fd);
        poll_event->base.dispose(&poll_event->base);
        return NXT_UNIT_ERROR;
    }

    /* Save poll_event in port->data for cleanup */
    port->data = poll_event;

    return NXT_UNIT_OK;
}


/**
 * Callback to remove port
 */
static void
nxt_php_remove_port(nxt_unit_t *unit, nxt_unit_ctx_t *ctx, nxt_unit_port_t *port)
{
    zend_async_poll_event_t  *poll_event;

    if (port->data != NULL) {
        poll_event = (zend_async_poll_event_t *) port->data;

        /* Dispose using TrueAsync cleanup */
        poll_event->base.dispose(&poll_event->base);

        port->data = NULL;
    }
}


/**
 * SHM ACK handler - called when Router releases shared memory
 */
static void
nxt_php_shm_ack_handler(nxt_unit_ctx_t *ctx)
{
    nxt_php_async_ctx_data_t *async_data = ctx->data;
    nxt_queue_link_t         *lnk;
    nxt_php_pending_write_t  *pw;
    ssize_t                  res;
    size_t                   remaining;

    if (async_data == NULL) {
        return;
    }

    /* Process all pending writes in drain_queue */
    lnk = nxt_queue_first(&async_data->drain_queue);

    while (lnk != nxt_queue_tail(&async_data->drain_queue)) {
        pw = nxt_container_of(lnk, nxt_php_pending_write_t, link);
        lnk = nxt_queue_next(lnk);  /* Save next element before potential removal */

        remaining = ZSTR_LEN(pw->data) - pw->offset;

        /* Try to send remaining data */
        res = nxt_unit_response_write_nb(pw->req,
                                         ZSTR_VAL(pw->data) + pw->offset,
                                         remaining,
                                         0);

        if (res < 0) {
            /* Error - remove from queue */
            nxt_unit_warn(ctx, "drain_queue: write error, removing pending write");
            nxt_php_drain_queue_remove(pw);
            continue;
        }

        if (res == 0) {
            /* Still no space - stop processing */
            return;
        }

        pw->offset += res;

        if (pw->offset >= ZSTR_LEN(pw->data)) {
            /* ALL SENT! Remove from queue */
            nxt_php_drain_queue_remove(pw);
        }
    }
}

#endif /* NXT_PHP_TRUEASYNC */


static nxt_int_t
nxt_php_start(nxt_task_t *task, nxt_process_data_t *data)
{
    uint32_t               next;
    nxt_int_t              ret;
    nxt_str_t              name;
    nxt_uint_t             n;
    nxt_unit_ctx_t         *unit_ctx;
    nxt_unit_init_t        php_init;
    nxt_conf_value_t       *value;
    nxt_php_app_conf_t     *c;
    nxt_common_app_conf_t  *conf;


    conf = data->app;
    c = &conf->u.php;

    n = (c->targets != NULL) ? nxt_conf_object_members_count(c->targets) : 1;

    nxt_php_targets = nxt_zalloc(sizeof(nxt_php_target_t) * n);
    if (nxt_slow_path(nxt_php_targets == NULL)) {
        return NXT_ERROR;
    }

    nxt_php_targets_count = n;

    if (c->targets != NULL) {
        next = 0;

        for (n = 0; /* void */; n++) {
            value = nxt_conf_next_object_member(c->targets, &name, &next);
            if (value == NULL) {
                break;
            }

            ret = nxt_php_set_target(task, &nxt_php_targets[n], value);
            if (nxt_slow_path(ret != NXT_OK)) {
                return NXT_ERROR;
            }
        }

    } else {
        ret = nxt_php_set_target(task, &nxt_php_targets[0], conf->self);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }
    }

    ret = nxt_unit_default_init(task, &php_init, conf);
    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_alert(task, "nxt_unit_default_init() failed");
        return ret;
    }

    /* Choose request handler based on mode */
#if NXT_PHP_TRUEASYNC
    if (c->async && c->entrypoint.length > 0) {
        nxt_debug(task, "PHP HTTP Server mode enabled with entrypoint: %V", &c->entrypoint);
        php_init.callbacks.add_port = nxt_php_add_port;
        php_init.callbacks.remove_port = nxt_php_remove_port;
        php_init.callbacks.request_handler = nxt_php_request_handler_async;
        php_init.callbacks.quit = nxt_php_quit_handler;
        php_init.callbacks.shm_ack_handler = nxt_php_shm_ack_handler;

        if (nxt_slow_path(nxt_php_async_load_entrypoint(task, &c->entrypoint) != NXT_OK)) {
            nxt_alert(task, "failed to load entrypoint script");
            return NXT_ERROR;
        }

        if (nxt_php_request_callback == NULL) {
            nxt_alert(task, "TrueAsync: Request callback not registered in the entrypoint script!");
            return NXT_ERROR;
        }

        if(nxt_slow_path(!nxt_php_activate_true_async(task))) {
            return NXT_ERROR;
        }
    } else {
#endif /* NXT_PHP_TRUEASYNC */
        nxt_debug(task, "PHP standard mode");
        php_init.callbacks.request_handler = nxt_php_request_handler;
#if NXT_PHP_TRUEASYNC
    }
#endif /* NXT_PHP_TRUEASYNC */

    unit_ctx = nxt_unit_init(&php_init);
    if (nxt_slow_path(unit_ctx == NULL)) {
        return NXT_ERROR;
    }

    nxt_php_unit_ctx = unit_ctx;

#if NXT_PHP_TRUEASYNC
    /* Initialize async context data for drain_queue */
    if (c->async && c->entrypoint.length > 0) {
        nxt_php_async_ctx_data_t *async_data;

        async_data = malloc(sizeof(nxt_php_async_ctx_data_t));
        if (async_data == NULL) {
            nxt_alert(task, "Failed to allocate async context data");
            nxt_unit_done(unit_ctx);
            return NXT_ERROR;
        }

        nxt_queue_init(&async_data->drain_queue);
        async_data->ctx = unit_ctx;

        unit_ctx->data = async_data;
    }

    if (c->async && c->entrypoint.length > 0) {
        /* Suspend main coroutine until the server continues to operate successfully */
        nxt_php_suspend_coroutine(nxt_php_unit_ctx);
    } else {
        nxt_unit_run(nxt_php_unit_ctx);
    }
#else
    nxt_unit_run(nxt_php_unit_ctx);
#endif /* NXT_PHP_TRUEASYNC */
    nxt_unit_done(nxt_php_unit_ctx);

    /* Clean up allocated memory before exit */
    nxt_php_cleanup_targets();

    exit(0);

    return NXT_OK;
}


static u_char  nxt_php_index_default[] = "index.php";


static nxt_int_t
nxt_php_set_target(nxt_task_t *task, nxt_php_target_t *target,
    nxt_conf_value_t *conf)
{
    u_char            *tmp, *p;
    nxt_str_t         str;
    nxt_int_t         ret;
    nxt_conf_value_t  *value;

    static const nxt_str_t  root_str = nxt_string("root");
    static const nxt_str_t  script_str = nxt_string("script");
    static const nxt_str_t  index_str = nxt_string("index");
    static const nxt_str_t  entrypoint_str = nxt_string("entrypoint");

    value = nxt_conf_get_object_member(conf, &root_str, NULL);

    nxt_conf_get_string(value, &str);

    tmp = nxt_malloc(str.length + 1);
    if (nxt_slow_path(tmp == NULL)) {
        return NXT_ERROR;
    }

    p = tmp;

    p = nxt_cpymem(p, str.start, str.length);
    *p = '\0';

    p = nxt_realpath(tmp);
    if (nxt_slow_path(p == NULL)) {
        nxt_alert(task, "root realpath(%s) failed %E", tmp, nxt_errno);
        return NXT_ERROR;
    }

    nxt_free(tmp);

    target->root.length = nxt_strlen(p);
    target->root.start = p;

    nxt_php_str_trim_trail(&target->root, '/');

    value = nxt_conf_get_object_member(conf, &script_str, NULL);

    if (value != NULL) {
        nxt_conf_get_string(value, &str);

        nxt_php_str_trim_lead(&str, '/');

        tmp = nxt_malloc(target->root.length + 1 + str.length + 1);
        if (nxt_slow_path(tmp == NULL)) {
            return NXT_ERROR;
        }

        p = tmp;

        p = nxt_cpymem(p, target->root.start, target->root.length);
        *p++ = '/';

        p = nxt_cpymem(p, str.start, str.length);
        *p = '\0';

        p = nxt_realpath(tmp);
        if (nxt_slow_path(p == NULL)) {
            nxt_alert(task, "script realpath(%s) failed %E", tmp, nxt_errno);
            nxt_free(tmp);
            return NXT_ERROR;
        }

        nxt_free(tmp);

        target->script_filename.length = nxt_strlen(p);
        target->script_filename.start = p;

        if (!nxt_str_start(&target->script_filename,
                           target->root.start, target->root.length))
        {
            nxt_alert(task, "script is not under php root");
            nxt_free(p);
            return NXT_ERROR;
        }

        ret = nxt_php_dirname(&target->script_filename,
                              &target->script_dirname);
        if (nxt_slow_path(ret != NXT_OK)) {
            nxt_free(target->script_filename.start);
            return NXT_ERROR;
        }

        target->script_name.length = target->script_filename.length
                                     - target->root.length;
        target->script_name.start = target->script_filename.start
                                    + target->root.length;

    } else {
        /* Check for entrypoint (async mode) */
        value = nxt_conf_get_object_member(conf, &entrypoint_str, NULL);

        if (value != NULL) {
            nxt_conf_get_string(value, &str);

            /* Check if entrypoint is an absolute path */
            if (str.length > 0 && str.start[0] == '/') {
                /* Absolute path - use it directly */
                tmp = nxt_malloc(str.length + 1);
                if (nxt_slow_path(tmp == NULL)) {
                    return NXT_ERROR;
                }

                nxt_memcpy(tmp, str.start, str.length);
                tmp[str.length] = '\0';

            } else {
                /* Relative path - prepend root */
                nxt_php_str_trim_lead(&str, '/');

                tmp = nxt_malloc(target->root.length + 1 + str.length + 1);
                if (nxt_slow_path(tmp == NULL)) {
                    return NXT_ERROR;
                }

                p = tmp;

                p = nxt_cpymem(p, target->root.start, target->root.length);
                *p++ = '/';

                p = nxt_cpymem(p, str.start, str.length);
                *p = '\0';
            }

            p = nxt_realpath(tmp);
            if (nxt_slow_path(p == NULL)) {
                nxt_alert(task, "entrypoint realpath(%s) failed %E", tmp, nxt_errno);
                nxt_free(tmp);
                return NXT_ERROR;
            }

            nxt_free(tmp);

            target->script_filename.length = nxt_strlen(p);
            target->script_filename.start = p;

            if (!nxt_str_start(&target->script_filename,
                               target->root.start, target->root.length))
            {
                nxt_alert(task, "entrypoint is not under php root");
                nxt_free(p);
                return NXT_ERROR;
            }

            ret = nxt_php_dirname(&target->script_filename,
                                  &target->script_dirname);
            if (nxt_slow_path(ret != NXT_OK)) {
                nxt_free(p);
                return NXT_ERROR;
            }

            target->script_name.length = target->script_filename.length
                                         - target->root.length;
            target->script_name.start = target->script_filename.start
                                        + target->root.length;
        }

        value = nxt_conf_get_object_member(conf, &index_str, NULL);

        if (value != NULL) {
            nxt_conf_get_string(value, &str);

            tmp = nxt_malloc(str.length + 1);
            if (nxt_slow_path(tmp == NULL)) {
                return NXT_ERROR;
            }

            nxt_memcpy(tmp, str.start, str.length);
            tmp[str.length] = '\0';

            target->index.length = str.length;
            target->index.start = tmp;

        } else {
            target->index.start = nxt_php_index_default;
            target->index.length = sizeof("index.php") - 1;
        }
    }

    return NXT_OK;
}


static void
nxt_php_cleanup_targets(void)
{
    nxt_uint_t  i;

    if (nxt_php_targets == NULL) {
        return;
    }

    for (i = 0; i < nxt_php_targets_count; i++) {
        nxt_php_target_t *target = &nxt_php_targets[i];

        /* Free root (allocated by nxt_realpath) */
        if (target->root.start != NULL) {
            nxt_free(target->root.start);
        }

        /* Free script_filename (allocated by nxt_realpath) */
        if (target->script_filename.start != NULL) {
            nxt_free(target->script_filename.start);
        }

        /* Free script_dirname (allocated by nxt_php_dirname) */
        if (target->script_dirname.start != NULL) {
            nxt_free(target->script_dirname.start);
        }

        /* Free index if it was heap-allocated (not the static default) */
        if (target->index.start != NULL &&
            target->index.start != nxt_php_index_default) {
            nxt_free(target->index.start);
        }
    }

    nxt_free(nxt_php_targets);
    nxt_php_targets = NULL;
    nxt_php_targets_count = 0;
}


static nxt_int_t
nxt_php_set_ini_path(nxt_task_t *task, nxt_str_t *ini_path, char *workdir)
{
    size_t  wdlen;
    u_char  *p, *start;

    if (ini_path->start[0] == '/' || workdir == NULL) {
        p = nxt_malloc(ini_path->length + 1);
        if (nxt_slow_path(p == NULL)) {
            return NXT_ERROR;
        }

        start = p;

    } else {
        wdlen = nxt_strlen(workdir);

        p = nxt_malloc(wdlen + ini_path->length + 2);
        if (nxt_slow_path(p == NULL)) {
            return NXT_ERROR;
        }

        start = p;

        p = nxt_cpymem(p, workdir, wdlen);

        if (workdir[wdlen - 1] != '/') {
            *p++ = '/';
        }
    }

    p = nxt_cpymem(p, ini_path->start, ini_path->length);
    *p = '\0';

    nxt_php_sapi_module.php_ini_path_override = (char *) start;

    return NXT_OK;
}


static void
nxt_php_set_options(nxt_task_t *task, nxt_conf_value_t *options, int type)
{
    uint32_t          next;
    nxt_str_t         name, value;
    nxt_conf_value_t  *value_obj;

    if (options != NULL) {
        next = 0;

        for ( ;; ) {
            value_obj = nxt_conf_next_object_member(options, &name, &next);
            if (value_obj == NULL) {
                break;
            }

            nxt_conf_get_string(value_obj, &value);

            if (nxt_php_alter_option(&name, &value, type) != NXT_OK) {
                nxt_log(task, NXT_LOG_ERR,
                        "setting PHP option \"%V: %V\" failed", &name, &value);
                continue;
            }

            if (nxt_str_eq(&name, "disable_functions", 17)) {
#ifdef NXT_PHP8
                nxt_php_disable_functions(&value);
#else
                nxt_php_disable(task, "function", &value,
                                &PG(disable_functions),
                                zend_disable_function);
#endif
                continue;
            }

            if (nxt_str_eq(&name, "disable_classes", 15)) {
#if (PHP_VERSION_ID < 80500)
                nxt_php_disable(task, "class", &value,
                                &PG(disable_classes),
                                zend_disable_class);
#endif
                continue;
            }
        }
    }
}


#ifdef NXT_PHP7

static nxt_int_t
nxt_php_alter_option(nxt_str_t *name, nxt_str_t *value, int type)
{
    zend_string     *zs;
    zend_ini_entry  *ini_entry;

    ini_entry = nxt_php_hash_str_find_ptr(EG(ini_directives), name);
    if (nxt_slow_path(ini_entry == NULL)) {
        return NXT_ERROR;
    }

    /* PHP exits on memory allocation errors. */
    zs = zend_string_init((char *) value->start, value->length, 1);

    if (ini_entry->on_modify
        && ini_entry->on_modify(ini_entry, zs, ini_entry->mh_arg1,
                                ini_entry->mh_arg2, ini_entry->mh_arg3,
                                ZEND_INI_STAGE_ACTIVATE)
           != SUCCESS)
    {
        zend_string_release(zs);
        return NXT_ERROR;
    }

    ini_entry->value = zs;
    ini_entry->modifiable = type;

    return NXT_OK;
}

#else  /* PHP 5. */

static nxt_int_t
nxt_php_alter_option(nxt_str_t *name, nxt_str_t *value, int type)
{
    char            *cstr;
    zend_ini_entry  *ini_entry;

    ini_entry = nxt_php_hash_str_find_ptr(EG(ini_directives), name);
    if (nxt_slow_path(ini_entry == NULL)) {
        return NXT_ERROR;
    }

    cstr = nxt_malloc(value->length + 1);
    if (nxt_slow_path(cstr == NULL)) {
        return NXT_ERROR;
    }

    nxt_memcpy(cstr, value->start, value->length);
    cstr[value->length] = '\0';

    if (ini_entry->on_modify
        && ini_entry->on_modify(ini_entry, cstr, value->length,
                                ini_entry->mh_arg1, ini_entry->mh_arg2,
                                ini_entry->mh_arg3, ZEND_INI_STAGE_ACTIVATE
                                TSRMLS_CC)
           != SUCCESS)
    {
        nxt_free(cstr);
        return NXT_ERROR;
    }

    ini_entry->value = cstr;
    ini_entry->value_length = value->length;
    ini_entry->modifiable = type;

    return NXT_OK;
}

#endif


#ifdef NXT_PHP8

static void
nxt_php_disable_functions(nxt_str_t *str)
{
    char  *p;

    p = nxt_malloc(str->length + 1);
    if (nxt_slow_path(p == NULL)) {
        return;
    }

    nxt_memcpy(p, str->start, str->length);
    p[str->length] = '\0';

    zend_disable_functions(p);

    nxt_free(p);
}

#endif


#if (PHP_VERSION_ID < 80500)
static void
nxt_php_disable(nxt_task_t *task, const char *type, nxt_str_t *value,
    char **ptr, nxt_php_disable_t disable)
{
    char  c, *p, *start;

    p = nxt_malloc(value->length + 1);
    if (nxt_slow_path(p == NULL)) {
        return;
    }

    /*
     * PHP frees this memory on module shutdown.
     * See core_globals_dtor() for details.
     */
    *ptr = p;

    nxt_memcpy(p, value->start, value->length);
    p[value->length] = '\0';

    start = p;

    do {
        c = *p;

        if (c == ' ' || c == ',' || c == '\0') {

            if (p != start) {
                *p = '\0';

#ifdef NXT_PHP7
                if (disable(start, p - start)
#else
                if (disable(start, p - start TSRMLS_CC)
#endif
                    != SUCCESS)
                {
                    nxt_log(task, NXT_LOG_ERR,
                            "PHP: failed to disable \"%s\": no such %s",
                            start, type);
                }
            }

            start = p + 1;
        }

        p++;

    } while (c != '\0');
}
#endif


static nxt_int_t
nxt_php_dirname(const nxt_str_t *file, nxt_str_t *dir)
{
    size_t  length;

    if (file->length == 0 || file->start[0] != '/') {
        nxt_unit_alert(NULL, "php_dirname: invalid file name "
                       "(not starts from '/')");
        return NXT_ERROR;
    }

    length = file->length;

    while (file->start[length - 1] != '/') {
        length--;
    }

    dir->length = length;
    dir->start = nxt_malloc(length + 1);
    if (nxt_slow_path(dir->start == NULL)) {
        return NXT_ERROR;
    }

    nxt_memcpy(dir->start, file->start, length);

    dir->start[length] = '\0';

    return NXT_OK;
}


static void
nxt_php_str_trim_trail(nxt_str_t *str, u_char t)
{
    while (str->length > 0 && str->start[str->length - 1] == t) {
        str->length--;
    }

    str->start[str->length] = '\0';
}


static void
nxt_php_str_trim_lead(nxt_str_t *str, u_char t)
{
    while (str->length > 0 && str->start[0] == t) {
        str->length--;
        str->start++;
    }
}


nxt_inline u_char *
nxt_realpath(const void *c)
{
    return (u_char *) realpath(c, NULL);
}


static nxt_int_t
nxt_php_do_301(nxt_unit_request_info_t *req)
{
    char                *p, *url, *port;
    uint32_t            size;
    const char          *proto;
    nxt_unit_request_t  *r;

    r = req->request;

    url = nxt_malloc(sizeof("https://") - 1
                     + r->server_name_length
                     + r->local_port_length + 1
                     + r->path_length + 1
                     + r->query_length + 1
                     + 1);
    if (nxt_slow_path(url == NULL)) {
        return NXT_UNIT_ERROR;
    }

    proto = r->tls ? "https://" : "http://";
    p = nxt_cpymem(url, proto, strlen(proto));
    p = nxt_cpymem(p, nxt_unit_sptr_get(&r->server_name),
                   r->server_name_length);

    port = nxt_unit_sptr_get(&r->local_port);
    if (r->local_port_length > 0
        && !(r->tls && strcmp(port, "443") == 0)
        && !(!r->tls && strcmp(port, "80") == 0))
    {
        *p++ = ':';
        p = nxt_cpymem(p, port, r->local_port_length);
    }

    p = nxt_cpymem(p, nxt_unit_sptr_get(&r->path), r->path_length);
    *p++ = '/';

    if (r->query_length > 0) {
        *p++ = '?';
        p = nxt_cpymem(p, nxt_unit_sptr_get(&r->query), r->query_length);
    }

    *p = '\0';

    size = p - url;

    nxt_unit_response_init(req, NXT_HTTP_MOVED_PERMANENTLY, 1,
                           nxt_length("Location") + size);
    nxt_unit_response_add_field(req, "Location", nxt_length("Location"),
                                url, size);

    nxt_free(url);

    return NXT_UNIT_OK;
}


static nxt_int_t
nxt_php_handle_fs_err(nxt_unit_request_info_t *req)
{
    switch (nxt_errno) {
    case ELOOP:
    case EACCES:
    case ENFILE:
        return nxt_unit_response_init(req, NXT_HTTP_FORBIDDEN, 0, 0);
    case ENOENT:
    case ENOTDIR:
    case ENAMETOOLONG:
        return nxt_unit_response_init(req, NXT_HTTP_NOT_FOUND, 0, 0);
    }

    return NXT_UNIT_ERROR;
}


static void
nxt_php_request_handler(nxt_unit_request_info_t *req)
{
    nxt_php_target_t    *target;
    nxt_php_run_ctx_t   ctx;
    nxt_unit_request_t  *r;

    r = req->request;
    target = &nxt_php_targets[r->app_target];

    nxt_memzero(&ctx, sizeof(ctx));

    ctx.req = req;
    ctx.root = &target->root;
    ctx.index = &target->index;

    if (target->script_filename.length == 0) {
        nxt_php_dynamic_request(&ctx, r);
        return;
    }

    ctx.script_filename = target->script_filename;
    ctx.script_dirname = target->script_dirname;
    ctx.script_name = target->script_name;

    ctx.chdir = (r->app_target != nxt_php_last_target);

    nxt_php_execute(&ctx, r);

    nxt_php_last_target = ctx.chdir ? -1 : r->app_target;
}


static void
nxt_php_dynamic_request(nxt_php_run_ctx_t *ctx, nxt_unit_request_t *r)
{
    u_char     *p;
    nxt_str_t  path, script_name;
    nxt_int_t  ret;

    path.length = r->path_length;
    path.start = nxt_unit_sptr_get(&r->path);

    nxt_str_null(&script_name);

    ctx->path_info.start = memmem(path.start, path.length, ".php/",
                                  strlen(".php/"));
    if (ctx->path_info.start != NULL) {
        ctx->path_info.start += 4;
        path.length = ctx->path_info.start - path.start;

        ctx->path_info.length = r->path_length - path.length;
        /*
         * ctx->path_info points into the shmem-mapped request buffer
         * and is not NUL-terminated.  All consumers below use the
         * length field; do not pass path_info.start to C-string APIs.
         */

    } else if (path.start[path.length - 1] == '/') {
        script_name = *ctx->index;

    } else if (path.length < 4
               || memcmp(path.start + (path.length - 4), ".php", 4) != 0)
    {
        char         tpath[PATH_MAX];
        nxt_int_t    ec;
        struct stat  sb;

        ec = NXT_UNIT_ERROR;

        if (ctx->root->length + path.length + 1 > PATH_MAX) {
            nxt_unit_request_done(ctx->req, ec);

            return;
        }

        p = nxt_cpymem(tpath, ctx->root->start, ctx->root->length);
        p = nxt_cpymem(p, path.start, path.length);
        *p = '\0';

        ret = stat(tpath, &sb);
        if (ret == 0 && S_ISDIR(sb.st_mode)) {
            ec = nxt_php_do_301(ctx->req);
        } else if (ret == -1) {
            ec = nxt_php_handle_fs_err(ctx->req);
        }

        nxt_unit_request_done(ctx->req, ec);

        return;
    }

    ctx->script_filename.length = ctx->root->length
                                  + path.length
                                  + script_name.length;

    p = nxt_malloc(ctx->script_filename.length + 1);
    if (nxt_slow_path(p == NULL)) {
        nxt_unit_request_done(ctx->req, NXT_UNIT_ERROR);

        return;
    }

    ctx->script_filename.start = p;

    ctx->script_name.length = path.length + script_name.length;
    ctx->script_name.start = p + ctx->root->length;

    p = nxt_cpymem(p, ctx->root->start, ctx->root->length);
    p = nxt_cpymem(p, path.start, path.length);

    if (script_name.length > 0) {
        p = nxt_cpymem(p, script_name.start, script_name.length);
    }

    *p = '\0';

    ctx->chdir = 1;

    ret = nxt_php_dirname(&ctx->script_filename, &ctx->script_dirname);
    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_unit_request_done(ctx->req, NXT_UNIT_ERROR);
        nxt_free(ctx->script_filename.start);

        return;
    }

    nxt_php_execute(ctx, r);

    nxt_free(ctx->script_filename.start);
    nxt_free(ctx->script_dirname.start);

    nxt_php_last_target = -1;
}


#if (PHP_VERSION_ID < 70400)
static void
nxt_zend_stream_init_fp(zend_file_handle *handle, FILE *fp,
                        const char *filename)
{
    nxt_memzero(handle, sizeof(zend_file_handle));
    handle->type = ZEND_HANDLE_FP;
    handle->handle.fp = fp;
    handle->filename = filename;
}
#else
#define nxt_zend_stream_init_fp  zend_stream_init_fp
#endif


static void
nxt_php_execute(nxt_php_run_ctx_t *ctx, nxt_unit_request_t *r)
{
    FILE              *fp;
#if (PHP_VERSION_ID < 50600)
    void              *read_post;
#endif
    const char        *filename;
    nxt_unit_field_t  *f;
    zend_file_handle  file_handle;

    filename = (const char *) ctx->script_filename.start;

    nxt_unit_req_debug(ctx->req, "PHP execute script %s", filename);

    fp = fopen(filename, "re");
    if (fp == NULL) {
        nxt_int_t  ec;

        nxt_unit_req_debug(ctx->req, "PHP fopen(\"%s\") failed", filename);

        ec = nxt_php_handle_fs_err(ctx->req);
        nxt_unit_request_done(ctx->req, ec);
        return;
    }

    SG(server_context) = ctx;
    SG(options) |= SAPI_OPTION_NO_CHDIR;
    SG(request_info).request_uri = nxt_unit_sptr_get(&r->target);
    SG(request_info).request_method = nxt_unit_sptr_get(&r->method);

    SG(request_info).proto_num = 1001;

    SG(request_info).query_string = r->query.offset
                                    ? nxt_unit_sptr_get(&r->query) : NULL;
    SG(request_info).content_length = r->content_length;

    if (r->content_type_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->content_type_field;

        SG(request_info).content_type = nxt_unit_sptr_get(&f->value);
    }

    if (r->cookie_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->cookie_field;

        ctx->cookie = nxt_unit_sptr_get(&f->value);
    }

    if (r->authorization_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->authorization_field;

#ifdef NXT_PHP7
        php_handle_auth_data(nxt_unit_sptr_get(&f->value));
#else
        php_handle_auth_data(nxt_unit_sptr_get(&f->value) TSRMLS_CC);
#endif

    } else {
        SG(request_info).auth_digest = NULL;
        SG(request_info).auth_user = NULL;
        SG(request_info).auth_password = NULL;
    }

    SG(sapi_headers).http_response_code = 200;

    SG(request_info).path_translated = NULL;

#ifdef NXT_PHP7
    if (nxt_slow_path(php_request_startup() == FAILURE)) {
#else
    if (nxt_slow_path(php_request_startup(TSRMLS_C) == FAILURE)) {
#endif
        nxt_unit_req_debug(ctx->req, "php_request_startup() failed");

        nxt_unit_request_done(ctx->req, NXT_UNIT_ERROR);
        fclose(fp);

        return;
    }

    if (ctx->chdir) {
        ctx->chdir = 0;
        nxt_php_vcwd_chdir(ctx->req, ctx->script_dirname.start);
    }

    nxt_zend_stream_init_fp(&file_handle, fp, filename);

    php_execute_script(&file_handle TSRMLS_CC);

#if (PHP_VERSION_ID >= 80100)
    zend_destroy_file_handle(&file_handle);
#endif

    /* Prevention of consuming possible unread request body. */
#if (PHP_VERSION_ID < 50600)
    read_post = sapi_module.read_post;
    sapi_module.read_post = NULL;
#else
    SG(post_read) = 1;
#endif

    php_request_shutdown(NULL);

    if (ctx->req != NULL) {
        nxt_unit_request_done(ctx->req, NXT_UNIT_OK);
    }

#if (PHP_VERSION_ID < 50600)
    sapi_module.read_post = read_post;
#endif
}


nxt_inline void
nxt_php_vcwd_chdir(nxt_unit_request_info_t *req, u_char *dir)
{
    if (nxt_slow_path(VCWD_CHDIR((char *) dir) != 0)) {
        nxt_unit_req_alert(req, "VCWD_CHDIR(%s) failed (%d: %s)",
                           dir, errno, strerror(errno));
    }
}


static int
nxt_php_startup(sapi_module_struct *sapi_module)
{
#if (PHP_VERSION_ID < 80200)
    return php_module_startup(sapi_module, &nxt_php_unit_module, 1);
#else
    return php_module_startup(sapi_module, &nxt_php_unit_module);
#endif
}


#ifdef NXT_PHP7
static size_t
nxt_php_unbuffered_write(const char *str, size_t str_length TSRMLS_DC)
#else
static int
nxt_php_unbuffered_write(const char *str, uint str_length TSRMLS_DC)
#endif
{
    int                rc;
    nxt_php_run_ctx_t  *ctx;

    ctx = SG(server_context);

    /* During entrypoint execution there's no request context */
    if (ctx == NULL || ctx->req == NULL) {
        /* Log output from entrypoint to unit log */
        nxt_unit_log(nxt_php_unit_ctx, NXT_UNIT_LOG_INFO,
                     "PHP output: %.*s", (int)str_length, str);
        return str_length;
    }

    rc = nxt_unit_response_write(ctx->req, str, str_length);
    if (nxt_fast_path(rc == NXT_UNIT_OK)) {
        return str_length;
    }

    php_handle_aborted_connection();
    return 0;
}


static int
nxt_php_send_headers(sapi_headers_struct *sapi_headers TSRMLS_DC)
{
    int                      rc, fields_count;
    char                     *colon, *value;
    uint16_t                 status;
    uint32_t                 resp_size;
    nxt_php_run_ctx_t        *ctx;
    sapi_header_struct       *h;
    zend_llist_position      zpos;
    nxt_unit_request_info_t  *req;

    ctx = SG(server_context);

    /* During entrypoint execution there's no request context */
    if (ctx == NULL || ctx->req == NULL) {
        return SAPI_HEADER_SENT_SUCCESSFULLY;
    }

    req = ctx->req;

    nxt_unit_req_debug(req, "nxt_php_send_headers");

    if (SG(request_info).no_headers == 1) {
        rc = nxt_unit_response_init(req, 200, 0, 0);
        if (nxt_slow_path(rc != NXT_UNIT_OK)) {
            return SAPI_HEADER_SEND_FAILED;
        }

        return SAPI_HEADER_SENT_SUCCESSFULLY;
    }

    resp_size = 0;
    fields_count = zend_llist_count(&sapi_headers->headers);

    for (h = zend_llist_get_first_ex(&sapi_headers->headers, &zpos);
         h;
         h = zend_llist_get_next_ex(&sapi_headers->headers, &zpos))
    {
        resp_size += h->header_len;
    }

    status = SG(sapi_headers).http_response_code;

    rc = nxt_unit_response_init(req, status, fields_count, resp_size);
    if (nxt_slow_path(rc != NXT_UNIT_OK)) {
        return SAPI_HEADER_SEND_FAILED;
    }

    for (h = zend_llist_get_first_ex(&sapi_headers->headers, &zpos);
         h;
         h = zend_llist_get_next_ex(&sapi_headers->headers, &zpos))
    {
        colon = memchr(h->header, ':', h->header_len);
        if (nxt_slow_path(colon == NULL)) {
            nxt_unit_req_warn(req, "colon not found in header '%.*s'",
                              (int) h->header_len, h->header);
            continue;
        }

        value = colon + 1;
        while (value < h->header + h->header_len
               && isspace((unsigned char) *value))
        {
            value++;
        }

        nxt_unit_response_add_field(req, h->header, colon - h->header,
                                    value,
                                    h->header_len - (value - h->header));
    }

    rc = nxt_unit_response_send(req);
    if (nxt_slow_path(rc != NXT_UNIT_OK)) {
        nxt_unit_req_debug(req, "failed to send response");

        return SAPI_HEADER_SEND_FAILED;
    }

    return SAPI_HEADER_SENT_SUCCESSFULLY;
}


#ifdef NXT_PHP7
static size_t
nxt_php_read_post(char *buffer, size_t count_bytes TSRMLS_DC)
#else
static int
nxt_php_read_post(char *buffer, uint count_bytes TSRMLS_DC)
#endif
{
    nxt_php_run_ctx_t  *ctx;

    ctx = SG(server_context);

    /* During entrypoint execution there's no request context */
    if (ctx == NULL || ctx->req == NULL) {
        return 0;
    }

    nxt_unit_req_debug(ctx->req, "nxt_php_read_post %d", (int) count_bytes);

    return nxt_unit_request_read(ctx->req, buffer, count_bytes);
}


static char *
nxt_php_read_cookies(TSRMLS_D)
{
    nxt_php_run_ctx_t  *ctx;

    ctx = SG(server_context);

    /* During entrypoint execution there's no request context */
    if (ctx == NULL) {
        return NULL;
    }

    nxt_unit_req_debug(ctx->req, "nxt_php_read_cookies");

    return ctx->cookie;
}


void
nxt_php_register_variables(zval *track_vars_array TSRMLS_DC)
{
    const char               *name;
    nxt_unit_field_t         *f, *f_end;
    nxt_php_run_ctx_t        *ctx;
    nxt_unit_request_t       *r;
    nxt_unit_request_info_t  *req;

    ctx = SG(server_context);

    /* During entrypoint execution there's no request context */
    if (ctx == NULL || ctx->req == NULL) {
        return;
    }

    req = ctx->req;
    r = req->request;

    nxt_unit_req_debug(req, "nxt_php_register_variables");

    php_register_variable_safe((char *) "SERVER_SOFTWARE",
                               (char *) nxt_server.start,
                               nxt_server.length, track_vars_array TSRMLS_CC);

    nxt_php_set_sptr(req, "SERVER_PROTOCOL", &r->version, r->version_length,
                     track_vars_array TSRMLS_CC);

    /*
     * 'PHP_SELF'
     * The filename of the currently executing script, relative to the document
     * root.  For instance, $_SERVER['PHP_SELF'] in a script at the address
     * http://example.com/foo/bar.php would be /foo/bar.php.  The __FILE__
     * constant contains the full path and filename of the current (i.e.
     * included) file.  If PHP is running as a command-line processor this
     * variable contains the script name since PHP 4.3.0. Previously it was not
     * available.
     */

    if (ctx->path_info.length != 0) {
        nxt_php_set_sptr(req, "PHP_SELF", &r->path, r->path_length,
                         track_vars_array TSRMLS_CC);

        nxt_php_set_str(req, "PATH_INFO", &ctx->path_info,
                        track_vars_array TSRMLS_CC);

    } else {
        nxt_php_set_str(req, "PHP_SELF", &ctx->script_name,
                        track_vars_array TSRMLS_CC);
    }

    /*
     * 'SCRIPT_NAME'
     * Contains the current script's path.  This is useful for pages which need
     * to point to themselves.  The __FILE__ constant contains the full path and
     * filename of the current (i.e. included) file.
     */

    nxt_php_set_str(req, "SCRIPT_NAME", &ctx->script_name,
                    track_vars_array TSRMLS_CC);

    /*
     * 'SCRIPT_FILENAME'
     * The absolute pathname of the currently executing script.
     */

    nxt_php_set_str(req, "SCRIPT_FILENAME", &ctx->script_filename,
                    track_vars_array TSRMLS_CC);

    /*
     * 'DOCUMENT_ROOT'
     * The document root directory under which the current script is executing,
     * as defined in the server's configuration file.
     */

    nxt_php_set_str(req, "DOCUMENT_ROOT", ctx->root,
                    track_vars_array TSRMLS_CC);

    nxt_php_set_sptr(req, "REQUEST_METHOD", &r->method, r->method_length,
                     track_vars_array TSRMLS_CC);
    nxt_php_set_sptr(req, "REQUEST_URI", &r->target, r->target_length,
                     track_vars_array TSRMLS_CC);
    nxt_php_set_sptr(req, "QUERY_STRING", &r->query, r->query_length,
                     track_vars_array TSRMLS_CC);

    nxt_php_set_sptr(req, "REMOTE_ADDR", &r->remote, r->remote_length,
                     track_vars_array TSRMLS_CC);
    nxt_php_set_sptr(req, "SERVER_ADDR", &r->local_addr, r->local_addr_length,
                     track_vars_array TSRMLS_CC);

    nxt_php_set_sptr(req, "SERVER_NAME", &r->server_name, r->server_name_length,
                     track_vars_array TSRMLS_CC);
    nxt_php_set_sptr(req, "SERVER_PORT", &r->local_port, r->local_port_length,
                     track_vars_array TSRMLS_CC);

    if (r->tls) {
        nxt_php_set_cstr(req, "HTTPS", "on", 2, track_vars_array TSRMLS_CC);
    }

    f_end = r->fields + r->fields_count;
    for (f = r->fields; f < f_end; f++) {
        name = nxt_unit_sptr_get(&f->name);

        nxt_php_set_sptr(req, name, &f->value, f->value_length,
                         track_vars_array TSRMLS_CC);
    }

    if (r->content_length_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->content_length_field;

        nxt_php_set_sptr(req, "CONTENT_LENGTH", &f->value, f->value_length,
                         track_vars_array TSRMLS_CC);
    }

    if (r->content_type_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->content_type_field;

        nxt_php_set_sptr(req, "CONTENT_TYPE", &f->value, f->value_length,
                         track_vars_array TSRMLS_CC);
    }
}


#if NXT_PHP_TRUEASYNC

static void
nxt_php_register_variables_async(nxt_unit_request_info_t *req,
    nxt_php_run_ctx_t *ctx, zval *track_vars_array TSRMLS_DC)
{
    const char               *name;
    char                     *str;
    nxt_unit_field_t         *f, *f_end;
    nxt_unit_request_t       *r;

    r = req->request;

    nxt_unit_req_debug(req, "nxt_php_register_variables_async");

    /* Register SERVER_SOFTWARE */
    php_register_variable_safe((char *) "SERVER_SOFTWARE",
                               (char *) nxt_server.start,
                               nxt_server.length, track_vars_array TSRMLS_CC);

    /* Register SERVER_PROTOCOL */
    str = nxt_unit_sptr_get(&r->version);
    php_register_variable_safe((char *) "SERVER_PROTOCOL", str,
                               r->version_length, track_vars_array TSRMLS_CC);

    /* Register PHP_SELF and PATH_INFO */
    if (ctx->path_info.length != 0) {
        str = nxt_unit_sptr_get(&r->path);
        php_register_variable_safe((char *) "PHP_SELF", str,
                                   r->path_length, track_vars_array TSRMLS_CC);
        php_register_variable_safe((char *) "PATH_INFO",
                                   (char *) ctx->path_info.start,
                                   ctx->path_info.length, track_vars_array TSRMLS_CC);
    } else {
        php_register_variable_safe((char *) "PHP_SELF",
                                   (char *) ctx->script_name.start,
                                   ctx->script_name.length, track_vars_array TSRMLS_CC);
    }

    /* Register SCRIPT_NAME */
    php_register_variable_safe((char *) "SCRIPT_NAME",
                               (char *) ctx->script_name.start,
                               ctx->script_name.length, track_vars_array TSRMLS_CC);

    /* Register SCRIPT_FILENAME */
    php_register_variable_safe((char *) "SCRIPT_FILENAME",
                               (char *) ctx->script_filename.start,
                               ctx->script_filename.length, track_vars_array TSRMLS_CC);

    /* Register DOCUMENT_ROOT */
    php_register_variable_safe((char *) "DOCUMENT_ROOT",
                               (char *) ctx->root->start,
                               ctx->root->length, track_vars_array TSRMLS_CC);

    /* Register REQUEST_METHOD */
    str = nxt_unit_sptr_get(&r->method);
    php_register_variable_safe((char *) "REQUEST_METHOD", str,
                               r->method_length, track_vars_array TSRMLS_CC);

    /* Register REQUEST_URI */
    str = nxt_unit_sptr_get(&r->target);
    php_register_variable_safe((char *) "REQUEST_URI", str,
                               r->target_length, track_vars_array TSRMLS_CC);

    /* Register QUERY_STRING */
    str = nxt_unit_sptr_get(&r->query);
    php_register_variable_safe((char *) "QUERY_STRING", str,
                               r->query_length, track_vars_array TSRMLS_CC);

    /* Register REMOTE_ADDR */
    str = nxt_unit_sptr_get(&r->remote);
    php_register_variable_safe((char *) "REMOTE_ADDR", str,
                               r->remote_length, track_vars_array TSRMLS_CC);

    /* Register SERVER_ADDR */
    str = nxt_unit_sptr_get(&r->local_addr);
    php_register_variable_safe((char *) "SERVER_ADDR", str,
                               r->local_addr_length, track_vars_array TSRMLS_CC);

    /* Register SERVER_NAME */
    str = nxt_unit_sptr_get(&r->server_name);
    php_register_variable_safe((char *) "SERVER_NAME", str,
                               r->server_name_length, track_vars_array TSRMLS_CC);

    /* Register SERVER_PORT */
    str = nxt_unit_sptr_get(&r->local_port);
    php_register_variable_safe((char *) "SERVER_PORT", str,
                               r->local_port_length, track_vars_array TSRMLS_CC);

    /* Register HTTPS if TLS is enabled */
    if (r->tls) {
        php_register_variable_safe((char *) "HTTPS", (char *) "on",
                                   2, track_vars_array TSRMLS_CC);
    }

    /* Register HTTP headers */
    f_end = r->fields + r->fields_count;
    for (f = r->fields; f < f_end; f++) {
        name = nxt_unit_sptr_get(&f->name);
        str = nxt_unit_sptr_get(&f->value);
        php_register_variable_safe((char *) name, str,
                                   f->value_length, track_vars_array TSRMLS_CC);
    }

    /* Register CONTENT_LENGTH */
    if (r->content_length_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->content_length_field;
        str = nxt_unit_sptr_get(&f->value);
        php_register_variable_safe((char *) "CONTENT_LENGTH", str,
                                   f->value_length, track_vars_array TSRMLS_CC);
    }

    /* Register CONTENT_TYPE */
    if (r->content_type_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->content_type_field;
        str = nxt_unit_sptr_get(&f->value);
        php_register_variable_safe((char *) "CONTENT_TYPE", str,
                                   f->value_length, track_vars_array TSRMLS_CC);
    }
}

#endif /* NXT_PHP_TRUEASYNC */


static void
nxt_php_set_sptr(nxt_unit_request_info_t *req, const char *name,
    nxt_unit_sptr_t *v, uint32_t len, zval *track_vars_array TSRMLS_DC)
{
    char          *str;
#if NXT_PHP7
    size_t        new_len;
#else
    unsigned int  new_len;
#endif

    str = nxt_unit_sptr_get(v);

    nxt_unit_req_debug(req, "php: register %s='%.*s'", name, (int) len, str);

    if (sapi_module.input_filter(PARSE_SERVER, (char *) name, &str, len,
                                 &new_len TSRMLS_CC))
    {
        php_register_variable_safe((char *) name, str, new_len,
                                   track_vars_array TSRMLS_CC);
    }
}


nxt_inline void
nxt_php_set_str(nxt_unit_request_info_t *req, const char *name,
    nxt_str_t *s, zval *track_vars_array TSRMLS_DC)
{
    nxt_php_set_cstr(req, name, (char *) s->start, s->length,
                     track_vars_array TSRMLS_CC);
}


#ifdef NXT_PHP7

static void *
nxt_php_hash_str_find_ptr(const HashTable *ht, const nxt_str_t *str)
{
    return zend_hash_str_find_ptr(ht, (const char *) str->start, str->length);
}

#else

static void *
nxt_php_hash_str_find_ptr(const HashTable *ht, const nxt_str_t *str)
{
    int   ret;
    void  *entry;
    char  buf[256];

    if (nxt_slow_path(str->length >= (sizeof(buf) - 1))) {
        return NULL;
    }

    nxt_memcpy(buf, str->start, str->length);
    buf[str->length] = '\0';

    ret = zend_hash_find(ht, buf, str->length + 1, &entry);
    if (nxt_fast_path(ret == SUCCESS)) {
        return entry;
    }

    return NULL;
}

#endif


static void
nxt_php_set_cstr(nxt_unit_request_info_t *req, const char *name,
    const char *cstr, uint32_t len, zval *track_vars_array TSRMLS_DC)
{
    if (nxt_slow_path(cstr == NULL)) {
        return;
    }

    nxt_unit_req_debug(req, "php: register %s='%.*s'", name, (int) len, cstr);

    php_register_variable_safe((char *) name, (char *) cstr, len,
                               track_vars_array TSRMLS_CC);
}


#if NXT_PHP8
static void
nxt_php_log_message(const char *message, int syslog_type_int)
#else
#ifdef NXT_HAVE_PHP_LOG_MESSAGE_WITH_SYSLOG_TYPE
static void
nxt_php_log_message(char *message, int syslog_type_int)
#else
static void
nxt_php_log_message(char *message TSRMLS_DC)
#endif
#endif
{
    nxt_php_run_ctx_t  *ctx;

    ctx = SG(server_context);

    if (ctx != NULL && ctx->req != NULL) {
        nxt_unit_req_log(ctx->req, NXT_UNIT_LOG_NOTICE,
                         "php message: %s", message);

    } else {
        nxt_unit_log(nxt_php_unit_ctx, NXT_UNIT_LOG_NOTICE,
                     "php message: %s", message);
    }
}


#if NXT_PHP_TRUEASYNC

/* TrueAsync Mode Implementation */

/* External globals */
extern zval  *nxt_php_request_callback;

/**
 * The request handler invoked from NGINX UNIT.
 * The handler uses a PHP callback function (nxt_php_request_callback) to process the request.
 *
 **/
static void
nxt_php_request_handler_async(nxt_unit_request_info_t *req)
{
    zend_async_scope_t    *request_scope;
    zend_coroutine_t      *coroutine;
    nxt_php_run_ctx_t     run_ctx;
    nxt_php_target_t      *target;
    nxt_unit_request_t    *r;
    nxt_unit_field_t      *f;
    void                  *saved_server_context;

    r = req->request;
    target = &nxt_php_targets[r->app_target];

    /* 1. Create new Scope for this request with its own superglobals */
    request_scope = ZEND_ASYNC_NEW_SCOPE(ZEND_ASYNC_CURRENT_SCOPE);

    if (request_scope == NULL) {
        nxt_unit_req_alert(req, "Failed to create request scope");
        nxt_unit_request_done(req, NXT_UNIT_ERROR);
        return;
    }

    /* 2. DISABLE inheriting global superglobals - each request has its own */
    ZEND_ASYNC_SCOPE_CLR_INHERIT_SUPERGLOBALS(request_scope);

    /* 3. Setup SG(server_context) and SG(request_info) for superglobals population */
    saved_server_context = SG(server_context);

    /* Prepare run context */
    nxt_memzero(&run_ctx, sizeof(run_ctx));
    run_ctx.req = req;
    run_ctx.root = &target->root;
    run_ctx.index = &target->index;
    run_ctx.script_filename = target->script_filename;
    run_ctx.script_dirname = target->script_dirname;
    run_ctx.script_name = target->script_name;

    /* Extract cookie if present */
    if (r->cookie_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->cookie_field;
        run_ctx.cookie = nxt_unit_sptr_get(&f->value);
    }

    /* Setup SG(server_context) */
    SG(server_context) = &run_ctx;

    /* Setup SG(request_info) for php_default_treat_data */
    SG(request_info).request_method = nxt_unit_sptr_get(&r->method);
    SG(request_info).query_string = r->query.offset ? nxt_unit_sptr_get(&r->query) : NULL;
    SG(request_info).content_length = r->content_length;

    if (r->content_type_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->content_type_field;
        SG(request_info).content_type = nxt_unit_sptr_get(&f->value);
    } else {
        SG(request_info).content_type = NULL;
    }

    /* 4. Populate superglobals with data from HTTP request */
    nxt_php_scope_populate_superglobals(request_scope);

    /* 5. Restore original SG(server_context) */
    SG(server_context) = saved_server_context;

    /* 6. Create coroutine within this Scope */
    coroutine = ZEND_ASYNC_NEW_COROUTINE(request_scope);
    if (coroutine == NULL) {
        nxt_unit_req_alert(req, "Failed to create coroutine");
        /* Scope will be automatically cleaned up */
        nxt_unit_request_done(req, NXT_UNIT_ERROR);
        return;
    }

    /* 5. Setup coroutine entry point and data */
    coroutine->internal_entry = nxt_php_request_coroutine_entry;
    coroutine->extended_data = req;

    /* 6. Enqueue coroutine for execution */
    if (!ZEND_ASYNC_ENQUEUE_COROUTINE(coroutine)) {
        nxt_unit_req_alert(req, "Failed to enqueue coroutine");
        nxt_unit_request_done(req, NXT_UNIT_ERROR);
        return;
    }
}

/* Load and execute entrypoint script in async mode */
static nxt_int_t
nxt_php_async_load_entrypoint(nxt_task_t *task, nxt_str_t *entrypoint)
{
    FILE              *fp;
    const char        *filename;
    zend_file_handle  file_handle;

    nxt_log(task, NXT_LOG_INFO, "TrueAsync: nxt_php_async_load_entrypoint called, PID=%d", getpid());


    /* Create null-terminated filename */
    char *filename_buf = nxt_malloc(entrypoint->length + 1);
    if (filename_buf == NULL) {
        nxt_alert(task, "TrueAsync: Failed to allocate filename buffer");
        return NXT_ERROR;
    }

    nxt_memcpy(filename_buf, entrypoint->start, entrypoint->length);
    filename_buf[entrypoint->length] = '\0';
    filename = filename_buf;


    fp = fopen(filename, "re");
    if (fp == NULL) {
        nxt_alert(task, "TrueAsync: Failed to open entrypoint %s", filename);
        nxt_free(filename_buf);
        return NXT_ERROR;
    }

    /* Initialize minimal SG() fields like CLI does */
    SG(request_info).argc = 0;
    SG(request_info).argv = NULL;
    SG(request_info).path_translated = (char*)filename;
    SG(server_context) = NULL;

    /* Call php_request_startup() like CLI does */
#ifdef NXT_PHP7
    if (nxt_slow_path(php_request_startup() == FAILURE)) {
#else
    if (nxt_slow_path(php_request_startup(TSRMLS_C) == FAILURE)) {
#endif
        nxt_alert(task, "TrueAsync: php_request_startup() failed");
        fclose(fp);
        nxt_free(filename_buf);
        return NXT_ERROR;
    }

    nxt_zend_stream_init_fp(&file_handle, fp, filename);

    /* Execute entrypoint script */
    int result = php_execute_script(&file_handle TSRMLS_CC);

    if (result == FAILURE) {
        nxt_alert(task, "TrueAsync: php_execute_script() FAILED!");
        fclose(fp);
        nxt_free(filename_buf);
        return NXT_ERROR;
    }

#if (PHP_VERSION_ID >= 80100)
    zend_destroy_file_handle(&file_handle);
#endif

    nxt_free(filename_buf);

    /* Check if callback was registered */
    if (nxt_php_request_callback == NULL) {
        nxt_alert(task, "TrueAsync: No request handler registered! Call HttpServer->onRequest() in your entrypoint.");
        return NXT_ERROR;
    }

    /* Check callback is callable */
    if (nxt_php_request_callback != NULL && Z_TYPE_P(nxt_php_request_callback) == IS_OBJECT) {
        nxt_log(task, NXT_LOG_INFO, "TrueAsync: Callback is object, type OK");
    } else {
        nxt_alert(task, "TrueAsync: Callback has wrong type: %d",
                  nxt_php_request_callback ? Z_TYPE_P(nxt_php_request_callback) : -1);
    }

    /*
     * DON'T call php_request_shutdown() — we want the callback zval to
     * persist across the prototype → worker fork.  But scrub any
     * pending exception left in EG: if the entrypoint script raised
     * something that wasn't caught before HttpServer->onRequest()
     * stored the callback, every forked worker would inherit the
     * exception on its first request.  The callback zval itself
     * (nxt_php_request_callback) is not on the exception path; it
     * was registered explicitly by the userland code.
     *
     * Other EG globals (output buffers, error_reporting, the symbol
     * table) are also inherited, but those are reset implicitly when
     * the worker enters a fresh request_init.  Exception state is
     * the one that bites hardest because the next request's
     * php_execute_script() can early-exit on a stale EG(exception).
     */
    if (EG(exception) != NULL) {
        zend_clear_exception();
    }

    return NXT_OK;
}


/* Quit handler for graceful shutdown */
static void
nxt_php_quit_handler(nxt_unit_ctx_t *ctx)
{
    nxt_unit_debug(ctx, "TrueAsync: quit handler called, triggering graceful shutdown");

    /* Call ZEND_ASYNC_SHUTDOWN to trigger graceful shutdown of all coroutines */
    ZEND_ASYNC_SHUTDOWN();
}


/* Server wait event methods */
static bool
nxt_php_server_wait_event_start(zend_async_event_t *event)
{
    /* No action needed - event waits indefinitely */
    return true;
}

static bool
nxt_php_server_wait_event_stop(zend_async_event_t *event)
{
    /* No action needed */
    return true;
}

static bool
nxt_php_server_wait_event_add_callback(zend_async_event_t *event, zend_async_event_callback_t *callback)
{
    return zend_async_callbacks_push(event, callback);
}

static bool
nxt_php_server_wait_event_del_callback(zend_async_event_t *event, zend_async_event_callback_t *callback)
{
    return zend_async_callbacks_remove(event, callback);
}

static bool
nxt_php_server_wait_event_replay(zend_async_event_t *event, zend_async_event_callback_t *callback, zval *result, zend_object **exception)
{
    /* Event never resolves - cannot replay */
    return false;
}

static zend_string *
nxt_php_server_wait_event_info(zend_async_event_t *event)
{
    return zend_string_init("NGINX Unit server waiting", sizeof("NGINX Unit server waiting") - 1, 0);
}

static bool
nxt_php_server_wait_event_dispose(zend_async_event_t *event)
{
    if (ZEND_ASYNC_EVENT_REFCOUNT(event) > 1) {
        ZEND_ASYNC_EVENT_DEL_REF(event);
        return true;
    }

    if (ZEND_ASYNC_EVENT_REFCOUNT(event) == 1) {
        ZEND_ASYNC_EVENT_DEL_REF(event);
    }

    /* Notify all callbacks that event is disposed */
    ZEND_ASYNC_CALLBACKS_NOTIFY(event, NULL, NULL);

    /* Free the event */
    efree(event);

    return true;
}

static bool
nxt_php_activate_true_async(nxt_task_t *task)
{
    if (ZEND_ASYNC_IS_OFF) {
        nxt_alert(task, "TrueAsync: async mode is off");
        return false;
    }

    if (ZEND_ASYNC_IS_READY) {
        if (!ZEND_ASYNC_SCHEDULER_LAUNCH()) {
            nxt_alert(task, "TrueAsync: Failed to launch scheduler");
            return false;
        }
    }

    return ZEND_ASYNC_IS_ACTIVE;
}

/**
 * Suspend coroutine until application should finish
 * Event handlers registered via add_port/remove_port will be triggered by scheduler
 */
static void
nxt_php_suspend_coroutine(nxt_unit_ctx_t *ctx)
{
    zend_coroutine_t *coroutine = ZEND_ASYNC_CURRENT_COROUTINE;
    if (coroutine == NULL) {
        nxt_unit_alert(ctx, "TrueAsync: Failed to get current coroutine");
        return;
    }

    /* Create server wait event (it's custom nginx unit trigger for TrueAsync) */
    zend_async_event_t *event = emalloc(sizeof(zend_async_event_t));
    if (event == NULL) {
        nxt_unit_alert(ctx, "TrueAsync: Failed to allocate server wait event");
        return;
    }

    /* Initialize event structure */
    memset(event, 0, sizeof(zend_async_event_t));

    /* Set event methods */
    event->start = nxt_php_server_wait_event_start;
    event->stop = nxt_php_server_wait_event_stop;
    event->add_callback = nxt_php_server_wait_event_add_callback;
    event->del_callback = nxt_php_server_wait_event_del_callback;
    event->replay = nxt_php_server_wait_event_replay;
    event->info = nxt_php_server_wait_event_info;
    event->dispose = nxt_php_server_wait_event_dispose;

    /* Create waker for coroutine */
    if (UNEXPECTED(zend_async_waker_new(coroutine) == NULL)) {
        nxt_unit_alert(ctx, "TrueAsync: Failed to create waker");
        event->dispose(event);
        return;
    }

    /* Attach coroutine to wait event - it will suspend until GRACEFUL_SHUTDOWN */
    zend_async_resume_when(coroutine, event, true, zend_async_waker_callback_resolve, NULL);

    if (UNEXPECTED(EG(exception) != NULL)) {
        nxt_unit_alert(ctx, "TrueAsync: Failed to attach coroutine to wait event");
        zend_async_waker_clean(coroutine);
        event->dispose(event);
        return;
    }

    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(coroutine);
}


static void
nxt_php_scope_init_superglobals(zend_async_scope_t *scope)
{
    zval tmp;

    if (scope->superglobals != NULL) {
        return;
    }

    ALLOC_HASHTABLE(scope->superglobals);
    zend_hash_init(scope->superglobals, 8, NULL, ZVAL_PTR_DTOR, 0);

    array_init(&tmp);
    zend_hash_str_add_new(scope->superglobals, "_GET", sizeof("_GET") - 1, &tmp);

    array_init(&tmp);
    zend_hash_str_add_new(scope->superglobals, "_POST", sizeof("_POST") - 1, &tmp);

    array_init(&tmp);
    zend_hash_str_add_new(scope->superglobals, "_COOKIE", sizeof("_COOKIE") - 1, &tmp);

    array_init(&tmp);
    zend_hash_str_add_new(scope->superglobals, "_SERVER", sizeof("_SERVER") - 1, &tmp);

    array_init(&tmp);
    zend_hash_str_add_new(scope->superglobals, "_ENV", sizeof("_ENV") - 1, &tmp);

    array_init(&tmp);
    zend_hash_str_add_new(scope->superglobals, "_FILES", sizeof("_FILES") - 1, &tmp);

    array_init(&tmp);
    zend_hash_str_add_new(scope->superglobals, "_REQUEST", sizeof("_REQUEST") - 1, &tmp);
}


static void
nxt_php_scope_populate_superglobals(zend_async_scope_t *scope)
{
    zval               *server_array, *get_array, *post_array, *cookie_array;
    nxt_php_run_ctx_t  *ctx;
    nxt_unit_request_t *r;
    nxt_unit_field_t   *f;

    if (scope->superglobals == NULL) {
        nxt_php_scope_init_superglobals(scope);
    }

    ctx = SG(server_context);
    if (ctx == NULL || ctx->req == NULL) {
        return;
    }

    r = ctx->req->request;

    server_array = zend_hash_str_find(scope->superglobals, "_SERVER", sizeof("_SERVER") - 1);
    get_array    = zend_hash_str_find(scope->superglobals, "_GET", sizeof("_GET") - 1);
    post_array   = zend_hash_str_find(scope->superglobals, "_POST", sizeof("_POST") - 1);
    cookie_array = zend_hash_str_find(scope->superglobals, "_COOKIE", sizeof("_COOKIE") - 1);

    /* Populate $_SERVER */
    if (server_array != NULL) {
        nxt_php_register_variables_async(ctx->req, ctx, server_array);
    }

    /* Populate $_GET - use PARSE_STRING to write into our array */
    if (get_array != NULL && r->query_length > 0) {
        char *query = estrndup((char *)nxt_unit_sptr_get(&r->query), r->query_length);
        php_default_treat_data(PARSE_STRING, query, get_array);
    }

    /* Populate $_COOKIE - use PARSE_STRING to write into our array */
    if (cookie_array != NULL && ctx->cookie != NULL) {
        char *cookie = estrdup(ctx->cookie);
        php_default_treat_data(PARSE_STRING, cookie, cookie_array);
    }

    /* Populate $_POST - use PARSE_STRING for application/x-www-form-urlencoded */
    if (post_array != NULL &&
        SG(request_info).request_method &&
        !strcasecmp(SG(request_info).request_method, "POST"))
    {
        /* Check if content type is application/x-www-form-urlencoded */
        if (r->content_type_field != NXT_UNIT_NONE_FIELD) {
            f = r->fields + r->content_type_field;
            const char *content_type = nxt_unit_sptr_get(&f->value);

            /* Only parse if it's form-urlencoded (simple POST data) */
            if (content_type &&
                strncasecmp(content_type, "application/x-www-form-urlencoded", 33) == 0)
            {
                /* Read POST body into memory */
                size_t post_len = r->content_length;
                if (post_len > 0) {
                    char *post_data = nxt_malloc(post_len + 1);
                    if (post_data != NULL) {
                        size_t read_bytes = nxt_unit_request_read(ctx->req, post_data, post_len);
                        if (read_bytes > 0) {
                            post_data[read_bytes] = '\0';
                            /* php_default_treat_data will modify and free the string, so use estrndup */
                            char *post_copy = estrndup(post_data, read_bytes);
                            php_default_treat_data(PARSE_STRING, post_copy, post_array);
                        }
                        nxt_free(post_data);
                    }
                }
            }
            /* For multipart/form-data and others - not supported yet in async mode */
        }
    }
}

#endif /* NXT_PHP_TRUEASYNC */
