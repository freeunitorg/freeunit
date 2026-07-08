/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include <nxt_cgroup.h>
#include "nxt_tests.h"


nxt_int_t
nxt_cgroup_test(nxt_thread_t *thr)
{
    char    path[NXT_MAX_PATH_LEN];
    size_t  suffix_len, len;

    suffix_len = nxt_length("/cgroup.procs");

    /*
     * The final NUL also has to fit.  This is the last valid directory
     * length before appending "/cgroup.procs".
     */
    len = NXT_MAX_PATH_LEN - suffix_len - 1;

    memset(path, 'a', len);
    path[len] = '\0';

    if (nxt_cgroup_make_procs_path(path, len) != NXT_OK) {
        nxt_log_alert(thr->log, "nxt_cgroup_make_procs_path() rejected "
                      "last valid path length");
        return NXT_ERROR;
    }

    if (strlen(path) != NXT_MAX_PATH_LEN - 1) {
        nxt_log_alert(thr->log, "nxt_cgroup_make_procs_path() produced "
                      "unexpected path length");
        return NXT_ERROR;
    }

    /*
     * One byte longer leaves room for the suffix bytes but not the trailing
     * NUL; this used to pass because the check compared against the
     * overwritten snprintf() return value.
     */
    len = NXT_MAX_PATH_LEN - suffix_len;

    memset(path, 'a', len);
    path[len] = '\0';
    nxt_errno = 0;

    if (nxt_cgroup_make_procs_path(path, len) != NXT_ERROR) {
        nxt_log_alert(thr->log, "nxt_cgroup_make_procs_path() accepted "
                      "overflowing path length");
        return NXT_ERROR;
    }

    if (nxt_errno != ENAMETOOLONG) {
        nxt_log_alert(thr->log, "nxt_cgroup_make_procs_path() set errno %E, "
                      "expected ENAMETOOLONG", nxt_errno);
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_cgroup test passed");

    return NXT_OK;
}
