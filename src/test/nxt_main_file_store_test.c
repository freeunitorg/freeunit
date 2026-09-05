/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for issue #215: nxt_main_file_store()
 * (src/nxt_main_process.c) ignored its "tmp_name" argument, opening the
 * destination itself with NXT_FILE_TRUNCATE and finishing with a rename()
 * of the file onto itself.  The state file was rewritten in place, and the
 * short-write path unlinked what was then the live conf.json.
 *
 * Four invariants, each observed rather than inferred:
 *
 *   - a store replaces the destination by rename(2), pinned down by the
 *     inode number changing.  An in-place rewrite keeps the inode, so this
 *     case alone separates the fixed function from the old one;
 *   - no temporary survives a successful store, including a stale one from
 *     an interrupted run left in the way;
 *   - a store that cannot create its temporary reports NXT_ERROR and leaves
 *     the destination byte-for-byte intact -- the shape of an ENOSPC or a
 *     read-only state directory, where the old code destroyed the
 *     configuration it was asked to update;
 *   - an existing destination keeps its mode across the replacement.
 *
 * Ownership is not asserted: the test does not require root, and chown() to
 * another uid is not available to an unprivileged run.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_main_process.h>
#include "nxt_tests.h"

#include <sys/stat.h>


#define nxt_main_file_store_test_fail(thr, ...)                               \
    do {                                                                      \
        nxt_log_alert((thr)->log, "main file store test: " __VA_ARGS__);       \
        goto done;                                                            \
    } while (0)


static nxt_int_t
nxt_main_file_store_test_content(const char *name, char *buf, size_t size,
    ssize_t *len)
{
    int      fd;
    ssize_t  n;

    fd = open(name, O_RDONLY);
    if (nxt_slow_path(fd == -1)) {
        return NXT_ERROR;
    }

    n = read(fd, buf, size);

    close(fd);

    if (nxt_slow_path(n < 0)) {
        return NXT_ERROR;
    }

    *len = n;

    return NXT_OK;
}


static nxt_int_t
nxt_main_file_store_test_holds(const char *name, const char *expected)
{
    char     buf[256];
    size_t   len;
    ssize_t  n;

    if (nxt_slow_path(nxt_main_file_store_test_content(name, buf, sizeof(buf),
                                                       &n)
                      != NXT_OK))
    {
        return NXT_ERROR;
    }

    len = nxt_strlen(expected);

    if (nxt_slow_path((size_t) n != len || memcmp(buf, expected, len) != 0)) {
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_main_file_store_test_plant(const char *name, const char *content,
    mode_t mode)
{
    int      fd;
    size_t   len;
    ssize_t  n;

    fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (nxt_slow_path(fd == -1)) {
        return NXT_ERROR;
    }

    len = nxt_strlen(content);

    n = write(fd, content, len);

    close(fd);

    /* open() honours the umask; the test needs the exact mode. */
    if (nxt_slow_path(n != (ssize_t) len || chmod(name, mode) != 0)) {
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_main_file_store_test_store(nxt_task_t *task, const char *dir,
    const char *tmp_name, const char *name, const char *content)
{
    return nxt_main_test_run_file_store(task, dir, tmp_name, name,
                                        (u_char *) content,
                                        nxt_strlen(content));
}


nxt_int_t
nxt_main_file_store_test(nxt_thread_t *thr)
{
    char         dir[NXT_MAX_PATH_LEN], name[NXT_MAX_PATH_LEN];
    char         tmp_name[NXT_MAX_PATH_LEN], nowhere[NXT_MAX_PATH_LEN];
    ino_t        first_ino;
    const char   *tmpdir;
    nxt_int_t    ret;
    nxt_task_t   *task;
    struct stat  st;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main file store test started");

    task = thr->task;
    task->thread = thr;

    tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] != '/') {
        tmpdir = "/tmp";
    }

    (void) nxt_sprintf((u_char *) dir, (u_char *) dir + sizeof(dir),
                       "%s/nxt_main_file_store_test.XXXXXX%Z", tmpdir);

    if (nxt_slow_path(mkdtemp(dir) == NULL)) {
        nxt_log_alert(thr->log, "main file store test: mkdtemp(\"%s\") "
                      "failed %E", dir, nxt_errno);
        return NXT_ERROR;
    }

    ret = NXT_ERROR;

    (void) nxt_sprintf((u_char *) name, (u_char *) name + sizeof(name),
                       "%s/conf.json%Z", dir);
    (void) nxt_sprintf((u_char *) tmp_name,
                       (u_char *) tmp_name + sizeof(tmp_name),
                       "%s/conf.json.tmp%Z", dir);
    (void) nxt_sprintf((u_char *) nowhere,
                       (u_char *) nowhere + sizeof(nowhere),
                       "%s/absent/conf.json.tmp%Z", dir);

    /* A first store into an empty directory creates the destination. */

    if (nxt_main_file_store_test_store(task, dir, tmp_name, name,
                                       "{\"one\":1}") != NXT_OK)
    {
        nxt_main_file_store_test_fail(thr, "the first store failed");
    }

    if (nxt_main_file_store_test_holds(name, "{\"one\":1}") != NXT_OK) {
        nxt_main_file_store_test_fail(thr, "\"%s\" does not hold what the "
                                      "first store wrote", name);
    }

    if (stat(name, &st) != 0) {
        nxt_main_file_store_test_fail(thr, "stat(\"%s\") failed %E", name,
                                      nxt_errno);
    }

    first_ino = st.st_ino;

    /*
     * A stale temporary sits in the way, standing in for a store that was
     * interrupted before its rename: it must be overwritten silently and
     * must not survive.
     */

    if (nxt_main_file_store_test_plant(tmp_name, "stale", 0600) != NXT_OK) {
        nxt_main_file_store_test_fail(thr, "could not plant a stale "
                                      "temporary");
    }

    if (nxt_main_file_store_test_store(task, dir, tmp_name, name,
                                       "{\"two\":2}") != NXT_OK)
    {
        nxt_main_file_store_test_fail(thr, "the second store failed");
    }

    if (nxt_main_file_store_test_holds(name, "{\"two\":2}") != NXT_OK) {
        nxt_main_file_store_test_fail(thr, "\"%s\" does not hold what the "
                                      "second store wrote", name);
    }

    if (stat(tmp_name, &st) == 0) {
        nxt_main_file_store_test_fail(thr, "\"%s\" was left behind by a "
                                      "successful store", tmp_name);
    }

    if (stat(name, &st) != 0) {
        nxt_main_file_store_test_fail(thr, "stat(\"%s\") failed %E", name,
                                      nxt_errno);
    }

    if (st.st_ino == first_ino) {
        nxt_main_file_store_test_fail(thr, "\"%s\" kept inode %uL across a "
                                      "store -- the file was rewritten in "
                                      "place, so a crash mid-write leaves "
                                      "it truncated", name,
                                      (uint64_t) first_ino);
    }

    /*
     * A store that cannot create its temporary -- the path names a
     * directory that does not exist, the shape of a full or read-only state
     * directory -- must report the failure and leave the destination as it
     * was.
     */

    if (nxt_main_file_store_test_store(task, dir, nowhere, name,
                                       "{\"three\":3}") == NXT_OK)
    {
        nxt_main_file_store_test_fail(thr, "a store whose temporary cannot "
                                      "be created reported success");
    }

    if (nxt_main_file_store_test_holds(name, "{\"two\":2}") != NXT_OK) {
        nxt_main_file_store_test_fail(thr, "a failed store damaged \"%s\"",
                                      name);
    }

    /* An existing destination keeps its mode across the replacement. */

    if (chmod(name, 0640) != 0) {
        nxt_main_file_store_test_fail(thr, "chmod(\"%s\") failed %E", name,
                                      nxt_errno);
    }

    if (nxt_main_file_store_test_store(task, dir, tmp_name, name,
                                       "{\"four\":4}") != NXT_OK)
    {
        nxt_main_file_store_test_fail(thr, "the mode-preserving store "
                                      "failed");
    }

    if (stat(name, &st) != 0 || (st.st_mode & 07777) != 0640) {
        nxt_main_file_store_test_fail(thr, "\"%s\" came back with mode "
                                      "%04o, expected the 0640 it had",
                                      name, (unsigned) (st.st_mode & 07777));
    }

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main file store test passed");

    ret = NXT_OK;

done:

    (void) unlink(name);
    (void) unlink(tmp_name);
    (void) rmdir(dir);

    return ret;
}
