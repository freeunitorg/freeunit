/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_ISOLATION_H_INCLUDED_
#define _NXT_ISOLATION_H_INCLUDED_


nxt_int_t nxt_isolation_main_prefork(nxt_task_t *task, nxt_process_t *process,
    nxt_mp_t *mp);

#if (NXT_HAVE_ISOLATION_ROOTFS)
nxt_int_t nxt_isolation_prepare_rootfs(nxt_task_t *task,
    nxt_process_t *process);
nxt_int_t nxt_isolation_change_root(nxt_task_t *task, nxt_process_t *process);

#if (NXT_TESTS)
/* The mount destination walk, for the mount destination test. */
nxt_int_t nxt_isolation_test_mount_dst_open(nxt_task_t *task, int rootfs_fd,
    const char *rel, nxt_bool_t fallback, nxt_bool_t *openat2_used, int *fdp);
#endif
#endif

#endif /* _NXT_ISOLATION_H_INCLUDED_ */
