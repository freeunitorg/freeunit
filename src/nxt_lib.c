
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_span.h>


nxt_uint_t    nxt_ncpu = 1;
nxt_uint_t    nxt_ncpu_unlimited;
nxt_uint_t    nxt_pagesize;
nxt_task_t    nxt_main_task;
nxt_atomic_t  nxt_task_ident;

nxt_thread_declare_data(nxt_thread_t, nxt_thread_context);


#if (NXT_LINUX)
static nxt_uint_t nxt_cgroup_cpu_limit(void);
static nxt_uint_t nxt_cgroup_cpu_max_read(const char *dir);
#endif


#if (NXT_DEBUG && NXT_FREEBSD)
/*
 * Fill memory with 0xA5 after malloc() and with 0x5A before free().
 * malloc() options variable has to override the libc symbol, otherwise
 * it has no effect.
 */
#if __FreeBSD_version < 1000011
const char *_malloc_options = "J";
#else
const char *malloc_conf = "junk:true";
#endif
#endif


nxt_int_t
nxt_lib_start(const char *app, char **argv, char ***envp)
{
    int           n = 0;
    nxt_int_t     flags;
    nxt_bool_t    update;
    nxt_thread_t  *thread;
#if (NXT_LINUX)
    nxt_uint_t    limit;
#endif

    flags = nxt_stderr_start();

    nxt_log_start(app);

    nxt_pid = getpid();
    nxt_ppid = getppid();
    nxt_euid = geteuid();
    nxt_egid = getegid();

#if (NXT_DEBUG)

    nxt_main_log.level = NXT_LOG_DEBUG;

#if (NXT_HAVE_MALLOPT)
    /* Fill memory with 0xAA after malloc() and with 0x55 before free(). */
    mallopt(M_PERTURB, 0x55);
#endif

#if (NXT_MACOSX)
    /* Fill memory with 0xAA after malloc() and with 0x55 before free(). */
    setenv("MallocScribble", "1", 0);
#endif

#endif /* NXT_DEBUG */

    /* Thread log is required for nxt_malloc() in nxt_strerror_start(). */

    nxt_thread_init_data(nxt_thread_context);
    thread = nxt_thread();
    thread->log = &nxt_main_log;

    thread->handle = nxt_thread_handle();
    thread->time.signal = -1;
    nxt_thread_time_update(thread);

    nxt_main_task.thread = thread;
    nxt_main_task.log = thread->log;
    nxt_main_task.ident = nxt_task_next_ident();

    if (nxt_strerror_start() != NXT_OK) {
        return NXT_ERROR;
    }

    if (flags != -1) {
        nxt_debug(&nxt_main_task, "stderr flags: 0x%04Xd", flags);
    }

#ifdef _SC_NPROCESSORS_ONLN
    /* Linux, FreeBSD, Solaris, MacOSX. */
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif

#if (NXT_HAVE_LINUX_SCHED_GETAFFINITY)
    if (n > 0) {
        int        err;
        size_t     size;
        cpu_set_t  *set;

        set = CPU_ALLOC(n);
        if (set == NULL) {
            return NXT_ERROR;
        }

        size = CPU_ALLOC_SIZE(n);

        err = sched_getaffinity(0, size, set);
        if (err == 0) {
            n = CPU_COUNT_S(size, set);
        }

        CPU_FREE(set);
    }

#elif (NXT_HPUX)
    n = mpctl(MPC_GETNUMSPUS, NULL, NULL);

#endif

#if (NXT_LINUX)
    /*
     * A CPU bandwidth limit (cgroup v2 "cpu.max") does not change the
     * affinity mask.  A container limited to 2 CPUs on a 64-CPU host
     * otherwise starts 64 router threads.
     */
    if (n > 1) {
        limit = nxt_cgroup_cpu_limit();

        if (limit != 0 && limit < (nxt_uint_t) n) {
            /* nxt_runtime_start() writes the record to unit.log. */
            nxt_ncpu_unlimited = n;
            n = (int) limit;
        }
    }
#endif

    nxt_debug(&nxt_main_task, "ncpu: %d", n);

    if (n > 1) {
        nxt_ncpu = n;
    }

    nxt_thread_spin_init(nxt_ncpu, 0);

    nxt_random_init(&thread->random);

    nxt_pagesize = getpagesize();

    nxt_debug(&nxt_main_task, "pagesize: %ui", nxt_pagesize);

    if (argv != NULL) {
        update = (argv[0] == app);

        nxt_process_arguments(&nxt_main_task, argv, envp);

        if (update) {
            nxt_log_start(nxt_process_argv[0]);
        }
    }

    return NXT_OK;
}


/*
 * Parses the contents of a cgroup v2 "cpu.max" file: "<quota> <period>",
 * or "max <period>" for no limit.  Returns the quota in whole CPUs, rounded
 * up, or 0 if there is no limit or the contents are not valid.
 */

nxt_uint_t
nxt_cgroup_cpu_max_parse(const u_char *p, size_t len)
{
    size_t        n;
    nxt_off_t     quota, period;
    nxt_span_t    span;
    const u_char  *sp, *field;

    /* A trailing newline or space is not part of the value. */

    while (len != 0 && (p[len - 1] == '\n' || p[len - 1] == ' ')) {
        len--;
    }

    nxt_span_init(&span, p, p + len);

    /* The quota is the bytes before the first space. */

    sp = memchr(p, ' ', len);
    if (sp == NULL) {
        return 0;
    }

    n = sp - p;

    if (nxt_span_take(&span, n, &field) != 0) {
        return 0;
    }

    quota = nxt_off_t_parse(field, n);

    /* The space. */

    if (nxt_span_take(&span, 1, &field) != 0) {
        return 0;
    }

    /* The period is the rest. */

    n = nxt_span_len(&span);

    if (nxt_span_take(&span, n, &field) != 0) {
        return 0;
    }

    period = nxt_off_t_parse(field, n);

    if (quota <= 0 || period <= 0) {
        return 0;
    }

    /* Rounds up without an addition that could overflow. */
    quota = quota / period + (quota % period != 0);

    return (quota > (nxt_off_t) NXT_INT32_T_MAX) ? 0 : (nxt_uint_t) quota;
}


/*
 * /proc/self/mountinfo writes a space, a tab, a newline and a backslash in
 * a path as a backslash and three octal digits, such as "\040" for a space.
 * Decodes these escapes in place.  /proc/self/cgroup does not escape a
 * path, so the decoded mount root can be compared with it.
 */

void
nxt_cgroup_mountinfo_unescape(char *s)
{
    char  *d;

    d = s;

    while (*s != '\0') {

        if (s[0] == '\\'
            && s[1] >= '0' && s[1] <= '3'
            && s[2] >= '0' && s[2] <= '7'
            && s[3] >= '0' && s[3] <= '7')
        {
            *d++ = (char) (((s[1] - '0') << 6) | ((s[2] - '0') << 3)
                           | (s[3] - '0'));
            s += 4;
            continue;
        }

        *d++ = *s++;
    }

    *d = '\0';
}


/*
 * Returns the path of "cgroup" below the root "root" of the cgroup2 mount:
 * "" for the root itself, else a path that starts with "/".  Returns NULL
 * if the cgroup is not below the root, or if its path has a ".." component.
 * Such a cgroup is outside the cgroup namespace, and the mount does not
 * show it.
 */

const char *
nxt_cgroup_relative_path(const char *cgroup, const char *root)
{
    size_t      len;
    const char  *rel, *p;

    rel = cgroup;

    if (strcmp(root, "/") != 0) {
        len = strlen(root);

        if (strncmp(cgroup, root, len) != 0
            || (cgroup[len] != '/' && cgroup[len] != '\0'))
        {
            return NULL;
        }

        rel = cgroup + len;
    }

    /* Only a whole ".." component: a cgroup can be named "..foo". */

    for (p = strstr(rel, "/.."); p != NULL; p = strstr(p + 1, "/..")) {

        if (p[3] == '/' || p[3] == '\0') {
            return NULL;
        }
    }

    if (strcmp(rel, "/") == 0) {
        return "";
    }

    return rel;
}


#if (NXT_LINUX)

/*
 * Returns the lowest CPU limit of the cgroup v2 directories from the
 * cgroup of this process up to the root of the cgroup2 mount, or 0 if
 * no level sets a limit.  The root of the mount is the root of the cgroup
 * namespace of the process.  With a private cgroup namespace, as in a
 * container, a limit on a parent of the namespace root is not seen.  The
 * cgroup v1 "cpu" controller is not read.
 */

static nxt_uint_t
nxt_cgroup_cpu_limit(void)
{
    int         i, len;
    char        *line, *p, *cgroup, *root, *mnt, *slash;
    FILE        *fp;
    size_t      size, base;
    ssize_t     nread;
    nxt_uint_t  limit, cpus;
    const char  *rel;
    char        dir[NXT_MAX_PATH_LEN];

    line = NULL;
    size = 0;
    cgroup = NULL;
    limit = 0;

    fp = fopen("/proc/self/cgroup", "re");
    if (fp == NULL) {
        return 0;
    }

    while ((nread = getline(&line, &size, fp)) > 0) {
        if (nread > 3 && memcmp(line, "0::", 3) == 0) {
            if (line[nread - 1] == '\n') {
                line[nread - 1] = '\0';
            }

            cgroup = strdup(line + 3);
            break;
        }
    }

    fclose(fp);

    if (cgroup == NULL) {
        goto done;
    }

    fp = fopen("/proc/self/mountinfo", "re");
    if (fp == NULL) {
        goto done;
    }

    root = NULL;
    mnt = NULL;

    while (getline(&line, &size, fp) > 0) {
        p = strstr(line, " - cgroup2 ");
        if (p == NULL) {
            continue;
        }

        *p = '\0';

        /* "<id> <parent> <major:minor> <root> <mount point> ..." */

        p = line;

        for (i = 0; i < 3 && p != NULL; i++) {
            p = strchr(p, ' ');

            if (p != NULL) {
                p++;
            }
        }

        if (p == NULL) {
            continue;
        }

        root = p;

        p = strchr(p, ' ');
        if (p == NULL) {
            continue;
        }

        *p++ = '\0';
        mnt = p;

        p = strchr(p, ' ');
        if (p != NULL) {
            *p = '\0';
        }

        break;
    }

    fclose(fp);

    if (mnt == NULL) {
        goto done;
    }

    nxt_cgroup_mountinfo_unescape(root);
    nxt_cgroup_mountinfo_unescape(mnt);

    /* The cgroup path is relative to the root of the cgroup namespace. */

    rel = nxt_cgroup_relative_path(cgroup, root);
    if (rel == NULL) {
        goto done;
    }

    len = snprintf(dir, sizeof(dir), "%s%s", mnt, rel);
    if (len < 0 || (size_t) len >= sizeof(dir)) {
        goto done;
    }

    base = strlen(mnt);

    for ( ;; ) {
        cpus = nxt_cgroup_cpu_max_read(dir);

        if (cpus != 0 && (limit == 0 || cpus < limit)) {
            limit = cpus;
        }

        if ((size_t) len <= base) {
            break;
        }

        slash = strrchr(dir + base, '/');
        if (slash == NULL) {
            break;
        }

        *slash = '\0';
        len = slash - dir;
    }

done:

    nxt_free(cgroup);
    nxt_free(line);

    return limit;
}


static nxt_uint_t
nxt_cgroup_cpu_max_read(const char *dir)
{
    int      fd, len;
    ssize_t  n;
    u_char   buf[64];
    char     path[NXT_MAX_PATH_LEN];

    len = snprintf(path, sizeof(path), "%s/cpu.max", dir);
    if (len < 0 || (size_t) len >= sizeof(path)) {
        return 0;
    }

    /* The root cgroup and a cgroup without the "cpu" controller have none. */

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        return 0;
    }

    n = read(fd, buf, sizeof(buf));

    close(fd);

    if (n <= 0) {
        return 0;
    }

    return nxt_cgroup_cpu_max_parse(buf, n);
}

#endif


void
nxt_lib_stop(void)
{
    /*
     * There is no in-tree caller for this exported symbol, and it has no
     * access to the runtime.  Event engines are per-thread and are torn down
     * on the nxt_runtime_quit path by their owning threads; stopping them
     * from here would require runtime access -- either an ABI-breaking change
     * to this signature or a new global.  The symbol keeps its historical
     * immediate-exit behavior for ABI compatibility.
     */

    exit(0);
    nxt_unreachable();
}
