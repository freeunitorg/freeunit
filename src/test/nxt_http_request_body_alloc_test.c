
/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include "nxt_tests.h"


/*
 * nxt_http_request_body_alloc() picks the request body buffer from the
 * listener's body_buffer_size: a body that fits stays in memory, a longer
 * or unknown one gets an unlinked temporary file with a memory part of
 * body_buffer_size.  The HTTP/1 path used to do this inline; other
 * frontends allocate the body the same way, so the table pins the choice
 * and the sizes.
 */

typedef struct {
    const char  *name;
    size_t      body_length;
    nxt_bool_t  file;
    size_t      mem_size;
} nxt_http_request_body_alloc_test_case_t;


static const nxt_http_request_body_alloc_test_case_t
    nxt_http_request_body_alloc_test_cases[] =
{
    { "a body below body_buffer_size",   1000,        0, 1000 },
    { "a body of body_buffer_size",      4096,        0, 4096 },
    { "a body above body_buffer_size",   4097,        1, 4096 },
    { "a body of unknown length",        (size_t) -1, 1, 4096 },
};


nxt_int_t
nxt_http_request_body_alloc_test(nxt_thread_t *thr)
{
    nxt_fd_t                 fd;
    const char               *tmpdir;
    size_t                   len;
    u_char                   *notdir;
    nxt_mp_t                 *mp;
    nxt_buf_t                *b;
    nxt_int_t                ret;
    nxt_uint_t               i;
    nxt_socket_conf_t        skcf;
    nxt_http_request_t       r;
    nxt_socket_conf_joint_t  joint;

    const nxt_http_request_body_alloc_test_case_t  *tc;

    nxt_memzero(&skcf, sizeof(nxt_socket_conf_t));
    nxt_memzero(&joint, sizeof(nxt_socket_conf_joint_t));

    /* An empty TMPDIR is the same as no TMPDIR. */

    tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0') {
        tmpdir = "/tmp";
    }

    skcf.body_buffer_size = 4096;
    skcf.body_temp_path.start = (u_char *) tmpdir;
    skcf.body_temp_path.length = nxt_strlen(tmpdir);

    joint.socket_conf = &skcf;

    fd = -1;

    for (i = 0; i < nxt_nitems(nxt_http_request_body_alloc_test_cases); i++) {
        tc = &nxt_http_request_body_alloc_test_cases[i];

        mp = nxt_mp_create(1024, 128, 256, 32);
        if (mp == NULL) {
            return NXT_ERROR;
        }

        nxt_memzero(&r, sizeof(nxt_http_request_t));
        r.mem_pool = mp;
        r.conf = &joint;

        ret = nxt_http_request_body_alloc(thr->task, &r, tc->body_length);

        b = r.body;

        if (ret != NXT_OK || b == NULL) {
            nxt_log_alert(thr->log, "http request body alloc test failed: "
                          "%s: no buffer", tc->name);
            goto fail;
        }

        if (nxt_buf_is_file(b) != tc->file) {
            nxt_log_alert(thr->log, "http request body alloc test failed: "
                          "%s: file %d expected %d",
                          tc->name, nxt_buf_is_file(b), tc->file);
            goto fail;
        }

        if ((size_t) nxt_buf_mem_size(&b->mem) != tc->mem_size
            || b->mem.pos != b->mem.start
            || b->mem.free != b->mem.start)
        {
            nxt_log_alert(thr->log, "http request body alloc test failed: "
                          "%s: memory part %uz expected %uz",
                          tc->name, nxt_buf_mem_size(&b->mem), tc->mem_size);
            goto fail;
        }

        if (tc->file) {
            fd = b->file->fd;

            if (b->file->fd == -1 || b->file_end != 0
                || b->file->size != (nxt_off_t) tc->body_length)
            {
                nxt_log_alert(thr->log, "http request body alloc test "
                              "failed: %s: file fd %d size %O",
                              tc->name, b->file->fd, b->file->size);
                goto fail;
            }

            if (!nxt_test_fd_is_open(b->file->fd)) {
                nxt_log_alert(thr->log, "http request body alloc test "
                              "failed: %s: file descriptor is not open",
                              tc->name);
                goto fail;
            }

            nxt_fd_close(fd);
            fd = -1;
        }

        nxt_mp_destroy(mp);
    }

    /*
     * A temporary path that cannot be opened is an error, not a buffer.
     * A regular file is used as the directory: a file under it cannot be
     * made on any host (ENOTDIR).
     */

    len = nxt_strlen(tmpdir) + nxt_length("/nxt_body_alloc_XXXXXX") + 1;

    notdir = nxt_malloc(len);
    if (notdir == NULL) {
        return NXT_ERROR;
    }

    (void) nxt_sprintf(notdir, notdir + len,
                       "%s/nxt_body_alloc_XXXXXX%Z", tmpdir);

    fd = mkstemp((char *) notdir);
    if (fd == -1) {
        nxt_log_alert(thr->log, "http request body alloc test failed: "
                      "mkstemp(\"%s\") %E", notdir, nxt_errno);
        nxt_free(notdir);
        return NXT_ERROR;
    }

    nxt_fd_close(fd);
    fd = -1;

    skcf.body_temp_path.start = notdir;
    skcf.body_temp_path.length = nxt_strlen(notdir);

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        (void) unlink((char *) notdir);
        nxt_free(notdir);
        return NXT_ERROR;
    }

    nxt_memzero(&r, sizeof(nxt_http_request_t));
    r.mem_pool = mp;
    r.conf = &joint;

    ret = nxt_http_request_body_alloc(thr->task, &r, 8192);

    if (ret != NXT_ERROR || r.body != NULL) {
        nxt_log_alert(thr->log, "http request body alloc test failed: "
                      "a bad temporary path returned %i", ret);
        (void) unlink((char *) notdir);
        nxt_free(notdir);
        goto fail;
    }

    nxt_mp_destroy(mp);
    (void) unlink((char *) notdir);
    nxt_free(notdir);

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "http request body alloc test passed");

    return NXT_OK;

fail:

    if (fd != -1) {
        nxt_fd_close(fd);
    }

    nxt_mp_destroy(mp);

    return NXT_ERROR;
}
