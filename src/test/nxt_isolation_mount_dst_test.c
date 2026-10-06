/*
 * Copyright (C) FreeUnit
 */

/*
 * The mount destination walk in src/nxt_isolation.c has two branches.  With
 * openat2() it resolves each prefix beneath the rootfs.  Without openat2()
 * it opens each component with O_NOFOLLOW and refuses a symlink.  Every CI
 * kernel has openat2(), so the Python isolation tests run only the first
 * branch.  This test runs each case twice: once as unitd runs it, and once
 * with the second branch forced through nxt_isolation_test_mount_dst_open().
 *
 * The walk needs no privileges, so the test runs as any user.
 *
 * The rootfs of each case:
 *
 *   decoy/                     outside the rootfs, must stay empty
 *   r/usr -> ../decoy          a link out of the rootfs
 *   r/a/esc -> ../../decoy     the same link one level down
 *   r/real/lib/
 *   r/lib -> real/lib          a link inside the rootfs
 */

#include <nxt_main.h>
#include <nxt_application.h>
#include <nxt_process.h>
#include <nxt_isolation.h>
#include "nxt_tests.h"

#include <dirent.h>
#include <ftw.h>


typedef struct {
    const char  *name;
    const char  *rel;
    /* The directory that the walk opens, relative to the rootfs. */
    const char  *opened;
    /* The component that the walk refuses. */
    const char  *comp;
    /* Only openat2() resolves the destination. */
    nxt_bool_t  openat2_only;
} nxt_isolation_mount_dst_case_t;


typedef struct {
    const nxt_isolation_mount_dst_case_t  *tc;
    nxt_bool_t                            fallback;
    nxt_bool_t                            have_openat2;
    nxt_log_t                             *log;
    const char                            *tmpdir;
    u_char                                msg[NXT_MAX_ERROR_STR];
} nxt_isolation_mount_dst_ctx_t;


static const nxt_isolation_mount_dst_case_t  nxt_mount_dst_cases[] = {
    { "symlink out of rootfs", "usr/lib/python3.12", NULL, "usr", 0 },
    { "symlink out of rootfs below a directory", "a/esc/lib", NULL, "esc",
      0 },
    { "symlink inside rootfs", "lib/x", "real/lib/x", "lib", 1 },
    { "no symlink", "a/b/c", "a/b/c", NULL, 0 },
};


static void nxt_cdecl
nxt_isolation_mount_dst_test_log_handler(nxt_uint_t level, nxt_log_t *log,
    const char *fmt, ...)
{
    u_char                         *p, *end;
    va_list                        args;
    nxt_isolation_mount_dst_ctx_t  *ctx;

    if (level != NXT_LOG_ALERT) {
        return;
    }

    ctx = log->ctx;

    p = ctx->msg;
    end = ctx->msg + sizeof(ctx->msg) - 1;

    va_start(args, fmt);
    p = nxt_vsprintf(p, end, fmt, args);
    va_end(args);

    *p = '\0';
}


static int
nxt_isolation_mount_dst_test_remove(const char *path, const struct stat *st,
    int flag, struct FTW *ftw)
{
    (void) remove(path);

    return 0;
}


static nxt_bool_t
nxt_isolation_mount_dst_test_empty(int dir_fd, const char *name)
{
    int            fd;
    DIR            *dir;
    nxt_bool_t     empty;
    struct dirent  *de;

    fd = openat(dir_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd == -1) {
        return 0;
    }

    dir = fdopendir(fd);
    if (dir == NULL) {
        close(fd);
        return 0;
    }

    empty = 1;

    while ((de = readdir(dir)) != NULL) {
        if (nxt_strcmp(de->d_name, ".") != 0
            && nxt_strcmp(de->d_name, "..") != 0)
        {
            empty = 0;
        }
    }

    closedir(dir);

    return empty;
}


static nxt_int_t
nxt_isolation_mount_dst_test_setup(int tmp_fd)
{
    if (mkdirat(tmp_fd, "decoy", 0777) != 0
        || mkdirat(tmp_fd, "r", 0777) != 0
        || mkdirat(tmp_fd, "r/a", 0777) != 0
        || mkdirat(tmp_fd, "r/real", 0777) != 0
        || mkdirat(tmp_fd, "r/real/lib", 0777) != 0
        || symlinkat("../decoy", tmp_fd, "r/usr") != 0
        || symlinkat("../../decoy", tmp_fd, "r/a/esc") != 0
        || symlinkat("real/lib", tmp_fd, "r/lib") != 0)
    {
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_isolation_mount_dst_test_run(nxt_isolation_mount_dst_ctx_t *ctx,
    int tmp_fd)
{
    int                                   fd, rootfs_fd;
    nxt_int_t                             ret;
    nxt_log_t                             log;
    nxt_bool_t                            openat2_used, use_openat2, refused;
    nxt_task_t                            task;
    struct stat                           st, want;
    const char                            *mode;
    const nxt_isolation_mount_dst_case_t  *tc;

    tc = ctx->tc;
    mode = ctx->fallback ? "fallback" : "default";

    NXT_TEST_CHECK(ctx->log,
                   nxt_isolation_mount_dst_test_setup(tmp_fd) == NXT_OK,
                   "mount dst test: setup in \"%s\" failed %E", ctx->tmpdir,
                   nxt_errno);

    rootfs_fd = openat(tmp_fd, "r", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    NXT_TEST_CHECK(ctx->log, rootfs_fd != -1,
                   "mount dst test: open rootfs failed %E", nxt_errno);

    log.level = NXT_LOG_INFO;
    log.ident = 0;
    log.handler = nxt_isolation_mount_dst_test_log_handler;
    log.ctx_handler = NULL;
    log.ctx = ctx;

    nxt_memzero(&task, sizeof(task));
    task.thread = nxt_thread();
    task.log = &log;

    fd = -1;

    ret = nxt_isolation_test_mount_dst_open(&task, rootfs_fd, tc->rel,
                                            ctx->fallback, &openat2_used, &fd);

    use_openat2 = !ctx->fallback && ctx->have_openat2;
    refused = (tc->comp != NULL && (!tc->openat2_only || !use_openat2));

    NXT_TEST_CHECK(ctx->log, nxt_isolation_mount_dst_test_empty(tmp_fd,
                                                                "decoy"),
                   "mount dst test: %s, %s: a directory was created "
                   "outside the rootfs", tc->name, mode);

    if (refused) {
        NXT_TEST_CHECK(ctx->log, ret == NXT_ERROR,
                       "mount dst test: %s, %s: \"%s\" is not refused",
                       tc->name, mode, tc->rel);

        nxt_log_error(NXT_LOG_NOTICE, ctx->log, "mount dst test: %s, %s: %s",
                      tc->name, mode, ctx->msg);

        /*
         * Each branch has its own message.  Only Linux is checked: for a
         * symlink opened with O_NOFOLLOW, FreeBSD returns EMLINK, and the
         * fallback logs the errno instead.
         */
#if (NXT_LINUX)
        {
            u_char  want_msg[128];

            (void) nxt_sprintf(want_msg, want_msg + sizeof(want_msg),
                               "component \"%s\" %s%Z", tc->comp,
                               use_openat2
                               ? "escapes rootfs or is not a directory: "
                               : "is a symlink or not a directory");

            NXT_TEST_CHECK(ctx->log,
                           strstr((char *) ctx->msg, (char *) want_msg)
                           != NULL,
                           "mount dst test: %s, %s: the log is \"%s\", "
                           "wanted \"%s\"", tc->name, mode, ctx->msg,
                           want_msg);
        }
#endif

        if (tc->opened != NULL) {
            NXT_TEST_CHECK(ctx->log,
                           fstatat(rootfs_fd, tc->opened, &st, 0) != 0,
                           "mount dst test: %s, %s: \"%s\" was created",
                           tc->name, mode, tc->opened);
        }

    } else {
        NXT_TEST_CHECK(ctx->log, ret == NXT_OK,
                       "mount dst test: %s, %s: \"%s\" is refused: %s",
                       tc->name, mode, tc->rel, ctx->msg);

        NXT_TEST_CHECK(ctx->log,
                       fstat(fd, &st) == 0
                       && fstatat(rootfs_fd, tc->opened, &want, 0) == 0
                       && st.st_dev == want.st_dev
                       && st.st_ino == want.st_ino,
                       "mount dst test: %s, %s: the walk did not open "
                       "\"%s\"", tc->name, mode, tc->opened);

        close(fd);
    }

    NXT_TEST_CHECK(ctx->log, openat2_used == use_openat2,
                   "mount dst test: %s, %s: openat2 used %d, wanted %d",
                   tc->name, mode, (int) openat2_used, (int) use_openat2);

    close(rootfs_fd);

    return NXT_OK;
}


static int
nxt_isolation_mount_dst_test_child(void *data)
{
    int                            tmp_fd;
    char                           dir[256];
    nxt_int_t                      ret;
    nxt_isolation_mount_dst_ctx_t  *ctx;

    ctx = data;

    (void) nxt_sprintf((u_char *) dir, (u_char *) dir + sizeof(dir),
                       "%s/nxt_isolation_mount_dst_test.XXXXXX%Z",
                       ctx->tmpdir);

    if (mkdtemp(dir) == NULL) {
        nxt_log_alert(ctx->log, "mount dst test: mkdtemp(\"%s\") failed %E",
                      dir, nxt_errno);
        return 1;
    }

    tmp_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (tmp_fd == -1) {
        nxt_log_alert(ctx->log, "mount dst test: open(\"%s\") failed %E",
                      dir, nxt_errno);
        ret = NXT_ERROR;

    } else {
        ret = nxt_isolation_mount_dst_test_run(ctx, tmp_fd);
        close(tmp_fd);
    }

    (void) nftw(dir, nxt_isolation_mount_dst_test_remove, 16,
                FTW_DEPTH | FTW_PHYS);

    return (ret == NXT_OK) ? 0 : 1;
}


#if (NXT_HAVE_OPENAT2)

static nxt_bool_t
nxt_isolation_mount_dst_test_have_openat2(void)
{
    int              fd;
    struct open_how  how;

    nxt_memzero(&how, sizeof(how));
    how.flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;

    fd = syscall(SYS_openat2, AT_FDCWD, ".", &how, sizeof(how));
    if (fd == -1) {
        return 0;
    }

    close(fd);

    return 1;
}

#endif


nxt_int_t
nxt_isolation_mount_dst_test(nxt_thread_t *thr)
{
    nxt_uint_t                     i, fallback;
    nxt_isolation_mount_dst_ctx_t  ctx;

    nxt_thread_time_update(thr);

    nxt_memzero(&ctx, sizeof(ctx));

    ctx.log = thr->log;

    ctx.tmpdir = getenv("TMPDIR");
    if (ctx.tmpdir == NULL || ctx.tmpdir[0] != '/') {
        ctx.tmpdir = "/tmp";
    }

#if (NXT_HAVE_OPENAT2)
    ctx.have_openat2 = nxt_isolation_mount_dst_test_have_openat2();
#endif

    if (!ctx.have_openat2) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "mount dst test: openat2() "
                      "is not available, both runs take the fallback");
    }

    for (fallback = 0; fallback <= 1; fallback++) {
        for (i = 0; i < nxt_nitems(nxt_mount_dst_cases); i++) {
            ctx.tc = &nxt_mount_dst_cases[i];
            ctx.fallback = fallback;

            if (nxt_test_in_child(thr, ctx.tc->name,
                                  nxt_isolation_mount_dst_test_child, &ctx)
                != 0)
            {
                nxt_log_alert(thr->log, "mount dst test: \"%s\" failed "
                              "with the %s branch", ctx.tc->name,
                              fallback ? "fallback" : "default");
                return NXT_ERROR;
            }
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "mount destination test passed");

    return NXT_OK;
}
