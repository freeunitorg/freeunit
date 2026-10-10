
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include "nxt_tests.h"


typedef struct {
    const char  *in;
    nxt_uint_t  cpus;
} nxt_cpu_limit_case_t;


typedef struct {
    const char  *in;
    const char  *out;
} nxt_cpu_limit_str_case_t;


typedef struct {
    const char  *cgroup;
    const char  *root;
    const char  *rel;
} nxt_cpu_limit_path_case_t;


/* "root" is NULL for a line that is not a cgroup2 mount. */

typedef struct {
    const char  *line;
    const char  *root;
    const char  *mnt;
} nxt_cpu_limit_mountinfo_case_t;


typedef struct {
    const char  *dir;
    nxt_uint_t  cpus;
} nxt_cpu_limit_level_t;


/*
 * "levels" gives the limit of each directory that has one, and ends with a
 * NULL "dir".  "visited" lists the directories that must be read, in order,
 * each followed by a space.
 */

typedef struct {
    const char             *mnt;
    const char             *rel;
    nxt_cpu_limit_level_t  levels[4];
    nxt_uint_t             cpus;
    const char             *visited;
} nxt_cpu_limit_walk_case_t;


typedef struct {
    const nxt_cpu_limit_walk_case_t  *test;
    size_t                           len;
    nxt_bool_t                       overflow;
    char                             visited[512];
} nxt_cpu_limit_walk_ctx_t;


static nxt_int_t nxt_cpu_limit_mountinfo_test(nxt_thread_t *thr);
static nxt_int_t nxt_cpu_limit_walk_test(nxt_thread_t *thr);
static nxt_uint_t nxt_cpu_limit_walk_read(const char *dir, void *data);


nxt_int_t
nxt_cpu_limit_test(nxt_thread_t *thr)
{
    nxt_uint_t  i, cpus;
    nxt_bool_t  ok;
    const char  *rel;
    char        buf[32];

    static const nxt_cpu_limit_case_t  cases[] = {
        { "max 100000\n",           0 },
        { "200000 100000\n",        2 },
        { "150000 100000\n",        2 },
        { "50000 100000\n",         1 },
        { "1000 1000000\n",         1 },
        { "6400000 100000",         64 },
        { "100000 0\n",             0 },
        { "0 100000\n",             0 },
        { "-1 100000\n",            0 },
        { "100000\n",               0 },
        { "100000 \n",              0 },
        { "\n",                     0 },
        { "",                       0 },
        { "1x0000 100000\n",        0 },
        { "9223372036854775807 1\n", 0 },
        { "100001 100000\n",        2 },
        { "2147483647 1\n",         2147483647 },
        { "2147483648 1\n",         0 },
        { "200000 100000 \n \n",    2 },
        { " 200000 100000\n",       0 },
        { "200000  100000\n",       0 },
        { "200000 100000 1\n",      0 },
        { "200000\n100000\n",       0 },
        { " \n",                    0 },
    };

    static const nxt_cpu_limit_str_case_t  unescape[] = {
        { "/sys/fs/cgroup",         "/sys/fs/cgroup" },
        { "/mnt/a\\040b",           "/mnt/a b" },
        { "\\011\\012\\134\\040",     "\t\n\\ " },
        { "/a\\04",                 "/a\\04" },
        { "/a\\0x0",                "/a\\0x0" },
        { "/a\\400",                "/a\\400" },
        { "/a\\",                   "/a\\" },
        { "",                       "" },
    };

    static const nxt_cpu_limit_path_case_t  paths[] = {
        { "/",              "/",    "" },
        { "/a/b",           "/",    "/a/b" },
        { "/a/b",           "/a",   "/b" },
        { "/a",             "/a",   "" },
        { "/ab",            "/a",   NULL },
        { "/b",             "/a",   NULL },
        { "/..",            "/",    NULL },
        { "/../b",          "/",    NULL },
        { "/a/..",          "/",    NULL },
        { "/a/../b",        "/",    NULL },
        { "/..a",           "/",    "/..a" },
        { "/a/..b/c",       "/",    "/a/..b/c" },
        { "/a..",           "/",    "/a.." },
        { "/a/b..c",        "/a",   "/b..c" },
    };

    for (i = 0; i < nxt_nitems(cases); i++) {
        cpus = nxt_cgroup_cpu_max_parse((const u_char *) cases[i].in,
                                        strlen(cases[i].in));

        NXT_TEST_CHECK(thr->log, cpus == cases[i].cpus,
                       "cpu.max \"%s\": %ui CPUs, expected %ui",
                       cases[i].in, cpus, cases[i].cpus);
    }

    for (i = 0; i < nxt_nitems(unescape); i++) {
        nxt_cpystrn((u_char *) buf, (const u_char *) unescape[i].in,
                    sizeof(buf));

        nxt_cgroup_mountinfo_unescape(buf);

        NXT_TEST_CHECK(thr->log, strcmp(buf, unescape[i].out) == 0,
                       "mountinfo \"%s\": \"%s\", expected \"%s\"",
                       unescape[i].in, buf, unescape[i].out);
    }

    for (i = 0; i < nxt_nitems(paths); i++) {
        rel = nxt_cgroup_relative_path(paths[i].cgroup, paths[i].root);

        ok = (rel == NULL || paths[i].rel == NULL)
             ? rel == paths[i].rel
             : strcmp(rel, paths[i].rel) == 0;

        NXT_TEST_CHECK(thr->log, ok,
                       "cgroup \"%s\" below \"%s\": \"%s\", expected \"%s\"",
                       paths[i].cgroup, paths[i].root,
                       (rel != NULL) ? rel : "NULL",
                       (paths[i].rel != NULL) ? paths[i].rel : "NULL");
    }

    if (nxt_cpu_limit_mountinfo_test(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_cpu_limit_walk_test(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "cpu limit test passed");

    return NXT_OK;
}


static nxt_int_t
nxt_cpu_limit_mountinfo_test(nxt_thread_t *thr)
{
    char        *root, *mnt;
    nxt_int_t   ret;
    nxt_bool_t  ok;
    nxt_uint_t  i;
    char        buf[256];
    char        unset[] = "unset";

    static const nxt_cpu_limit_mountinfo_case_t  cases[] = {

        /* Docker on cgroup v2: a private cgroup namespace. */
        { "1196 1187 0:28 / /sys/fs/cgroup ro,nosuid,nodev,noexec,relatime"
          " - cgroup2 cgroup rw,nsdelegate,memory_recursiveprot\n",
          "/", "/sys/fs/cgroup" },

        /* systemd on the host: one optional field. */
        { "35 24 0:30 / /sys/fs/cgroup rw,nosuid,nodev,noexec,relatime"
          " shared:9 - cgroup2 cgroup2 rw,nsdelegate,memory_recursiveprot\n",
          "/", "/sys/fs/cgroup" },

        /* Three optional fields; the host cgroup namespace. */
        { "612 590 0:30 /system.slice/docker-1f2e.scope /sys/fs/cgroup"
          " ro,nosuid,nodev,noexec,relatime master:9 propagate_from:2"
          " unbindable - cgroup2 cgroup rw,nsdelegate\n",
          "/system.slice/docker-1f2e.scope", "/sys/fs/cgroup" },

        /* A space, a tab and a backslash in the paths. */
        { "40 30 0:31 /a\\040b /mnt/cg\\040root\\011x rw,relatime"
          " - cgroup2 none rw\n",
          "/a b", "/mnt/cg root\tx" },

        { "41 30 0:31 /c\\134d /mnt/e rw - cgroup2 none rw",
          "/c\\d", "/mnt/e" },

        /* An escaped " - cgroup2 " in a path is not the separator. */
        { "50 30 0:40 / /mnt/a\\040-\\040cgroup2\\040b rw"
          " - tmpfs tmpfs rw\n",
          NULL, NULL },

        { "51 30 0:40 / /mnt/a\\040-\\040cgroup2\\040b rw"
          " - cgroup2 cgroup2 rw\n",
          "/", "/mnt/a - cgroup2 b" },

        /* Not cgroup2. */
        { "22 28 0:21 / /proc rw,nosuid,nodev,noexec,relatime shared:12"
          " - proc proc rw\n",
          NULL, NULL },

        { "30 25 0:26 / /sys/fs/cgroup/cpu,cpuacct rw,nosuid shared:12"
          " - cgroup cgroup rw,cpu,cpuacct\n",
          NULL, NULL },

        { "31 25 0:26 / /sys/fs/cgroup/cpu rw - cgroup cgroup2 rw,cpu\n",
          NULL, NULL },

        { "32 25 0:27 / /run/cgroup2 rw - tmpfs tmpfs rw\n",
          NULL, NULL },

        { "33 25 0:27 / /sys/fs/cgroup rw - cgroup2\n",
          NULL, NULL },

        /* Too few fields before the separator. */
        { "1 2 0:3 / - cgroup2 cgroup2 rw\n",      NULL, NULL },
        { "1 2 - cgroup2 cgroup2 rw\n",            NULL, NULL },
        { " - cgroup2 cgroup2 rw\n",               NULL, NULL },
        { "\n",                                    NULL, NULL },
        { "",                                       NULL, NULL },

        /* No options field: the mount point ends at the separator. */
        { "1 2 0:3 / /mnt - cgroup2 cgroup2 rw\n", "/", "/mnt" },

        /* An empty mount point.  The kernel never writes one. */
        { "1 2 0:3 /  rw - cgroup2 cgroup2 rw\n",  NULL, NULL },
        { "1 2 0:3 /  - cgroup2 cgroup2 rw\n",     NULL, NULL },

        /* A mount point that is empty after the unescape. */
        { "1 2 0:3 / \\000 rw - cgroup2 cgroup2 rw\n", NULL, NULL },

        /* Empty fields, as the code before the helper read them. */
        { "1 2 0:3  /mnt rw - cgroup2 cgroup2 rw\n", "", "/mnt" },
    };

    for (i = 0; i < nxt_nitems(cases); i++) {

        NXT_TEST_CHECK(thr->log, strlen(cases[i].line) < sizeof(buf),
                       "mountinfo case %ui is too long", i);

        nxt_cpystrn((u_char *) buf, (const u_char *) cases[i].line,
                    sizeof(buf));

        root = unset;
        mnt = unset;

        ret = nxt_cgroup_mountinfo_parse(buf, &root, &mnt);

        if (cases[i].root == NULL) {
            ok = (ret == NXT_DECLINED && root == unset && mnt == unset);

        } else {
            ok = (ret == NXT_OK
                  && strcmp(root, cases[i].root) == 0
                  && strcmp(mnt, cases[i].mnt) == 0);
        }

        NXT_TEST_CHECK(thr->log, ok,
                       "mountinfo line \"%s\": %i, root \"%s\", "
                       "mount point \"%s\", expected root \"%s\", "
                       "mount point \"%s\"",
                       cases[i].line, ret, root, mnt,
                       (cases[i].root != NULL) ? cases[i].root : "NULL",
                       (cases[i].mnt != NULL) ? cases[i].mnt : "NULL");
    }

    return NXT_OK;
}


static nxt_int_t
nxt_cpu_limit_walk_test(nxt_thread_t *thr)
{
    int                       len;
    nxt_uint_t                i, cpus;
    nxt_cpu_limit_walk_ctx_t  ctx;
    char                      dir[256];

#define CG  "/sys/fs/cgroup"
#define POD CG "/kubepods.slice/pod1"
#define CTR POD "/cri-1"

    static const nxt_cpu_limit_walk_case_t  cases[] = {

        /* A limit on the cgroup of the process only. */
        { CG, "/kubepods.slice/pod1/cri-1",
          { { CTR, 2 }, { NULL, 0 } },
          2, CTR " " POD " " CG "/kubepods.slice " CG " " },

        /* A limit on a parent only. */
        { CG, "/kubepods.slice/pod1/cri-1",
          { { POD, 4 }, { NULL, 0 } },
          4, CTR " " POD " " CG "/kubepods.slice " CG " " },

        /* Limits on both: the lower one, wherever it is. */
        { CG, "/kubepods.slice/pod1/cri-1",
          { { CTR, 8 }, { POD, 3 }, { NULL, 0 } },
          3, CTR " " POD " " CG "/kubepods.slice " CG " " },

        { CG, "/kubepods.slice/pod1/cri-1",
          { { CTR, 2 }, { POD, 6 }, { CG "/kubepods.slice", 5 },
            { NULL, 0 } },
          2, CTR " " POD " " CG "/kubepods.slice " CG " " },

        /* "max" at every level: cpu.max reads as 0. */
        { CG, "/kubepods.slice/pod1/cri-1",
          { { NULL, 0 } },
          0, CTR " " POD " " CG "/kubepods.slice " CG " " },

        /*
         * A private cgroup namespace: the cgroup of the process is the
         * root of the mount, and it is read.
         */
        { CG, "",
          { { CG, 2 }, { NULL, 0 } },
          2, CG " " },

        /* The walk stops at the mount point and reads nothing above it. */
        { CG, "/a",
          { { "/sys/fs", 1 }, { "/sys", 1 }, { "/", 1 }, { NULL, 0 } },
          0, CG "/a " CG " " },

        { CG, "/a",
          { { CG, 7 }, { "/sys/fs", 1 }, { NULL, 0 } },
          7, CG "/a " CG " " },

        /* A mount point with a space, after the escapes are decoded. */
        { "/mnt/a b", "/x/y",
          { { "/mnt/a b/x", 3 }, { NULL, 0 } },
          3, "/mnt/a b/x/y /mnt/a b/x /mnt/a b " },
    };

#undef CTR
#undef POD
#undef CG

    for (i = 0; i < nxt_nitems(cases); i++) {
        len = snprintf(dir, sizeof(dir), "%s%s", cases[i].mnt, cases[i].rel);

        NXT_TEST_CHECK(thr->log, len > 0 && (size_t) len < sizeof(dir),
                       "walk case %ui is too long", i);

        nxt_memzero(&ctx, sizeof(ctx));
        ctx.test = &cases[i];

        cpus = nxt_cgroup_cpu_limit_walk(dir, strlen(cases[i].mnt),
                                         nxt_cpu_limit_walk_read, &ctx);

        NXT_TEST_CHECK(thr->log,
                       cpus == cases[i].cpus && !ctx.overflow
                       && strcmp(ctx.visited, cases[i].visited) == 0,
                       "walk from \"%s%s\": %ui CPUs, read \"%s\", "
                       "expected %ui CPUs, read \"%s\"",
                       cases[i].mnt, cases[i].rel, cpus, ctx.visited,
                       cases[i].cpus, cases[i].visited);
    }

    return NXT_OK;
}


static nxt_uint_t
nxt_cpu_limit_walk_read(const char *dir, void *data)
{
    int                          n;
    nxt_uint_t                   i;
    nxt_cpu_limit_walk_ctx_t     *ctx;
    const nxt_cpu_limit_level_t  *level;

    ctx = data;

    n = snprintf(ctx->visited + ctx->len, sizeof(ctx->visited) - ctx->len,
                 "%s ", dir);

    if (n < 0 || (size_t) n >= sizeof(ctx->visited) - ctx->len) {
        ctx->overflow = 1;

    } else {
        ctx->len += n;
    }

    for (i = 0; i < nxt_nitems(ctx->test->levels); i++) {
        level = &ctx->test->levels[i];

        if (level->dir == NULL) {
            break;
        }

        if (strcmp(level->dir, dir) == 0) {
            return level->cpus;
        }
    }

    return 0;
}
