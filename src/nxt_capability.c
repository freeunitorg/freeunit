/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

/*
 * NOTE: this module does two separate things, in two different
 * processes.
 *
 * nxt_capability_set() is *detection* (capget), run once in the main
 * process from nxt_runtime_conf_init().  nxt_isolation.c and the
 * credential machinery use its answer to decide whether a non-root
 * unitd can honour setuid/setgid in the "user"/"group" config keys
 * and "rootfs" isolation.
 *
 * nxt_capability_drop() is *disposal* (capset), run in every forked
 * process from the tail of nxt_process_apply_creds().  It is never
 * run in main, which keeps CAP_NET_BIND_SERVICE for the bind() in
 * nxt_main_listening_socket() -- listeners are created on every
 * reconfiguration, long after conf_init, so main cannot drop
 * anything.  See nxt_capability_drop() for the rest.
 */

#include <nxt_main.h>

#if (NXT_HAVE_LINUX_CAPABILITY)

#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>


#if (_LINUX_CAPABILITY_VERSION_3)
#define NXT_CAPABILITY_VERSION  _LINUX_CAPABILITY_VERSION_3
#elif (_LINUX_CAPABILITY_VERSION_2)
#define NXT_CAPABILITY_VERSION  _LINUX_CAPABILITY_VERSION_2
#else
#define NXT_CAPABILITY_VERSION  _LINUX_CAPABILITY_VERSION
#endif


#define nxt_capget(hdrp, datap)                                               \
            syscall(SYS_capget, hdrp, datap)
#define nxt_capset(hdrp, datap)                                               \
            syscall(SYS_capset, hdrp, datap)

#endif /* NXT_HAVE_LINUX_CAPABILITY */


static nxt_int_t nxt_capability_specific_set(nxt_task_t *task,
    nxt_capabilities_t *cap);


nxt_int_t
nxt_capability_set(nxt_task_t *task, nxt_capabilities_t *cap)
{
    nxt_assert(cap->setid == 0);

    if (nxt_euid == 0) {
        cap->setid = 1;
        cap->chroot = 1;
        return NXT_OK;
    }

    return nxt_capability_specific_set(task, cap);
}


#if (NXT_HAVE_LINUX_CAPABILITY)

static uint32_t
nxt_capability_linux_get_version(void)
{
    struct __user_cap_header_struct hdr;

    hdr.version = NXT_CAPABILITY_VERSION;
    hdr.pid     = nxt_pid;

    nxt_capget(&hdr, NULL);
    return hdr.version;
}


static nxt_int_t
nxt_capability_specific_set(nxt_task_t *task, nxt_capabilities_t *cap)
{
    nxt_err_t                        err;
    struct __user_cap_data_struct    *val, data[2];
    struct __user_cap_header_struct  hdr;

    /*
     * Linux capability v1 fills an u32 struct.
     * Linux capability v2 and v3 fills an u64 struct.
     * We allocate data[2] for compatibility, we waste 4 bytes on v1.
     *
     * This is safe as we only need to check CAP_SETUID and CAP_SETGID
     * that resides in the first 32-bit chunk.
     */

    val = &data[0];

    /*
     * Ask the kernel the preferred capability version
     * instead of using _LINUX_CAPABILITY_VERSION from header.
     * This is safer when distributing a pre-compiled Unit binary.
     */
    hdr.version = nxt_capability_linux_get_version();
    hdr.pid = nxt_pid;

    if (nxt_slow_path(nxt_capget(&hdr, val) == -1)) {
        err = nxt_errno;

        /*
         * A syscall filter that does not allow capget() reports it
         * through SECCOMP_RET_ERRNO, conventionally as EPERM or ENOSYS.
         * Capabilities then cannot be observed at all, which is not a
         * reason to refuse to start: leave both flags clear and run the
         * way an ordinary unprivileged unitd already does.
         *
         * Any other errno means the call itself is wrong -- EINVAL,
         * which a working version probe would have prevented, or EFAULT
         * for a bad pointer -- so it is a bug in Unit rather than a
         * policy decision made by the operator.  Keep failing closed
         * there, rather than silently downgrading privileges.
         */

        if (err == NXT_EPERM || err == NXT_ENOSYS) {
            nxt_log(task, NXT_LOG_WARN, "capget() failed %E; process "
                    "capabilities are unknown and will not be used: user and "
                    "group switching and \"rootfs\" isolation are disabled "
                    "for applications that do not enable the \"credential\" "
                    "namespace, and applications run as uid %d",
                    err, (int) nxt_euid);

            /*
             * The log file is not open yet -- this runs from
             * nxt_runtime_conf_init() -- so the warning above reaches
             * stderr only.  Record it, and nxt_runtime_start() repeats it
             * once unit.log exists, or a daemonised unitd would keep no
             * trace of running without knowing its own capabilities.
             */
            cap->unknown = 1;

            return NXT_OK;
        }

        nxt_alert(task, "failed to get process capabilities: %E", err);
        return NXT_ERROR;
    }

    if ((val->effective & (1 << CAP_SYS_CHROOT)) != 0) {
        cap->chroot = 1;
    }

    if ((val->effective & (1 << CAP_SETUID)) == 0) {
        return NXT_OK;
    }

    if ((val->effective & (1 << CAP_SETGID)) == 0) {
        return NXT_OK;
    }

    cap->setid = 1;
    return NXT_OK;
}


/*
 * Drop every capability the calling process still holds.  Called from
 * the tail of nxt_process_apply_creds(), which is the last thing that
 * runs in a freshly forked prototype, router, controller or discovery
 * process before it starts doing work on an application's behalf -- and
 * after setgid()/setuid(), after unshare() and main's uid_map write,
 * and after all the mount/pivot_root/chroot work nxt_proto_setup() does
 * for "rootfs".  Application workers never reach this function: the
 * prototype forks them and nxt_app_setup() calls init->start directly,
 * so they inherit sets this call already emptied.
 *
 * It is not the earliest possible point.  nxt_proto_setup() calls the
 * language module's setup first, and for PHP that is
 * php_module_startup() with the php.ini named in the configuration,
 * which loads and initialises whatever extensions that file lists.
 * Nothing can be dropped before that: the mount, pivot_root and
 * chroot calls later in the same function need CAP_SYS_ADMIN and
 * CAP_SYS_CHROOT.  The window is unchanged by this code and is not
 * widened by it -- setgid()/setuid() have not happened yet either, so
 * module startup has always run with unitd's own uid, which is the
 * larger privilege of the two.
 *
 * Capabilities are per-thread, and capset() with pid 0 sets only the
 * calling thread's, so this has to run while the process is still
 * single-threaded -- and it does.  The router's engine threads are
 * created from init->start, which runs after this and inherits the
 * emptied sets; the prototype and its workers never leave one thread;
 * thread pools spawn on their first posted work item, which is later
 * still.  The one thread that could pre-exist is the signal thread,
 * and only for an event engine that cannot poll signals itself
 * (nxt_signal_thread_start(), reached from nxt_event_engine_create()
 * when !interface->signal_support).  Linux never selects one: epoll
 * declares NXT_SIGNAL_EVENTS wherever signalfd exists, and rt->engine
 * is only ever set to the interface Unit itself chose
 * (nxt_runtime.c), never by an operator.  Measured: with the router at
 * nine threads, every /proc/<pid>/task/<tid>/status in every child
 * reports empty sets.
 *
 * Why here and not in main: main binds listening sockets in
 * nxt_main_listening_socket() every time the configuration changes, so
 * it needs CAP_NET_BIND_SERVICE for the whole life of the process --
 * and also chown()s sockets, writes uid_map and moves children into
 * cgroups.  nxt_capability_set() runs from nxt_runtime_conf_init(),
 * long before the first bind(), so a drop there would turn "unitd fails
 * to start" into "unitd starts but cannot listen on :80".
 *
 * fork() copies the permitted, effective, inheritable, ambient and
 * bounding sets verbatim, so without this a non-root unitd that was
 * granted, say, CAP_SETUID/CAP_SETGID/CAP_SYS_CHROOT would hand those
 * to every application worker.  Two deployments reach that state:
 *
 *   - capget() filtered.  nxt_capability_specific_set() leaves setid
 *     clear, so nxt_process_apply_creds() skips setgid()/setuid()
 *     entirely and nothing lowers anything.
 *   - capget() working.  The credentials are switched, but from one
 *     nonzero uid to another, and capabilities(7) only clears the sets
 *     on a transition *out of* uid 0 -- so the capabilities survive
 *     the setuid() as well.
 *
 * A root unitd is the case that was already safe: its setuid() away
 * from 0 clears the permitted, effective and ambient sets by itself,
 * which makes the capset() below a verified no-op there rather than a
 * behaviour change.
 *
 * The bounding set is deliberately left alone: emptying it needs
 * CAP_SETPCAP, which this process is about to give up and usually
 * never had, and it is unreachable anyway once PR_SET_NO_NEW_PRIVS is
 * set, because execve() can then add nothing to the permitted set.
 */

nxt_int_t
nxt_capability_drop(nxt_task_t *task)
{
    nxt_err_t                        err;
    struct __user_cap_data_struct    data[2];
    struct __user_cap_header_struct  hdr;

    if (geteuid() == 0) {
        /*
         * An application configured with "user": "root" -- and the
         * initial state of a root unitd, before apply_creds() has
         * switched user -- keeps what root has.  Dropping there would
         * be a compatibility break with no security value: euid 0
         * regains a full permitted set on the next execve() of any
         * ordinary file anyway (capabilities(7), "Capabilities and
         * execution of programs by root"), so the drop could not hold
         * for an exec'd application and would only make an embedded
         * one inconsistently crippled.  This mirrors the geteuid()==0
         * short-circuit in nxt_capability_set() above.
         *
         * It is also the escape hatch for the one case that does lose
         * something here: an application in a "credential" namespace
         * starts with a full capability set in that namespace
         * (user_namespaces(7)), and unless it is mapped to uid 0 it
         * now loses it.  Those capabilities were only ever valid
         * inside the namespace, and everything Unit itself does with
         * them -- the mounts, pivot_root and chroot for "rootfs" --
         * has already happened in nxt_proto_setup() by the time this
         * runs.  An application that wants to keep them can ask for
         * uid 0 within its own namespace.
         *
         * geteuid() rather than nxt_euid: nxt_euid is sampled once in
         * nxt_lib_start() and is stale in a process that has just
         * called setuid().
         */

        return NXT_OK;
    }

#ifdef PR_CAP_AMBIENT

    /*
     * Clear the ambient set first, and separately.  For a non-root
     * process executing a file with no capabilities, capabilities(7)
     * reduces to P'(permitted) = P'(effective) = P(ambient): the
     * ambient set is the only one that carries real privilege across
     * execve(), inheritable survives but does nothing without a
     * matching file inheritable set.  So ambient is precisely what an
     * "external" application -- Go, Node, anything Unit runs through
     * the execve() in nxt_external.c -- would otherwise inherit.
     * PR_CAP_AMBIENT_CLEAR_ALL needs no privilege and no prior read of
     * the set, so it still works when capget() and capset() are both
     * denied; emptying the permitted set below clears ambient
     * implicitly, but only if that call succeeds.
     *
     * A plain #ifdef rather than an auto/ feature test: PR_CAP_AMBIENT
     * is a macro in <sys/prctl.h>, so this compiles out on headers too
     * old to define it, and a kernel older than 4.3 has no ambient set
     * to clear and answers EINVAL, which is not worth a warning.
     */

    if (nxt_slow_path(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0)
                      != 0))
    {
        nxt_debug(task, "prctl(PR_CAP_AMBIENT_CLEAR_ALL) failed %E",
                  nxt_errno);
    }

#endif

    nxt_memzero(data, sizeof(data));

    /*
     * The compiled-in version constant, not nxt_capability_linux_get_version():
     * the probe is a capget() call, and this function has to work in the
     * deployment where capget() is exactly what is filtered.  On every
     * header Unit builds against this is _LINUX_CAPABILITY_VERSION_3,
     * which the kernel has accepted since 2.6.26 and which describes the
     * two-element data array above.
     *
     * pid 0, not nxt_pid: capset() takes only 0 or the caller's own pid,
     * and nxt_pid holds the *global* pid in a process that was moved into
     * a PID namespace (nxt_process_whoami()), which would be EPERM here.
     */

    hdr.version = NXT_CAPABILITY_VERSION;
    hdr.pid     = 0;

    if (nxt_fast_path(nxt_capset(&hdr, data) == 0)) {
        return NXT_OK;
    }

    err = nxt_errno;

    /*
     * Same errno split as nxt_capability_specific_set(): EPERM and
     * ENOSYS are what a syscall filter returns, and a filter is an
     * operator's decision, not an error.  It is also the only way this
     * call can fail with EPERM: lowering all three sets to empty is
     * always permitted for any process (capabilities(7), the transition
     * rules for a caller without CAP_SETPCAP), so a refusal cannot have
     * come from the capability model itself.  Refusing to start here
     * would recreate the outage this whole fallback exists to prevent,
     * and would do it in the worst possible shape: this runs in a
     * forked child, so NXT_ERROR is a dead worker the router keeps
     * respawning, not a visible startup failure.
     *
     * Anything else -- EINVAL, EFAULT -- means the call we just made is
     * malformed, which can only be a bug in Unit, since neither the
     * version nor the pointer depends on anything an operator controls.
     * Fail closed there.
     */

    if (err == NXT_EPERM || err == NXT_ENOSYS) {
        nxt_log(task, NXT_LOG_WARN, "capset() failed %E; capabilities this "
                "process may hold could not be dropped and are inherited by "
                "the applications it runs", err);

        return NXT_OK;
    }

    nxt_alert(task, "failed to drop process capabilities: %E", err);

    return NXT_ERROR;
}

#else

static nxt_int_t
nxt_capability_specific_set(nxt_task_t *task, nxt_capabilities_t *cap)
{
    return NXT_OK;
}


nxt_int_t
nxt_capability_drop(nxt_task_t *task)
{
    return NXT_OK;
}

#endif
