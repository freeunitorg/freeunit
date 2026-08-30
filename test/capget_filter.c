/*
 * LD_PRELOAD shim that makes capget(2) fail the way a syscall filter
 * makes it fail, for test_capget_fallback.py.
 *
 * src/nxt_capability.c reaches the kernel through the nxt_capget()
 * macro, which expands to glibc's syscall() wrapper -- an ordinary
 * interposable symbol, not a compiler builtin or an inline "syscall"
 * instruction.  So a preloaded definition of syscall() shadows it and
 * we can inject an errno without needing seccomp, a privileged
 * helper, or a container.
 *
 * The interposition is total: every syscall() in the process, from
 * every library, comes through here.  Only SYS_capget is ever
 * answered locally, and only when NXT_TEST_CAPGET_ERRNO names an
 * errno; everything else is forwarded to the real symbol untouched.
 *
 * Set NXT_TEST_CAPGET_RECORD to a file path to have every pid whose
 * capget() we failed appended to it, one per line.  The test uses
 * that to show which processes the filter actually reached -- the
 * environment is inherited by unitd's children, so "no other process
 * was affected" has to be demonstrated rather than assumed.
 *
 * That path is copied, not kept as the getenv() pointer, and the copy
 * is what makes the demonstration mean anything.  getenv() returns a
 * pointer into the contiguous argv+environ block, and
 * nxt_process_title() (src/nxt_process_title.c) writes the process
 * title over that block and zero-pads the remainder, so in every
 * process unitd forks the inherited pointer would read as "" and
 * open("") would fail silently.  The main process looked correct only
 * by accident of ordering: nxt_capability_set() runs from
 * nxt_runtime_conf_init(), before nxt_main_process_title()
 * (src/nxt_main_process.c:96) rewrites the area.  A record file that
 * no child could have written to proves nothing about what the
 * children did.
 *
 * NXT_TEST_CAPSET_RECORD does the same for capset(2), which the shim
 * never fails -- it only writes down who called it and forwards the
 * call.  That is how the test observes nxt_capability_drop(): the drop
 * has no visible effect in a suite that runs without any capability to
 * drop, so "the app has no capabilities" would pass just as well
 * without the code.  Which *processes* called capset() is the part
 * that cannot pass by accident: main must never appear (it keeps
 * CAP_NET_BIND_SERVICE for bind()), the prototype must, and the
 * application worker must not, because it inherits the prototype's
 * already-emptied sets rather than dropping again.
 *
 * Build:
 *     cc -shared -fPIC -o capget_filter.so capget_filter.c -ldl
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * glibc declares syscall() variadic, but it is an assembly stub that
 * only moves registers: it never walks a va_list, so calling it
 * through a fixed six-argument prototype is what every syscall
 * interposer does and is correct on every Linux ABI Unit builds for.
 */
typedef long (*nxt_syscall_fn)(long, long, long, long, long, long, long);

static nxt_syscall_fn  nxt_real_syscall;
static int             nxt_capget_errno;   /* 0: filter disabled */
static char           *nxt_record_path;    /* strdup()ed, see above */
static char           *nxt_capset_record_path;


static void
nxt_shim_init(void)
{
    const char  *value;

    nxt_real_syscall = (nxt_syscall_fn) dlsym(RTLD_NEXT, "syscall");

    value = getenv("NXT_TEST_CAPGET_ERRNO");

    if (value != NULL && *value != '\0') {
        nxt_capget_errno = (int) strtol(value, NULL, 10);
    }

    value = getenv("NXT_TEST_CAPGET_RECORD");

    if (nxt_record_path == NULL && value != NULL && *value != '\0') {
        nxt_record_path = strdup(value);
    }

    value = getenv("NXT_TEST_CAPSET_RECORD");

    if (nxt_capset_record_path == NULL && value != NULL && *value != '\0') {
        nxt_capset_record_path = strdup(value);
    }
}


__attribute__((constructor))
static void
nxt_shim_ctor(void)
{
    nxt_shim_init();
}


static void
nxt_shim_record(const char *path)
{
    int   fd, len;
    char  buf[32];

    if (path == NULL) {
        return;
    }

    /*
     * open()/write()/close() are their own glibc stubs, not calls to
     * syscall(), so this cannot recurse back into the shim.
     */

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd < 0) {
        return;
    }

    len = snprintf(buf, sizeof(buf), "%ld\n", (long) getpid());

    if (len > 0) {
        (void) !write(fd, buf, (size_t) len);
    }

    close(fd);
}


long
syscall(long number, ...)
{
    long     a0, a1, a2, a3, a4, a5;
    va_list  ap;

    if (nxt_real_syscall == NULL) {
        /*
         * A constructor in another preloaded object may run before
         * ours and call syscall() from it.
         */
        nxt_shim_init();

        if (nxt_real_syscall == NULL) {
            /*
             * Nothing to forward to.  Fail loudly: silently returning
             * an error here would corrupt an unrelated syscall and
             * make the test lie about what it exercised.
             */
            _exit(127);
        }
    }

    /*
     * Reading six arguments when the caller passed fewer yields
     * garbage in the unused ones, which the kernel ignores for every
     * syscall that does not declare them.
     *
     * ISO C calls that undefined (C17 7.16.1.1p2: va_arg with no
     * actual next argument), and there is no portable fix -- a
     * variadic forwarder cannot learn its own argument count, and
     * re-declaring syscall() with a fixed prototype only trades this
     * undefined behaviour for calling a function through an
     * incompatible type.  It is left as-is deliberately, because the
     * concrete behaviour is the one every implementation already
     * depends on:
     *
     *   - glibc's syscall() is an assembly stub that unconditionally
     *     moves six argument registers, and musl's is a fixed
     *     seven-parameter C function.  Neither ever walks a va_list,
     *     so both read exactly the slots this reads.
     *   - The slots read are always mapped memory, on every ABI Unit
     *     builds for.  On x86-64 SysV the first five come from the
     *     register save area gcc/clang spill at entry and the sixth
     *     from the caller's own frame via overflow_arg_area; on
     *     aarch64 all seven fit the eight-register save area; on
     *     32-bit ABIs the excess reads land in the caller's frame.
     *     None of them can fault, and none of them is used: only
     *     `number` decides what happens below.
     *
     * Whether the shim should install a real seccomp filter instead
     * was weighed and rejected: a filter needs PR_SET_NO_NEW_PRIVS,
     * after which it is inherited across both fork() and execve(), so
     * every unitd child would see capget() fail too -- and the record
     * file below, whose whole value is showing that only main was
     * affected, could no longer say anything.  It would also filter
     * capset() in the children, which is the call the drop depends on.
     */

    va_start(ap, number);
    a0 = va_arg(ap, long);
    a1 = va_arg(ap, long);
    a2 = va_arg(ap, long);
    a3 = va_arg(ap, long);
    a4 = va_arg(ap, long);
    a5 = va_arg(ap, long);
    va_end(ap);

#ifdef SYS_capget
    if (number == SYS_capget && nxt_capget_errno != 0) {
        nxt_shim_record(nxt_record_path);
        errno = nxt_capget_errno;
        return -1;
    }
#endif

#ifdef SYS_capset
    if (number == SYS_capset) {
        /*
         * Recorded, never failed: the test needs to know which
         * processes drop their capabilities, and a failed capset()
         * would be a different test.
         */
        nxt_shim_record(nxt_capset_record_path);
    }
#endif

    return nxt_real_syscall(number, a0, a1, a2, a3, a4, a5);
}
