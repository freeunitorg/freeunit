
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

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "cpu limit test passed");

    return NXT_OK;
}
