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
#include <sys/vfs.h>


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
 * The magic number rather than <linux/magic.h>: one constant, stable
 * since procfs existed, against a header that is not present on every
 * libc Unit builds against.
 */
#define NXT_PROC_SUPER_MAGIC  0x9fa0


static nxt_int_t
nxt_capability_status_line(u_char *p, u_char *end, uint64_t *bits)
{
    u_char      *q;
    uint64_t    v;
    nxt_uint_t  digits;

    if (end - p <= (ssize_t) nxt_length("CapXxx:")
        || memcmp(p, "Cap", 3) != 0)
    {
        return 0;
    }

    if (memcmp(p, "CapPrm:", 7) != 0
        && memcmp(p, "CapEff:", 7) != 0
        && memcmp(p, "CapAmb:", 7) != 0)
    {
        /* CapInh and CapBnd, deliberately: see nxt_capability_still_held(). */
        return 0;
    }

    q = p + nxt_length("CapXxx:");

    while (q < end && (*q == ' ' || *q == '\t')) {
        q++;
    }

    v = 0;
    digits = 0;

    while (q < end) {
        if (*q >= '0' && *q <= '9') {
            v = (v << 4) | (uint64_t) (*q - '0');

        } else if (*q >= 'a' && *q <= 'f') {
            v = (v << 4) | (uint64_t) (*q - 'a' + 10);

        } else if (*q >= 'A' && *q <= 'F') {
            v = (v << 4) | (uint64_t) (*q - 'A' + 10);

        } else {
            break;
        }

        q++;
        digits++;
    }

    if (digits == 0 || digits > 16) {
        return -1;
    }

    *bits |= v;

    return 1;
}


static nxt_int_t
nxt_capability_proc_status_held(void)
{
    int             fd;
    u_char          *p, *end, *eol;
    size_t          len;
    ssize_t         n;
    uint64_t        bits;
    nxt_int_t       ret;
    nxt_uint_t      found;
    nxt_bool_t      skipping;
    struct statfs   fs;
    u_char          buf[1024];

    fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        return -1;
    }

    /*
     * Prove it is procfs before believing a word of it.  An application
     * that enables a "credential" namespace may also use "rootfs" even
     * when capabilities are unknown, and nxt_proto_setup() has already
     * pivot_root()ed into it by the time this runs -- with
     * "automount": {"procfs": false} there may be no /proc there at all,
     * or there may be a regular file of that name supplied with the
     * application image.  Reading capability sets out of one would let
     * the image decide whether the prototype it is about to run keeps
     * unitd's capabilities.
     */

    if (fstatfs(fd, &fs) != 0 || fs.f_type != NXT_PROC_SUPER_MAGIC) {
        close(fd);
        return -1;
    }

    bits = 0;
    found = 0;
    len = 0;
    skipping = 0;

    /*
     * Streamed over a small buffer, with the tail of a partial line
     * carried across reads, because this file has no bounded size: a
     * process in enough supplementary groups pushes the Cap* lines past
     * any fixed buffer on the strength of its "Groups:" line alone, and
     * reading only the first N bytes would turn "holds nothing" into
     * "cannot tell" and refuse it.  A line too long for the buffer is
     * skipped rather than grown into -- a Cap* line is 25 bytes.
     */

    for ( ;; ) {
        n = read(fd, buf + len, sizeof(buf) - len);

        if (n < 0) {

            /*
             * A signal that arrives mid-read is not an answer about
             * capabilities.  Without the retry it would become one:
             * read() fails, the reader reports "cannot tell", and the
             * caller refuses an application over a SIGCHLD.  Every other
             * errno is a real failure to read the file, and saying so is
             * the honest reply.
             */

            if (nxt_errno == NXT_EINTR) {
                continue;
            }

            close(fd);
            return -1;
        }

        if (n == 0) {
            break;
        }

        end = buf + len + n;
        p = buf;

        for ( ;; ) {
            eol = memchr(p, '\n', (size_t) (end - p));

            if (eol == NULL) {
                break;
            }

            if (!skipping) {
                ret = nxt_capability_status_line(p, eol, &bits);

                if (nxt_slow_path(ret < 0)) {
                    close(fd);
                    return -1;
                }

                found += ret;
            }

            skipping = 0;
            p = eol + 1;
        }

        len = (size_t) (end - p);

        if (len == sizeof(buf)) {
            skipping = 1;
            len = 0;

        } else {
            memmove(buf, p, len);
        }
    }

    /*
     * Whatever is left when the file ends is a last line with no
     * trailing newline.  procfs always terminates the one it writes, so
     * this is unreachable for the real /proc/self/status -- but the
     * fstatfs() above is the only thing standing between this parser and
     * a file supplied with an application image, and dropping the tail
     * would silently turn a "CapPrm: 00000000000000c0" written without a
     * newline into a file whose Cap* lines simply do not count.
     */

    if (len > 0 && !skipping) {
        ret = nxt_capability_status_line(buf, buf + len, &bits);

        if (nxt_slow_path(ret < 0)) {
            close(fd);
            return -1;
        }

        found += ret;
    }

    close(fd);

    /*
     * CapPrm and CapEff, at least.  CapAmb needs Linux 4.3 and is
     * allowed to be missing; fewer than two means this is not the file
     * we think it is, and guessing from it would be worse than saying
     * so.
     */

    if (found < 2) {
        return -1;
    }

    return bits != 0;
}


/*
 * Does this process still hold anything an application could inherit?
 * 1 yes, 0 no, -1 could not be determined.
 *
 * Consulted only after capset() has already been denied, to separate
 * "the operator filtered a call that had nothing left to do" from "the
 * sets really are still full".  Without it the refusal in
 * nxt_process_created_ok() would fire in the two commonest deployments
 * there are, neither of which is unsafe: a root unitd, whose setuid()
 * away from uid 0 empties permitted, effective and ambient before this
 * function is ever reached, and an ordinary unprivileged unitd that was
 * granted nothing.  Both would lose every application to a filter that
 * costs them nothing.
 *
 * Two readers are tried, cheapest first, because neither covers every
 * deployment on its own.
 *
 *   - capget().  A filter that denies capset() often still permits it,
 *     and this is the exact answer with no filesystem involved.  It does
 *     not report the ambient set, which does not matter here:
 *     PR_CAP_AMBIENT_CLEAR_ALL in nxt_capability_drop() has already
 *     run, needs no privilege and cannot be refused by the capability
 *     model.  pid 0 rather than nxt_pid for the same reason capset()
 *     uses it -- nxt_pid is the global pid, which does not resolve from
 *     inside a PID namespace.
 *
 *   - /proc/self/status, when capget() is filtered too -- the shape this
 *     whole fallback exists for.  A syscall filter cannot see through
 *     open() and read().
 *
 * Both are readings.  Nothing here is inferred from what this process
 * ought to be holding, and an earlier draft that did -- a root unitd's
 * setuid() away from uid 0 empties permitted, effective and ambient, so
 * the transition alone looks like an answer -- was removed rather than
 * patched.  It was wrong two separate ways: SECBIT_KEEP_CAPS or
 * SECBIT_NO_SETUID_FIXUP, either of which a service manager can set,
 * suppress the clearing entirely; and inside a "credential" namespace a
 * process can reach a nonzero euid while holding a full capability set
 * in that namespace, never having made the transition at all.  Both
 * leave a process that looks disarmed and is not.  It bought only the
 * case where capget() is filtered *and* procfs is unreadable, and being
 * wrong there is exactly where it costs the most.
 *
 * If neither reader answers, the caller refuses: this is a security
 * fallback, and "could not tell" is not "there is nothing there".
 *
 * "Anything" means the permitted, effective and ambient sets, and only
 * those three.  The capset() this reader is consulted after still
 * empties the inheritable set along with them whenever it is allowed to
 * run -- what this function decides is narrower, and it is the only
 * question worth asking here: is anything left that an application
 * could use?
 *
 * An inheritable bit is not.  It confers nothing in the process holding
 * it; capabilities(7) turns it into privilege only on an execve() of a
 * file whose own inheritable capability set intersects it.  Unit's
 * language modules are fork()ed and never exec()ed, so they cannot
 * reach that rule at all, and an "external" application execs the
 * binary named in its configuration -- an ordinary file, carrying no
 * file capabilities unless an operator deliberately gave it some, in
 * which case they asked for exactly this.  Refusing over inheritable
 * alone would therefore be refusing over something strictly narrower
 * than the escalation this fallback exists to stop, and it is also
 * precisely what an unprivileged unitd handed its workers before
 * nxt_capability_drop() existed: nothing in Unit has ever cleared an
 * inheritable set before now.
 *
 * That leaves a residual, and it is left deliberately: an "external"
 * application whose own binary an operator gave a matching file
 * inheritable capability does get that bit promoted on execve().
 * Refusing would not take it away -- capset() has just been denied, so
 * nothing here can clear anything -- it would only take away the
 * application.  The trade is a certain outage on every host with a
 * stray inheritable bit against a bit the operator had to place twice,
 * once on unitd and once on the file it execs.
 *
 * Counting it would cost a great deal, too.  Inheritable bits turn up on
 * ordinary hosts without anyone meaning them to matter -- pam_cap exists
 * to set exactly them, and the desktop this was measured on carries
 * CapInh=0000000800000000 over entirely empty permitted, effective and
 * ambient sets.  A root unitd is no safer either, because setuid() away
 * from uid 0 clears permitted, effective and ambient and leaves
 * inheritable exactly as it was.  Both would lose every application to a
 * filter that costs them nothing.
 * Conditioning it on PR_SET_NO_NEW_PRIVS does not rescue the idea
 * either: no_new_privs is off in the default configuration --
 * nxt_isolation_main_prefork() sets isolation.new_privs to 1 and
 * nxt_process_apply_creds() issues the prctl only when it is 0 -- so
 * the condition would be false on exactly the hosts that have the
 * stray bit.
 *
 * The bounding set is never counted either, for the reason it is never
 * emptied: it is a ceiling on what an execve() may add, not something
 * this process holds, and capset() would not have cleared it either.
 *
 * The ambient set is read where a reader offers it, even though the
 * kernel keeps it a subset of permitted -- so an empty permitted
 * already implies an empty ambient -- because the
 * PR_CAP_AMBIENT_CLEAR_ALL in nxt_capability_drop() is only warned
 * about when it fails.
 */
static nxt_int_t
nxt_capability_still_held(void)
{
    uint32_t                         bits;
    struct __user_cap_data_struct    data[2];
    struct __user_cap_header_struct  hdr;

    nxt_memzero(data, sizeof(data));

    hdr.version = NXT_CAPABILITY_VERSION;
    hdr.pid     = 0;

    if (nxt_fast_path(nxt_capget(&hdr, data) == 0)) {
        bits = data[0].effective | data[0].permitted
               | data[1].effective | data[1].permitted;

        return bits != 0;
    }

    return nxt_capability_proc_status_held();
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
 * never had.  It is a ceiling on what an execve() may add to the
 * permitted set, and with inheritable emptied below the only thing
 * left for it to admit is a file capability on the binary an
 * application execve()s -- which is the operator's own choice, not
 * something inherited from unitd.  PR_SET_NO_NEW_PRIVS would close
 * even that, but it is off in the default configuration (see the
 * comment on nxt_capability_still_held() above), so it is not what
 * makes this safe.
 *
 * The return value is three-way and advisory in the middle case:
 * NXT_OK when nothing is left to inherit, NXT_ERROR when the call was
 * malformed, and NXT_DECLINED when a syscall filter denied capset()
 * and this process is still holding something as a result.
 * NXT_DECLINED is deliberately not a refusal here.  This function is
 * reached by the router, the controller and discovery as well, through
 * nxt_process_core_setup(), and failing those closed is worse than the
 * bug it would be protecting against: router and controller carry
 * .restart = 1 and are re-forked immediately and without backoff, so a
 * filter that is going to deny every attempt turns into an unbounded
 * respawn loop; and discovery notifies no one when it dies, leaving
 * unitd running with no modules and no control socket.  Only the caller
 * knows whether application code is about to inherit what could not be
 * dropped, so only the caller can decide.
 */
nxt_int_t
nxt_capability_drop(nxt_task_t *task)
{
    nxt_int_t                        held;
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
     * The compiled-in version constant, not
     * nxt_capability_linux_get_version(): the probe is a capget() call,
     * and this function has to work in the deployment where capget() is
     * exactly what is filtered.  On every
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
     * come from the capability model itself.
     *
     * A denial is not automatically a problem, though, which is why
     * nxt_capability_still_held() gets the last word: a root unitd has
     * already emptied every set by switching away from uid 0, and an
     * unprivileged one never held anything, so in both the call that
     * was just refused would have done nothing.  Only a process that
     * demonstrably still holds something -- or cannot find out -- has
     * anything to report.
     *
     * NXT_DECLINED rather than NXT_ERROR, and rather than NXT_OK: the
     * consequence of a denied capset() is not the same for every
     * caller, so this function reports "the sets are still full" and
     * lets nxt_process_apply_creds()'s callers decide.  A prototype
     * refuses to start, because its whole remaining job is to fork
     * application workers that would inherit those sets; the router,
     * controller and discovery processes warn and continue, because
     * they run no application code and killing them is worse than the
     * bug -- see nxt_process_created_ok() and nxt_process_core_setup().
     *
     * Anything else -- EINVAL, EFAULT -- means the call we just made is
     * malformed, which can only be a bug in Unit, since neither the
     * version nor the pointer depends on anything an operator controls.
     * Fail closed there, for every caller alike.
     */

    if (err == NXT_EPERM || err == NXT_ENOSYS) {
        held = nxt_capability_still_held();

        if (held == 0) {
            /*
             * The overwhelmingly common case, and the reason this is
             * measured rather than assumed.  A root unitd has already
             * cleared every set by switching away from uid 0, and an
             * ordinary unprivileged unitd never had anything -- in both
             * the call that was just denied would have been a no-op, and
             * refusing over it would take the application down for no
             * security benefit at all.  Not even a warning: nothing
             * happened.
             */

            nxt_debug(task, "capset() failed %E, and no capability is held",
                      err);

            return NXT_OK;
        }

        if (held > 0) {
            nxt_log(task, NXT_LOG_WARN, "capset() failed %E; the capabilities "
                    "this process holds could not be dropped", err);

        } else {
            nxt_log(task, NXT_LOG_WARN, "capset() failed %E, and whether this "
                    "process holds any capability could not be determined "
                    "either", err);
        }

        return NXT_DECLINED;
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
