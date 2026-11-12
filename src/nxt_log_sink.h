/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_LOG_SINK_H_INCLUDED_
#define _NXT_LOG_SINK_H_INCLUDED_


#include <nxt_main.h>
#include <nxt_conf.h>


/*
 * The log sink interface (freeunit-exp issue 77).
 *
 * A sink is the destination of one complete log record.  The access log
 * builds a record, newline included, and hands it to its sink; a module sink
 * is resolved by an exported symbol and a version check, shaped like the TLS
 * backend table.  The built-in file destination is a sink too, so the writer
 * has one path for every destination.
 *
 * The interface is deliberately small: one record in, a byte count out.  The
 * record is assembled before the call, so the sink never sees a variable, a
 * request or the configuration; a later module cannot ask for more than the
 * caller already has.
 */

#define NXT_LOG_SINK_VERSION  1u


typedef struct nxt_log_sink_s  nxt_log_sink_t;

struct nxt_log_sink_s {
    /* A loader refuses a table whose version it does not know. */
    uint32_t     version;

    /* "file", "syslog", ...: for the error message and the status report. */
    const char   *name;

    /*
     * Create the destination from its configuration and return the context
     * that write() and close() receive.  NULL means "the built-in file
     * destination", whose context the caller owns.  An error is a context
     * of NULL with the reason reported through the caller's log.
     */
    void         *(*open)(nxt_mp_t *mp, const nxt_conf_value_t *conf);

    /* Release the context from open().  NULL for the built-in sink. */
    void         (*close)(void *sink);

    /*
     * Write one complete record, newline included.
     *
     * Returns the number of bytes accepted:
     *   length          the whole record is in the destination;
     *   0               no byte was accepted, errno says why;
     *   0 < n < length  the destination stopped after n bytes, errno says
     *                   why.  The caller marks the log torn and repairs
     *                   the missing newline before the next record, so a
     *                   short count is a lost record, never a silent split.
     *
     * Every count other than length carries a defined errno.  A sink that
     * stops without a system call error sets errno itself; the built-in
     * file sink reports NXT_EIO for a zero-byte write and for an exhausted
     * retry budget.
     *
     * The sink must not block indefinitely and must not call back into the
     * access log.  The caller owns the record buffer and keeps it valid only
     * for the duration of this call; a sink that wants to keep bytes copies
     * them.  The built-in file sink makes at most three write() calls per
     * record: a short write is completed with the same buffer, and a call
     * interrupted by a signal is not one of the three.
     */
    ssize_t      (*write)(void *sink, const u_char *record, size_t length);

    /* Best-effort flush for a buffering sink; 0 on success, -1 with errno. */
    int          (*flush)(void *sink);
};


/*
 * The built-in file destination: the descriptor is opened by the main
 * process and passed to the router, and the caller owns it.  The sink
 * context is the caller's nxt_fd_t; the descriptor is written with the
 * retry loop described on write() above.
 */
extern const nxt_log_sink_t  nxt_log_file_sink;


#if (NXT_TESTS)

/*
 * The write faults of the access log test, one per write() call in order:
 * -1 fails the call with ENOSPC before any byte, -2 fails it with EINTR
 * before any byte, and n >= 0 lets the call write at most n bytes.  The
 * definitions are in src/nxt_log_file_sink.c.
 */

extern const ssize_t  *nxt_log_file_sink_test_fault;
extern nxt_uint_t     nxt_log_file_sink_test_faults;

#endif


#endif /* _NXT_LOG_SINK_H_INCLUDED_ */
