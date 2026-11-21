/*
 * Copyright (C) FreeUnit contributors.
 */

#include <nxt_main.h>
#include <nxt_log_sink.h>


#define NXT_LOG_FILE_SINK_ATTEMPTS  3


static ssize_t nxt_log_file_sink_write(void *sink, const u_char *start,
    size_t length);
static ssize_t nxt_log_file_sink_write_fd(nxt_fd_t fd, const u_char *start,
    size_t length);
static int nxt_log_file_sink_flush(void *sink);


const nxt_log_sink_t  nxt_log_file_sink = {
    NXT_LOG_SINK_VERSION,
    "file",
    NULL,
    NULL,
    nxt_log_file_sink_write,
    nxt_log_file_sink_flush,
};


/*
 * One record is one write() and a bounded retry loop: a short write is
 * completed with the same buffer, so the record is never split into two
 * records or resumed after a later record has been appended.  A short write
 * is rare (a full disk, a quota, a signal after some bytes), and the loop
 * makes at most NXT_LOG_FILE_SINK_ATTEMPTS calls that are not interrupted by
 * a signal.  The bound keeps the caller's mutex held for a bounded number of
 * calls: a descriptor that keeps taking less than the rest ends the record
 * as a failed write.  A write of no byte or an error other than EINTR ends it
 * too.  The count tells the caller whether a prefix landed, so it can repair
 * the line.
 *
 * The descriptor is the caller's: the access log keeps it for the reopen,
 * and the sink context points at it.
 *
 * Every return that is not the whole record carries a defined errno: a
 * failed write() leaves its error, and a loop that stops without a system
 * call error, an exhausted retry budget or a zero-byte write, reports
 * NXT_EIO.  The caller's loss marker and warning can then name a reason.
 */

static ssize_t
nxt_log_file_sink_write(void *sink, const u_char *start, size_t length)
{
    size_t      left;
    ssize_t     n;
    u_char      *p;
    nxt_fd_t    fd;
    nxt_uint_t  attempts;

    fd = *(nxt_fd_t *) sink;

    p = (u_char *) start;
    left = length;
    attempts = 0;

    for ( ;; ) {
        n = nxt_log_file_sink_write_fd(fd, p, left);

        if (n == -1 && nxt_errno == NXT_EINTR) {
            continue;
        }

        if (n <= 0) {
            if (n == 0) {
                nxt_set_errno(NXT_EIO);
            }

            break;
        }

        p += n;
        left -= n;

        if (left == 0) {
            return length;
        }

        if (++attempts == NXT_LOG_FILE_SINK_ATTEMPTS) {
            nxt_set_errno(NXT_EIO);
            break;
        }
    }

    return p - (u_char *) start;
}


static int
nxt_log_file_sink_flush(void *sink)
{
    (void) sink;

    /* The file destination is unbuffered: every record is one write(). */
    return 0;
}


#if (NXT_TESTS)

/*
 * The faults of the next write() calls, for the access log test, one per
 * call in order: -1 fails the call with ENOSPC before any byte, -2 fails it
 * with EINTR before any byte, and n >= 0 lets the call write at most n
 * bytes.  nxt_log_file_sink_test_faults is the number of faults left;
 * the calls after them are not changed.
 */

const ssize_t  *nxt_log_file_sink_test_fault;
nxt_uint_t     nxt_log_file_sink_test_faults;

#endif


static ssize_t
nxt_log_file_sink_write_fd(nxt_fd_t fd, const u_char *start, size_t length)
{
#if (NXT_TESTS)
    ssize_t  fault;

    if (nxt_log_file_sink_test_faults != 0) {
        nxt_log_file_sink_test_faults--;
        fault = *nxt_log_file_sink_test_fault++;

        if (fault < 0) {
            nxt_set_errno((fault == -2) ? NXT_EINTR : NXT_ENOSPC);
            return -1;
        }

        length = nxt_min(length, (size_t) fault);
    }
#endif

    return write(fd, start, length);
}
