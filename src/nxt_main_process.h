
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_MAIN_PROCESS_H_INCLUDED_
#define _NXT_MAIN_PROCESS_H_INCLUDED_


typedef enum {
    NXT_SOCKET_ERROR_SYSTEM = 0,
    NXT_SOCKET_ERROR_NOINET6,
    NXT_SOCKET_ERROR_PORT,
    NXT_SOCKET_ERROR_INUSE,
    NXT_SOCKET_ERROR_NOADDR,
    NXT_SOCKET_ERROR_ACCESS,
    NXT_SOCKET_ERROR_PATH,
} nxt_socket_error_t;


nxt_int_t nxt_main_process_start(nxt_thread_t *thr, nxt_task_t *task,
    nxt_runtime_t *runtime);
nxt_int_t nxt_main_file_store(nxt_task_t *task, const char *dir,
    const char *tmp_name, const char *name, u_char *buf, size_t size);


NXT_EXPORT extern nxt_uint_t                nxt_conf_ver;
NXT_EXPORT extern const nxt_process_init_t  nxt_discovery_process;
NXT_EXPORT extern const nxt_process_init_t  nxt_controller_process;
NXT_EXPORT extern const nxt_process_init_t  nxt_router_process;
NXT_EXPORT extern const nxt_process_init_t  nxt_proto_process;
NXT_EXPORT extern const nxt_process_init_t  nxt_app_process;

extern const nxt_sig_event_t  nxt_main_process_signals[];
extern const nxt_sig_event_t  nxt_process_signals[];

#if (NXT_TESTS)
void nxt_main_test_process_new_failures(nxt_uint_t failures);
void nxt_main_test_run_start_process_handler(nxt_task_t *task,
    nxt_port_recv_msg_t *msg);
void nxt_main_test_run_whoami_handler(nxt_task_t *task,
    nxt_port_recv_msg_t *msg);
#if (NXT_USE_CMSG_PID)
void nxt_main_test_run_name_child(nxt_task_t *task, nxt_process_t *pprocess,
    nxt_process_t *process, nxt_pid_t ns_pid);
void nxt_main_test_run_remove_child_pid_handler(nxt_task_t *task,
    nxt_port_recv_msg_t *msg);
#endif
#endif


#endif /* _NXT_MAIN_PROCESS_H_INCLUDED_ */
