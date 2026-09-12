
/*
 * Copyright (C) Max Romanov
 * Copyright (C) Valentin V. Bartenev
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_APPLICATION_H_INCLUDED_
#define _NXT_APPLICATION_H_INCLUDED_


#include <nxt_conf.h>

#include <nxt_unit_typedefs.h>


typedef enum {
    NXT_APP_EXTERNAL,
    NXT_APP_PYTHON,
    NXT_APP_PHP,
    NXT_APP_PERL,
    NXT_APP_RUBY,
    NXT_APP_JAVA,
    NXT_APP_WASM,
    NXT_APP_WASM_WC,

    NXT_APP_UNKNOWN,
} nxt_app_type_t;


typedef struct nxt_app_module_s  nxt_app_module_t;
typedef nxt_int_t (*nxt_application_setup_t)(nxt_task_t *task,
    nxt_process_t *process, nxt_common_app_conf_t *conf);


typedef struct {
    nxt_app_type_t            type;
    char                      *name;
    u_char                    *version;
    char                      *file;
    nxt_app_module_t          *module;
    nxt_array_t               *mounts;    /* of nxt_fs_mount_t */
} nxt_app_lang_module_t;


typedef struct {
    char                       *executable;
    nxt_conf_value_t           *arguments;
} nxt_external_app_conf_t;


typedef struct {
    char                       *home;
    nxt_conf_value_t           *path;
    nxt_str_t                  protocol;
    uint32_t                   threads;
    uint32_t                   thread_stack_size;
    nxt_conf_value_t           *targets;
} nxt_python_app_conf_t;


typedef struct {
    nxt_conf_value_t           *targets;
    nxt_conf_value_t           *options;
} nxt_php_app_conf_t;


typedef struct {
    char       *script;
    uint32_t   threads;
    uint32_t   thread_stack_size;
} nxt_perl_app_conf_t;


typedef struct {
    nxt_str_t  script;
    uint32_t   threads;
    nxt_str_t  hooks;
} nxt_ruby_app_conf_t;


typedef struct {
    nxt_conf_value_t           *classpath;
    char                       *webapp;
    nxt_conf_value_t           *options;
    char                       *unit_jars;
    uint32_t                   threads;
    uint32_t                   thread_stack_size;
} nxt_java_app_conf_t;


typedef struct {
    const char        *module;

    const char        *request_handler;
    const char        *malloc_handler;
    const char        *free_handler;

    const char        *module_init_handler;
    const char        *module_end_handler;
    const char        *request_init_handler;
    const char        *request_end_handler;
    const char        *response_end_handler;

    nxt_conf_value_t  *access;
} nxt_wasm_app_conf_t;


typedef struct {
    const char        *component;

    nxt_conf_value_t  *access;

    /*
     * "execution_timeout" in milliseconds, 0 for unbounded.  It bounds one
     * invocation of the component's "handle", and only the wasm module acts
     * on it: the guest is preempted inside the module's own process, so
     * unlike "limits": {"start_timeout"} the router has nothing to arm.
     *
     * It is wall clock from the guest's first instruction until "handle"
     * returns, not CPU time, so time the guest spends parked in a host
     * call -- waiting for a slow client to drain the response body, say --
     * is spent too.  And the deadline is only checked while wasm executes,
     * so a guest parked in a host call is not preempted, only trapped when
     * it resumes.  This is not a request timeout; that is
     * "limits": {"timeout"}, which the router arms for every app type.
     */
    nxt_msec_t        execution_timeout;
} nxt_wasm_wc_app_conf_t;


struct nxt_common_app_conf_s {
    nxt_str_t                  name;
    nxt_str_t                  type;
    nxt_str_t                  user;
    nxt_str_t                  group;

    char                       *stdout_log;
    char                       *stderr_log;

    char                       *working_directory;
    nxt_conf_value_t           *environment;

    nxt_conf_value_t           *isolation;
    nxt_conf_value_t           *limits;

    size_t                     shm_limit;
    uint32_t                   request_limit;

    /*
     * "limits": {"start_timeout"} in milliseconds, 0 for unbounded.  The
     * router arms it on the start itself (nxt_router_start_timer_arm());
     * the prototype is given the same number because it is the one process
     * that can act on a worker the router gave up on -- it knows its pid.
     * See nxt_proto_quit_children().
     */
    nxt_msec_t                 start_timeout;

    nxt_fd_t                   shared_port_fd;
    nxt_fd_t                   shared_queue_fd;

    union {
        nxt_external_app_conf_t  external;
        nxt_python_app_conf_t    python;
        nxt_php_app_conf_t       php;
        nxt_perl_app_conf_t      perl;
        nxt_ruby_app_conf_t      ruby;
        nxt_java_app_conf_t      java;
        nxt_wasm_app_conf_t      wasm;
        nxt_wasm_wc_app_conf_t   wasm_wc;
    } u;

    nxt_conf_value_t           *self;
};


struct nxt_app_module_s {
    size_t                     compat_length;
    uint32_t                   *compat;

    nxt_str_t                  type;
    const char                 *version;

    const nxt_fs_mount_t       *mounts;
    nxt_uint_t                 nmounts;

    nxt_application_setup_t    setup;
    nxt_process_start_t        start;
};


nxt_app_lang_module_t *nxt_app_lang_module(nxt_runtime_t *rt, nxt_str_t *name);
nxt_app_type_t nxt_app_parse_type(u_char *p, size_t length);

#if (NXT_TESTS)
/*
 * Invokes the static nxt_proto_start_process_handler() with a synthesised
 * runtime and message; see src/test/nxt_main_start_process_reply_test.c.
 */
void nxt_proto_test_run_start_process_handler(nxt_task_t *task,
    nxt_port_recv_msg_t *msg);
#endif

NXT_EXPORT extern nxt_str_t  nxt_server;
extern nxt_app_module_t      nxt_external_module;

NXT_EXPORT nxt_int_t nxt_unit_default_init(nxt_task_t *task,
    nxt_unit_init_t *init, nxt_common_app_conf_t *conf);

NXT_EXPORT nxt_int_t nxt_app_set_logs(void);

#endif /* _NXT_APPLICATION_H_INCLIDED_ */
