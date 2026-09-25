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
    char         victim[NXT_MAX_PATH_LEN];
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
    (void) nxt_sprintf((u_char *) victim,
                       (u_char *) victim + sizeof(victim),
                       "%s/victim%Z", dir);

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
                                      "0x%04xd, expected the 0640 (0x1a0) it had",
                                      name, (unsigned) (st.st_mode & 07777));
    }

    /*
     * Set-user-ID survives too.  The replacement carries the mode over with
     * fchmod() and the ownership with fchown(), and a chown() of a regular
     * file clears set-user-ID -- so doing them in that order would drop the
     * bit while appearing to preserve the mode.  An unchanged owner is
     * enough to trigger it, which is why this is assertable without root.
     */

    if (chmod(name, 04640) != 0) {
        nxt_main_file_store_test_fail(thr, "chmod(\"%s\") failed %E", name,
                                      nxt_errno);
    }

    if (nxt_main_file_store_test_store(task, dir, tmp_name, name,
                                       "{\"setid\":1}") != NXT_OK)
    {
        nxt_main_file_store_test_fail(thr, "the set-user-ID preserving store "
                                      "failed");
    }

    if (stat(name, &st) != 0 || (st.st_mode & 07777) != 04640) {
        nxt_main_file_store_test_fail(thr, "\"%s\" came back with mode "
                                      "0x%04xd, expected the 04640 (0x9a0) "
                                      "it had -- chown() after chmod() "
                                      "clears set-user-ID", name,
                                      (unsigned) (st.st_mode & 07777));
    }

    if (chmod(name, 0640) != 0) {
        nxt_main_file_store_test_fail(thr, "chmod(\"%s\") failed %E", name,
                                      nxt_errno);
    }

    /*
     * The temporary's name is predictable and the store runs as root, so a
     * state directory writable by anyone else is a place to leave a symbolic
     * link under that name and have the store follow it.  Opening with
     * O_TRUNC did: it truncated whatever the link addressed and the rename
     * then installed the link as the state file.  The store must write the
     * state file and leave the link's target alone.
     */

    if (nxt_main_file_store_test_plant(victim, "precious", 0600) != NXT_OK) {
        nxt_main_file_store_test_fail(thr, "could not plant the link target");
    }

    (void) unlink(tmp_name);

    if (symlink(victim, tmp_name) != 0) {
        nxt_main_file_store_test_fail(thr, "symlink(\"%s\", \"%s\") failed "
                                      "%E", victim, tmp_name, nxt_errno);
    }

    if (nxt_main_file_store_test_store(task, dir, tmp_name, name,
                                       "{\"five\":5}") != NXT_OK)
    {
        nxt_main_file_store_test_fail(thr, "a store whose temporary was a "
                                      "symbolic link failed");
    }

    if (nxt_main_file_store_test_holds(victim, "precious") != NXT_OK) {
        nxt_main_file_store_test_fail(thr, "the store followed the link and "
                                      "wrote through it to \"%s\"", victim);
    }

    if (nxt_main_file_store_test_holds(name, "{\"five\":5}") != NXT_OK) {
        nxt_main_file_store_test_fail(thr, "\"%s\" does not hold what the "
                                      "store past a link wrote", name);
    }

    if (lstat(name, &st) != 0 || !S_ISREG(st.st_mode)) {
        nxt_main_file_store_test_fail(thr, "\"%s\" is not a regular file -- "
                                      "the link was installed as the state "
                                      "file", name);
    }

    (void) unlink(victim);

    /*
     * The destination can be a symbolic link as well, and there the danger
     * is not the write -- the rename replaces the link whatever it points
     * at -- but the inheritance.  Reading the destination's mode with
     * stat() follows the link, so a link to a file of the other user's own
     * hands them the mode of the conf.json that replaces it: aim it at
     * something 0666 and every later store inherits that again.
     */

    if (nxt_main_file_store_test_plant(victim, "bait", 0666) != NXT_OK) {
        nxt_main_file_store_test_fail(thr, "could not plant the mode bait");
    }

    (void) unlink(name);

    if (symlink(victim, name) != 0) {
        nxt_main_file_store_test_fail(thr, "symlink(\"%s\", \"%s\") failed "
                                      "%E", victim, name, nxt_errno);
    }

    if (nxt_main_file_store_test_store(task, dir, tmp_name, name,
                                       "{\"six\":6}") != NXT_OK)
    {
        nxt_main_file_store_test_fail(thr, "a store onto a linked "
                                      "destination failed");
    }

    if (nxt_main_file_store_test_holds(victim, "bait") != NXT_OK) {
        nxt_main_file_store_test_fail(thr, "the store wrote through the "
                                      "linked destination to \"%s\"", victim);
    }

    if (nxt_main_file_store_test_holds(name, "{\"six\":6}") != NXT_OK) {
        nxt_main_file_store_test_fail(thr, "\"%s\" does not hold what the "
                                      "store onto a link wrote", name);
    }

    if (lstat(name, &st) != 0 || !S_ISREG(st.st_mode)) {
        nxt_main_file_store_test_fail(thr, "\"%s\" is not a regular file "
                                      "after a store onto a link", name);
    }

    if ((st.st_mode & 07777) != 0600) {
        nxt_main_file_store_test_fail(thr, "\"%s\" came back with mode "
                                      "0x%04xd, expected the 0600 (0x180) "
                                      "the temporary was created with -- the "
                                      "store inherited the link target's "
                                      "mode", name,
                                      (unsigned) (st.st_mode & 07777));
    }

    (void) unlink(victim);

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "main file store test passed");

    ret = NXT_OK;

done:

    (void) unlink(name);
    (void) unlink(tmp_name);
    (void) unlink(victim);
    (void) rmdir(dir);

    return ret;
}
