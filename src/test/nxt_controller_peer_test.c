/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include "nxt_tests.h"


#define NONE_UID  ((nxt_uid_t) -1)
#define NONE_GID  ((nxt_gid_t) -1)


typedef struct {
    nxt_uid_t         uid;
    nxt_gid_t         gid;
    const nxt_gid_t   *groups;
    nxt_uint_t        ngroups;
    nxt_uid_t         euid;
    nxt_uid_t         ctl_uid;
    nxt_gid_t         ctl_gid;
    nxt_bool_t        allowed;
} nxt_controller_peer_test_t;


static const nxt_gid_t  groups_with_ctl[] = { 10, 20, 500 };
static const nxt_gid_t  groups_without[] = { 10, 20 };


static const nxt_controller_peer_test_t  tests[] = {
    /* root and unitd's own euid are always allowed */
    { 0, 0, NULL, 0, 1000, NONE_UID, NONE_GID, 1 },
    { 1000, 1000, NULL, 0, 1000, NONE_UID, NONE_GID, 1 },

    /* unrelated uid without delegation is rejected */
    { 1001, 1001, NULL, 0, 0, NONE_UID, NONE_GID, 0 },

    /* --control-user alice (uid 1001) with unitd running as root */
    { 1001, 1001, NULL, 0, 0, 1001, NONE_GID, 1 },
    { 1002, 1002, NULL, 0, 0, 1001, NONE_GID, 0 },

    /* --control-group (gid 500): primary gid match */
    { 1002, 500, NULL, 0, 0, NONE_UID, 500, 1 },

    /* --control-group: supplementary group match */
    { 1002, 1002, groups_with_ctl, 3, 0, NONE_UID, 500, 1 },
    { 1002, 1002, groups_without, 2, 0, NONE_UID, 500, 0 },

    /* supplementary groups are ignored without --control-group */
    { 1002, 1002, groups_with_ctl, 3, 0, NONE_UID, NONE_GID, 0 },
};


nxt_int_t
nxt_controller_peer_test(nxt_thread_t *thr)
{
    nxt_uint_t                        i;
    nxt_bool_t                        allowed;
    const nxt_controller_peer_test_t  *t;

    for (i = 0; i < nxt_nitems(tests); i++) {
        t = &tests[i];

        allowed = nxt_controller_peer_allowed(t->uid, t->gid, t->groups,
                                              t->ngroups, t->euid,
                                              t->ctl_uid, t->ctl_gid);

        if (allowed != t->allowed) {
            nxt_log_alert(thr->log, "nxt_controller_peer_allowed() test #%ui "
                          "failed: got %d, expected %d",
                          i, (int) allowed, (int) t->allowed);
            return NXT_ERROR;
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "nxt_controller_peer_allowed() test passed");

    return NXT_OK;
}
