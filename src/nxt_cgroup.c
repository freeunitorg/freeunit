/*
 * Copyright (C) Andrew Clayton
 * Copyright (C) F5, Inc.
 */

#include <nxt_main.h>

#include <nxt_cgroup.h>


static int nxt_mk_cgpath_relative(nxt_task_t *task, const char *dir,
    char *cgpath, nxt_pid_t pid);
static nxt_int_t nxt_mk_cgpath(nxt_task_t *task, const char *dir,
    char *cgpath, nxt_pid_t pid);


nxt_int_t
nxt_cgroup_proc_add(nxt_task_t *task, nxt_process_t *process)
{
    int        len;
    char       cgprocs[NXT_MAX_PATH_LEN];
    FILE       *fp;
    nxt_int_t  ret;

    if (task->thread->runtime->type != NXT_PROCESS_MAIN
        || nxt_process_type(process) != NXT_PROCESS_PROTOTYPE
        || process->isolation.cgroup.path == NULL)
    {
        return NXT_OK;
    }

    /*
     * Resolve the cgroup path against /proc/<child>/cgroup rather than
     * /proc/self/cgroup: the parent's cgroup view may differ from the
     * just-forked child's, particularly when CLONE_NEWCGROUP is in play
     * and the configured path is relative.  Reading the child's own
     * /proc entry avoids a TOCTOU where the parent moves between cgroups
     * after fork() but before this write.
     */
    ret = nxt_mk_cgpath(task, process->isolation.cgroup.path, cgprocs,
                        process->pid);
    if (nxt_slow_path(ret == NXT_ERROR)) {
        return NXT_ERROR;
    }

    ret = nxt_fs_mkdir_p((const u_char *) cgprocs, 0777);
    if (nxt_slow_path(ret == NXT_ERROR)) {
        return NXT_ERROR;
    }

    len = strlen(cgprocs);

    /*
     * Stash the resolved directory before appending "/cgroup.procs" so
     * nxt_cgroup_cleanup() can rmdir without re-reading /proc/<pid>/cgroup,
     * which is gone once the child has exited.
     */
    process->isolation.cgroup.resolved_path = nxt_mp_alloc(process->mem_pool,
                                                           len + 1);
    if (nxt_fast_path(process->isolation.cgroup.resolved_path != NULL)) {
        nxt_memcpy(process->isolation.cgroup.resolved_path, cgprocs, len);
        process->isolation.cgroup.resolved_path[len] = '\0';
    }

    len = snprintf(cgprocs + len, NXT_MAX_PATH_LEN - len, "/cgroup.procs");
    if (nxt_slow_path(len >= NXT_MAX_PATH_LEN - len)) {
        nxt_errno = ENAMETOOLONG;
        return NXT_ERROR;
    }

    fp = nxt_file_fopen(task, cgprocs, "we");
    if (nxt_slow_path(fp == NULL)) {
        return NXT_ERROR;
    }

    setvbuf(fp, NULL, _IONBF, 0);
    len = fprintf(fp, "%d\n", process->pid);
    nxt_file_fclose(task, fp);

    if (nxt_slow_path(len < 0)) {
        return NXT_ERROR;
    }

    return NXT_OK;
}


void
nxt_cgroup_cleanup(nxt_task_t *task, const nxt_process_t *process)
{
    char       *ptr;
    char       cgroot[NXT_MAX_PATH_LEN], cgpath[NXT_MAX_PATH_LEN];
    nxt_int_t  ret;

    /*
     * cgroot is the parent process's own cgroup directory; we must not
     * rmdir it.  Resolved against /proc/self/cgroup (pid=0): the child
     * is gone by the time cleanup runs, so /proc/<child_pid>/cgroup no
     * longer exists.  The TOCTOU concern that motivated using the
     * child's view in nxt_cgroup_proc_add() does not apply at cleanup
     * — we just need a stop boundary, and rmdir on the parent's own
     * cgroup will fail anyway because it is non-empty.
     */
    ret = nxt_mk_cgpath(task, "", cgroot, 0);
    if (nxt_slow_path(ret == NXT_ERROR)) {
        return;
    }

    /*
     * Use the resolved path cached by nxt_cgroup_proc_add(); falling
     * back to /proc/<pid>/cgroup here would fail with ENOENT.  If the
     * cache was missed (e.g. mp_alloc failure during add), there is
     * nothing to clean up that we can address safely — bail out.
     */
    if (process->isolation.cgroup.resolved_path == NULL) {
        return;
    }

    ret = snprintf(cgpath, sizeof(cgpath), "%s",
                   process->isolation.cgroup.resolved_path);
    if (nxt_slow_path(ret < 0 || (size_t) ret >= sizeof(cgpath))) {
        return;
    }

    while (*cgpath != '\0' && strcmp(cgroot, cgpath) != 0) {
        rmdir(cgpath);
        ptr = strrchr(cgpath, '/');
        if (ptr == NULL) {
            break;
        }
        *ptr = '\0';
    }
}


static int
nxt_mk_cgpath_relative(nxt_task_t *task, const char *dir, char *cgpath,
    nxt_pid_t pid)
{
    int         i, len;
    char        *buf, *ptr;
    FILE        *fp;
    size_t      size;
    ssize_t     nread;
    nxt_bool_t  found;
    char        procpath[NXT_MAX_PATH_LEN];

    if (pid > 0) {
        len = snprintf(procpath, sizeof(procpath), "/proc/%d/cgroup",
                       (int) pid);
        if (len < 0 || (size_t) len >= sizeof(procpath)) {
            nxt_errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        nxt_memcpy(procpath, "/proc/self/cgroup",
                   sizeof("/proc/self/cgroup"));
    }

    fp = nxt_file_fopen(task, procpath, "re");
    if (nxt_slow_path(fp == NULL)) {
        return -1;
    }

    len = -1;
    buf = NULL;
    found = 0;
    while ((nread = getline(&buf, &size, fp)) != -1) {
        if (strncmp(buf, "0::", 3) == 0) {
            found = 1;
            break;
        }
    }

    nxt_file_fclose(task, fp);

    if (!found) {
        nxt_errno = ENODATA;
        goto out_free_buf;
    }

    buf[nread - 1] = '\0';  /* lose the trailing '\n' */
    ptr = buf;
    for (i = 0; i < 2; i++) {
        ptr = strchr(ptr, ':');
        if (ptr == NULL) {
            nxt_errno = ENODATA;
            goto out_free_buf;
        }

        ptr++;
    }

    len = snprintf(cgpath, NXT_MAX_PATH_LEN, NXT_CGROUP_ROOT "%s/%s",
                   ptr, dir);

out_free_buf:

    nxt_free(buf);

    return len;
}


static nxt_int_t
nxt_mk_cgpath(nxt_task_t *task, const char *dir, char *cgpath, nxt_pid_t pid)
{
    int  len;

    /*
     * If the path from the config is relative, we need to make
     * the cgroup path include the main unit processes cgroup. I.e
     *
     *   NXT_CGROUP_ROOT/<main process cgroup>/<cgroup path>
     *
     * pid: read /proc/<pid>/cgroup so the path reflects the just-forked
     *      child's cgroup view (or 0 to fall back to /proc/self/cgroup).
     */
    if (dir[0] != '/') {
        len = nxt_mk_cgpath_relative(task, dir, cgpath, pid);
    } else {
        len = snprintf(cgpath, NXT_MAX_PATH_LEN, NXT_CGROUP_ROOT "%s", dir);
    }

    if (len == -1) {
        return NXT_ERROR;
    }

    if (len >= NXT_MAX_PATH_LEN) {
        nxt_errno = ENAMETOOLONG;
        return NXT_ERROR;
    }

    return NXT_OK;
}
