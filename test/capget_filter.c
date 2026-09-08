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
}


__attribute__((constructor))
static void
nxt_shim_ctor(void)
{
    nxt_shim_init();
}


static void
nxt_shim_record(void)
{
    int   fd, len;
    char  buf[32];

    if (nxt_record_path == NULL) {
        return;
    }

    /*
     * open()/write()/close() are their own glibc stubs, not calls to
     * syscall(), so this cannot recurse back into the shim.
     */

    fd = open(nxt_record_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

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
        nxt_shim_record();
        errno = nxt_capget_errno;
        return -1;
    }
#endif

    return nxt_real_syscall(number, a0, a1, a2, a3, a4, a5);
}
