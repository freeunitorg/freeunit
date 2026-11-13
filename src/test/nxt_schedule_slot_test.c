
/*
 * Copyright (C) FreeUnit Community
 */

/*
 * The slots of a schedule (src/nxt_schedule.h, nxt_schedule_slot_next()).
 *
 * A slot starts at a multiple of "interval" seconds since the Epoch, so
 * the slots of a schedule are the same after a new configuration, a new
 * router or a new unitd.  The router arms a timer for the wait from now to
 * the next slot, and a timer cannot wait 2^31 ms or longer.
 *
 * Cases:
 *
 *   - The next slot is a multiple of the interval, later than now and not
 *     more than one interval away, for intervals from 1 s to the maximum
 *     and for times around a slot.
 *   - A time that is exactly the start of a slot gives the slot after it:
 *     that slot is the one that is due.
 *   - The wait to the next slot of the largest interval fits a timer.
 *   - A time before the Epoch gives the slot at the Epoch.
 */

#include <nxt_main.h>
#include <nxt_schedule.h>
#include "nxt_tests.h"


static nxt_int_t
nxt_schedule_slot_test_one(nxt_thread_t *thr, int64_t now, uint32_t interval)
{
    int64_t  next, step;

    step = (int64_t) interval * 1000;

    next = nxt_schedule_slot_next(now, interval);

    if (next % step != 0) {
        nxt_log_alert(thr->log, "schedule slot test: the slot %L after %L "
                      "is not a multiple of %uD s", next, now, interval);
        return NXT_ERROR;
    }

    if (next <= now) {
        nxt_log_alert(thr->log, "schedule slot test: the slot %L is not "
                      "after %L (every %uD s)", next, now, interval);
        return NXT_ERROR;
    }

    if (next - now > step) {
        nxt_log_alert(thr->log, "schedule slot test: the slot %L is more "
                      "than %uD s after %L", next, interval, now);
        return NXT_ERROR;
    }

    /* A timer compares nxt_msec_t values as a signed 32-bit difference. */

    if (nxt_msec_diff((nxt_msec_t) (next - now), 0) <= 0) {
        nxt_log_alert(thr->log, "schedule slot test: the wait of %L ms "
                      "(every %uD s) does not fit a timer", next - now,
                      interval);
        return NXT_ERROR;
    }

    return NXT_OK;
}


nxt_int_t
nxt_schedule_slot_test(nxt_thread_t *thr)
{
    int64_t     base, step, now;
    nxt_uint_t  i, j, k;

    static const uint32_t  intervals[] = {
        1, 2, 3, 7, 60, 300, 3600, 86400, 604800, 2147482,
        NXT_SCHEDULE_INTERVAL_MAX,
    };

    /* Around the Epoch, in 2001, in 2026, and past 2038. */
    static const int64_t  times[] = {
        0, 1, 999, 1000, 1001,
        1000000000000, 1794606026123, 2147483648000, 4102444800000,
    };

    static const int64_t  offsets[] = { -1001, -1000, -999, -1, 0, 1, 999 };

    nxt_thread_time_update(thr);

    for (i = 0; i < nxt_nitems(intervals); i++) {
        step = (int64_t) intervals[i] * 1000;

        for (j = 0; j < nxt_nitems(times); j++) {

            /* The time itself, and times around the slot that follows it. */

            if (nxt_schedule_slot_test_one(thr, times[j], intervals[i])
                != NXT_OK)
            {
                return NXT_ERROR;
            }

            base = (times[j] / step + 1) * step;

            for (k = 0; k < nxt_nitems(offsets); k++) {
                now = base + offsets[k];

                if (now < 0) {
                    continue;
                }

                if (nxt_schedule_slot_test_one(thr, now, intervals[i])
                    != NXT_OK)
                {
                    return NXT_ERROR;
                }
            }

            /* The start of a slot is not its own next slot. */

            if (nxt_schedule_slot_next(base, intervals[i]) != base + step) {
                nxt_log_alert(thr->log, "schedule slot test: the slot after "
                              "the start %L of a slot is %L, not %L", base,
                              nxt_schedule_slot_next(base, intervals[i]),
                              base + step);
                return NXT_ERROR;
            }

            /* One millisecond before a slot, that slot is next. */

            if (nxt_schedule_slot_next(base - 1, intervals[i]) != base) {
                nxt_log_alert(thr->log, "schedule slot test: the slot after "
                              "%L is %L, not %L", base - 1,
                              nxt_schedule_slot_next(base - 1, intervals[i]),
                              base);
                return NXT_ERROR;
            }
        }
    }

    /* The longest wait there is: the largest interval, from a slot start. */

    step = (int64_t) NXT_SCHEDULE_INTERVAL_MAX * 1000;

    if (nxt_schedule_slot_next(step, NXT_SCHEDULE_INTERVAL_MAX) - step != step
        || step > 2147483647)
    {
        nxt_log_alert(thr->log, "schedule slot test: the largest wait is "
                      "not %L ms, or it does not fit a timer", step);
        return NXT_ERROR;
    }

    if (nxt_schedule_slot_next(-1, 60) != 0
        || nxt_schedule_slot_next(-86400000, 60) != 0)
    {
        nxt_log_alert(thr->log, "schedule slot test: a time before the "
                      "Epoch does not give the slot at the Epoch");
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "schedule slot test passed");

    return NXT_OK;
}
