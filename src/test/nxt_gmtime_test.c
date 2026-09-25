
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include "nxt_tests.h"


#if (NXT_TIME_T_SIZE == 4)

/* A 86400-fold number below 2^31. */
#define NXT_GMTIME_MAX  2147472000

#else
/*
 * March 19, 29398 is maximum valid data if nxt_uint_t
 * is 4 bytes size whilst nxt_time_t is 8 bytes size.
 */
#define NXT_GMTIME_MAX  865550793600
#endif


/*
 * Pre-epoch expectations, checked against fixed calendar fields rather than
 * against gmtime().  Do not "simplify" these into the differential loops
 * below: handing a nxt_time_t to gmtime(), which takes a time_t, is only
 * safe where the two types agree, and for a negative value they need not.
 * Where nxt_time_t is 8 bytes against a 4-byte time_t -- QNX before SDP
 * 8.0, whose native time_t is uint32_t, and the case the NXT_TIME_T_SIZE
 * test above already contemplates -- the library reads half the object,
 * the low half on a little-endian machine, which often looks right by
 * accident, and the high half on a big-endian one, which is garbage.
 * Fixed fields test the arithmetic on every platform instead.
 *
 * For the same reason the loops below never hand gmtime() the address of
 * the nxt_time_t itself: they convert into a real time_t first, and skip
 * the values that do not survive the conversion.
 *
 * A file's mtime can be negative ("touch -d 1969-07-20"), and the day-time
 * step used to wrap and render an hour of 1193046.  -432000 and beyond
 * matter on their own: a weekday taken from an unsigned day number is still
 * right for the first four days before the epoch and wrong after them.
 *
 * Rows derived with "date -u -d @<seconds>" on a signed 64-bit time_t host,
 * never from nxt_gmtime() itself.
 */

static const struct {
    nxt_time_t  time;
    int         year, mon, mday, wday, yday, hour, min, sec;
} nxt_gmtime_pre_epoch[] = {
    { -1,          69, 11, 31, 3, 364, 23, 59, 59 },  /* Wed, 31 Dec 1969 */
    { -86400,      69, 11, 31, 3, 364,  0,  0,  0 },  /* Wed, 31 Dec 1969 */
    { -432000,     69, 11, 27, 6, 360,  0,  0,  0 },  /* Sat, 27 Dec 1969 */
    { -432001,     69, 11, 26, 5, 359, 23, 59, 59 },  /* Fri, 26 Dec 1969 */
    { -14182940,   69,  6, 20, 0, 200, 20, 17, 40 },  /* Sun, 20 Jul 1969 */
    { -2147472000,  1, 11, 14, 6, 347,  0,  0,  0 },  /* Sat, 14 Dec 1901 */
#if (NXT_TIME_T_SIZE == 8)
    /* The oldest date the Gauss' formula supports. */
    { -62135596800, -1899, 0, 1, 1, 0, 0, 0, 0 },     /* Mon, 01 Jan 0001 */
#endif
};


nxt_int_t
nxt_gmtime_test(nxt_thread_t *thr)
{
    time_t      native;
    struct tm   tm0, *tm1;
    nxt_uint_t  i;
    nxt_time_t  s;
    nxt_nsec_t  start, end;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "gmtime test started");

    for (i = 0; i < nxt_nitems(nxt_gmtime_pre_epoch); i++) {

        s = nxt_gmtime_pre_epoch[i].time;

        nxt_gmtime(s, &tm0);

        if (tm0.tm_year != nxt_gmtime_pre_epoch[i].year
            || tm0.tm_mon != nxt_gmtime_pre_epoch[i].mon
            || tm0.tm_mday != nxt_gmtime_pre_epoch[i].mday
            || tm0.tm_wday != nxt_gmtime_pre_epoch[i].wday
            || tm0.tm_yday != nxt_gmtime_pre_epoch[i].yday
            || tm0.tm_hour != nxt_gmtime_pre_epoch[i].hour
            || tm0.tm_min != nxt_gmtime_pre_epoch[i].min
            || tm0.tm_sec != nxt_gmtime_pre_epoch[i].sec)
        {
            nxt_log_alert(thr->log,
                          "gmtime test failed: %T @ wday %d, "
                          "%02d.%02d.%d %02d:%02d:%02d",
                          s, tm0.tm_wday, tm0.tm_mday, tm0.tm_mon + 1,
                          tm0.tm_year + 1900, tm0.tm_hour, tm0.tm_min,
                          tm0.tm_sec);
            return NXT_ERROR;
        }
    }

    for (s = 0; s < NXT_GMTIME_MAX; s += 86400) {

        nxt_gmtime(s, &tm0);

        native = (time_t) s;

        if ((nxt_time_t) native != s) {
            /*
             * The native time_t is narrower than nxt_time_t and cannot hold
             * s, so gmtime() would be given a truncated value and there is
             * nothing left to compare against.  Where the two types agree
             * this never triggers.
             */
            break;
        }

        tm1 = gmtime(&native);

        if (tm0.tm_mday != tm1->tm_mday
            || tm0.tm_mon != tm1->tm_mon
            || tm0.tm_year != tm1->tm_year
            || tm0.tm_yday != tm1->tm_yday
            || tm0.tm_wday != tm1->tm_wday)
        {
            nxt_log_alert(thr->log,
                          "gmtime test failed: %T @ %02d.%02d.%d",
                          s, tm1->tm_mday, tm1->tm_mon + 1,
                          tm1->tm_year + 1900);
            return NXT_ERROR;
        }
    }

    /*
     * The loop above steps whole days, so it never exercises the time of
     * day.  Walk a day's worth of seconds either side of the epoch.
     */

    for (s = 0; s < 90000; s++) {

        nxt_gmtime(s, &tm0);

        native = (time_t) s;

        if ((nxt_time_t) native != s) {
            break;
        }

        tm1 = gmtime(&native);

        if (tm0.tm_hour != tm1->tm_hour
            || tm0.tm_min != tm1->tm_min
            || tm0.tm_sec != tm1->tm_sec
            || tm0.tm_mday != tm1->tm_mday
            || tm0.tm_wday != tm1->tm_wday)
        {
            nxt_log_alert(thr->log,
                          "gmtime test failed: %T @ %02d:%02d:%02d",
                          s, tm1->tm_hour, tm1->tm_min, tm1->tm_sec);
            return NXT_ERROR;
        }
    }


    nxt_thread_time_update(thr);
    start = nxt_thread_monotonic_time(thr);

    for (s = 0; s < 10000000; s++) {
        nxt_gmtime(s, &tm0);
    }

    nxt_thread_time_update(thr);
    end = nxt_thread_monotonic_time(thr);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_gmtime(): %0.1fns",
                  (end - start) / 10000000.0);


    nxt_thread_time_update(thr);
    start = nxt_thread_monotonic_time(thr);

    for (s = 0; s < 10000000; s++) {
        native = (time_t) s;
        (void) gmtime(&native);
    }

    nxt_thread_time_update(thr);
    end = nxt_thread_monotonic_time(thr);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "gmtime(): %0.1fns",
                  (end - start) / 10000000.0);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "gmtime test passed");
    return NXT_OK;
}
