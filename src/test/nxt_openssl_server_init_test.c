/*
 * Copyright (C) FreeUnit Community
 */

/*
 * Regression test for the failure paths of nxt_openssl_server_init()
 * (src/nxt_openssl.c).
 *
 * nxt_router_tls_rpc_handler() links a new bundle into the chain of the
 * socket conf and calls server_init().  When that fails, the router calls
 * nxt_router_conf_error(), which calls server_free() on the conf, and
 * server_free() calls SSL_CTX_free() on the context of every bundle in the
 * chain.  Two bugs were on this path:
 *
 *   - The bundle came from nxt_mp_get(), and server_init() returned right
 *     after a failed SSL_CTX_new() without a write to bundle->ctx.  So
 *     server_free() gave a garbage pointer to SSL_CTX_free().
 *
 *   - The bundle owns the chain file.  Only the BIO in
 *     nxt_openssl_chain_file() closed it, so the descriptor leaked when
 *     SSL_CTX_new() or BIO_new() failed.
 *
 * The test calls server_init() and server_free() directly, not through the
 * router: a router conf needs an engine, a port and a controller peer, and
 * none of them has a part in these two bugs.  Each bundle is allocated the
 * way the router allocated it before the fix, with nxt_mp_get(), and filled
 * with 0xa5, so the test also covers a caller that does not zero it.
 * NXT_TESTS hooks in src/nxt_openssl.c make SSL_CTX_new() or BIO_new()
 * fail.
 *
 * After each call the test opens a probe descriptor.  It usually gets the
 * number of the closed chain file, so a second close of that number from a
 * later path closes the probe, and the test sees it.
 */

#include <nxt_main.h>
#include "nxt_tests.h"

#include <fcntl.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>


typedef struct {
    nxt_thread_t    *thr;
    nxt_task_t      *task;
    nxt_mp_t        *mp;
    nxt_tls_conf_t  *conf;
    nxt_tls_init_t  tls_init;
    const char      *path;
} nxt_openssl_si_test_t;


static nxt_int_t
nxt_openssl_si_test_write_bundle(nxt_thread_t *thr, const char *path)
{
    int           ok;
    FILE          *f;
    X509          *x;
    EVP_PKEY      *key;
    X509_NAME     *name;
    EVP_PKEY_CTX  *kctx;

    ok = 0;
    x = NULL;
    key = NULL;
    name = NULL;
    f = NULL;

    kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (kctx == NULL
        || EVP_PKEY_keygen_init(kctx) != 1
        || EVP_PKEY_keygen(kctx, &key) != 1)
    {
        goto done;
    }

    x = X509_new();
    name = X509_NAME_new();
    if (x == NULL || name == NULL) {
        goto done;
    }

    if (X509_set_version(x, 2) != 1
        || ASN1_INTEGER_set(X509_get_serialNumber(x), 1) != 1
        || X509_gmtime_adj(X509_getm_notBefore(x), 0) == NULL
        || X509_gmtime_adj(X509_getm_notAfter(x), 3600) == NULL
        || X509_set_pubkey(x, key) != 1
        || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                      (const u_char *) "localhost", -1, -1, 0)
           != 1
        || X509_set_subject_name(x, name) != 1
        || X509_set_issuer_name(x, name) != 1
        || X509_sign(x, key, NULL) == 0)
    {
        goto done;
    }

    f = fopen(path, "w");
    if (f == NULL) {
        goto done;
    }

    ok = PEM_write_X509(f, x) == 1
         && PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL) == 1;

done:

    if (f != NULL && fclose(f) != 0) {
        ok = 0;
    }

    X509_NAME_free(name);
    X509_free(x);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(kctx);

    if (!ok) {
        nxt_log_alert(thr->log, "openssl server init test: cannot write "
                      "a test bundle to \"%s\"", path);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * Links a new bundle into the chain the way nxt_router_tls_rpc_handler()
 * does, and returns its chain file descriptor.
 */

static nxt_fd_t
nxt_openssl_si_test_add_bundle(nxt_openssl_si_test_t *t)
{
    nxt_fd_t               fd;
    nxt_tls_bundle_conf_t  *bundle;

    fd = open(t->path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        nxt_log_alert(t->thr->log, "openssl server init test: "
                      "open(\"%s\") failed %E", t->path, nxt_errno);
        return -1;
    }

    bundle = nxt_mp_get(t->mp, sizeof(nxt_tls_bundle_conf_t));
    if (bundle == NULL) {
        (void) close(fd);
        return -1;
    }

    /* Garbage, as nxt_mp_get() can give it. */
    nxt_memset(bundle, 0xa5, sizeof(nxt_tls_bundle_conf_t));

    nxt_str_set(&bundle->name, "test");
    bundle->chain_file = fd;

    bundle->next = t->conf->bundle;
    t->conf->bundle = bundle;

    return fd;
}


/*
 * Runs server_init() on a new bundle and checks the result, the context of
 * the bundle and that the chain file is closed.
 */

static nxt_int_t
nxt_openssl_si_test_init(nxt_openssl_si_test_t *t, const char *name,
    nxt_uint_t fail, nxt_bool_t last, nxt_int_t expect)
{
    nxt_fd_t               fd;
    nxt_int_t              ret;
    nxt_tls_bundle_conf_t  *bundle;

    fd = nxt_openssl_si_test_add_bundle(t);
    if (fd == -1) {
        return NXT_ERROR;
    }

    bundle = t->conf->bundle;

    nxt_openssl_test_fail = fail;

    ret = nxt_openssl_lib.server_init(t->task, t->mp, &t->tls_init, last);

    nxt_openssl_test_fail = 0;

    NXT_TEST_CHECK(t->thr->log, ret == expect,
                   "openssl server init test: %s: server_init() returned "
                   "%i, expected %i", name, ret, expect);

    if (nxt_test_fd_is_open(fd)) {
        (void) close(fd);

        nxt_log_alert(t->thr->log, "openssl server init test: %s: "
                      "the chain file %FD is still open", name, fd);
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(t->thr->log, (bundle->ctx != NULL) == (expect == NXT_OK),
                   "openssl server init test: %s: bundle->ctx is %p",
                   name, bundle->ctx);

    /* Nothing may close the number again. */
    NXT_TEST_CHECK(t->thr->log, bundle->chain_file == -1,
                   "openssl server init test: %s: bundle->chain_file is %FD, "
                   "expected -1", name, bundle->chain_file);

    return NXT_OK;
}


/*
 * Opens a probe, calls server_free() as nxt_router_conf_error() does, and
 * checks that the probe is still open.
 */

static nxt_int_t
nxt_openssl_si_test_free(nxt_openssl_si_test_t *t, const char *name)
{
    nxt_fd_t    probe;
    nxt_bool_t  is_open;

    probe = open(t->path, O_RDONLY | O_CLOEXEC);
    if (probe == -1) {
        nxt_log_alert(t->thr->log, "openssl server init test: "
                      "open(\"%s\") failed %E", t->path, nxt_errno);
        return NXT_ERROR;
    }

    nxt_openssl_lib.server_free(t->task, t->conf);

    is_open = nxt_test_fd_is_open(probe);

    if (is_open) {
        (void) close(probe);
    }

    NXT_TEST_CHECK(t->thr->log, is_open,
                   "openssl server init test: %s: server_free() closed "
                   "the probe descriptor %FD", name, probe);

    return NXT_OK;
}


static nxt_int_t
nxt_openssl_si_test_case(nxt_openssl_si_test_t *t, const char *name,
    nxt_bool_t first_ok, nxt_uint_t fail)
{
    nxt_int_t  ret;

    t->conf = nxt_mp_zget(t->mp, sizeof(nxt_tls_conf_t));
    if (t->conf == NULL) {
        return NXT_ERROR;
    }

    t->conf->no_wait_shutdown = 1;

    nxt_memzero(&t->tls_init, sizeof(nxt_tls_init_t));
    t->tls_init.conf = t->conf;

    if (first_ok) {
        /* An earlier certificate of the same socket, with a context. */
        ret = nxt_openssl_si_test_init(t, name, 0, 0, NXT_OK);
        if (ret != NXT_OK) {
            return ret;
        }
    }

    ret = nxt_openssl_si_test_init(t, name, fail, 1,
                                   fail == 0 ? NXT_OK : NXT_ERROR);
    if (ret != NXT_OK) {
        return ret;
    }

    return nxt_openssl_si_test_free(t, name);
}


/*
 * A configuration of the listener "*:443" with rotating ticket keys, made
 * at the time "msec" of the clock of the rings.
 */

static nxt_int_t
nxt_openssl_si_test_ring_conf(nxt_openssl_si_test_t *t, uint64_t msec,
    nxt_time_t lifetime, const char *tickets)
{
    nxt_str_t  json;

    nxt_openssl_test_ticket_msec = msec;

    t->conf = nxt_mp_zget(t->mp, sizeof(nxt_tls_conf_t));
    if (t->conf == NULL) {
        return NXT_ERROR;
    }

    json.length = nxt_strlen(tickets);
    json.start = (u_char *) tickets;

    nxt_memzero(&t->tls_init, sizeof(nxt_tls_init_t));

    t->tls_init.conf = t->conf;
    t->tls_init.ticket_lifetime = lifetime;
    nxt_str_set(&t->tls_init.listener, "*:443");

    t->tls_init.tickets_conf = nxt_conf_json_parse_str(t->mp, &json);
    if (t->tls_init.tickets_conf == NULL) {
        return NXT_ERROR;
    }

    return nxt_openssl_si_test_init(t, "ticket ring", 0, 1, NXT_OK);
}


/* The key the ring issues with at the time "msec" of its clock. */

static nxt_int_t
nxt_openssl_si_test_ring_issue(nxt_openssl_si_test_t *t, uint64_t msec,
    u_char *name)
{
    nxt_int_t  ret;

    nxt_openssl_test_ticket_msec = msec;

    ret = nxt_openssl_test_ticket_key(t->task, t->conf, name, 1);

    NXT_TEST_CHECK(t->thr->log, ret == NXT_OK,
                   "openssl ticket ring test: no key to issue with at %uL",
                   msec);

    return NXT_OK;
}


static nxt_int_t
nxt_openssl_si_test_ring_find(nxt_openssl_si_test_t *t, uint64_t msec,
    u_char *name, nxt_int_t expect)
{
    nxt_int_t  ret;

    nxt_openssl_test_ticket_msec = msec;

    ret = nxt_openssl_test_ticket_key(t->task, t->conf, name, 0);

    NXT_TEST_CHECK(t->thr->log, ret == expect,
                   "openssl ticket ring test: at %uL the key is %i, "
                   "expected %i", msec, ret, expect);

    return NXT_OK;
}


/*
 * The life of rotating ticket keys, with the clock of the rings in the
 * hands of the test.  The lifetime is 10 seconds: a key issues for 10,
 * only decrypts for 10 more, and then is gone.  The ring belongs to the
 * listener, so the next configuration of the listener finds the keys.
 */

static nxt_int_t
nxt_openssl_si_test_ring(nxt_openssl_si_test_t *t)
{
    nxt_tls_conf_t  *prev;
    u_char          a[16], b[16], c[16], k1[48], k2[48];

    /* Two keys as "tickets" has them: a name, an AES key and an HMAC key. */
    static const char  key1[] =
        "\"U1oDTh11mMxODuw12gS0EXX1E/PkZG13cJNQ6m5+6BGlfPTjNlIEw7PSVU3X1gTE\"";
    static const char  key2[] =
        "\"5AV0DSYIYbZWZQB7fCnTHZmMxtotb/aXjam+n2XS79lTvX3Tq9xGqpC8XKNEF2lt\"";

    (void) nxt_base64_decode(k1, (u_char *) key1 + 1, 64);
    (void) nxt_base64_decode(k2, (u_char *) key2 + 1, 64);

    /* Generated keys. */

    if (nxt_openssl_si_test_ring_conf(t, 1000, 10, "true") != NXT_OK
        || nxt_openssl_si_test_ring_issue(t, 1000, a) != NXT_OK
        || nxt_openssl_si_test_ring_find(t, 1000, a, NXT_OK) != NXT_OK
        || nxt_openssl_si_test_ring_issue(t, 10999, b) != NXT_OK)
    {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(t->thr->log, memcmp(a, b, 16) == 0,
                   "openssl ticket ring test: a new key within the lifetime");

    if (nxt_openssl_si_test_ring_find(t, 11000, a, NXT_AGAIN) != NXT_OK
        || nxt_openssl_si_test_ring_issue(t, 11000, b) != NXT_OK)
    {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(t->thr->log, memcmp(a, b, 16) != 0,
                   "openssl ticket ring test: no new key after the lifetime");

    if (nxt_openssl_si_test_ring_find(t, 11000, b, NXT_OK) != NXT_OK
        || nxt_openssl_si_test_ring_find(t, 20999, a, NXT_AGAIN) != NXT_OK
        || nxt_openssl_si_test_ring_find(t, 21000, a, NXT_DECLINED) != NXT_OK
        || nxt_openssl_si_test_ring_find(t, 21000, b, NXT_AGAIN) != NXT_OK)
    {
        return NXT_ERROR;
    }

    /*
     * A clock value from before the key was born, as a thread has it that
     * read the clock and then waited for the lock while another thread
     * added the key.  The key is new for it, not very old.
     */

    if (nxt_openssl_si_test_ring_find(t, 10000, b, NXT_OK) != NXT_OK
        || nxt_openssl_si_test_ring_find(t, 21000, b, NXT_AGAIN) != NXT_OK)
    {
        return NXT_ERROR;
    }

    /*
     * A reconfiguration with a key from the configuration.  The key of the
     * previous configuration still decrypts, also after that configuration
     * is freed.
     */

    prev = t->conf;

    if (nxt_openssl_si_test_ring_conf(t, 21000, 10, key1) != NXT_OK
        || nxt_openssl_si_test_ring_issue(t, 21000, c) != NXT_OK)
    {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(t->thr->log, memcmp(c, k1, 16) == 0,
                   "openssl ticket ring test: the configured key does not "
                   "issue");

    nxt_openssl_lib.server_free(t->task, prev);

    if (nxt_openssl_si_test_ring_find(t, 21000, b, NXT_AGAIN) != NXT_OK) {
        return NXT_ERROR;
    }

    /*
     * A configuration that the router rejects: it is made and freed, and
     * never used.  Its lifetime and its key must not reach the ring: the
     * other key is not there, and the generated key expires on time.
     */

    prev = t->conf;

    if (nxt_openssl_si_test_ring_conf(t, 21500, 1000, key2) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_openssl_lib.server_free(t->task, t->conf);
    t->conf = prev;

    if (nxt_openssl_si_test_ring_find(t, 21500, k2, NXT_DECLINED) != NXT_OK
        || nxt_openssl_si_test_ring_find(t, 30999, b, NXT_AGAIN) != NXT_OK
        || nxt_openssl_si_test_ring_find(t, 31000, b, NXT_DECLINED) != NXT_OK)
    {
        return NXT_ERROR;
    }

    /* A key that left the configuration decrypts until it ages out. */

    if (nxt_openssl_si_test_ring_conf(t, 32000, 10, key2) != NXT_OK
        || nxt_openssl_si_test_ring_issue(t, 32000, c) != NXT_OK)
    {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(t->thr->log, memcmp(c, k2, 16) == 0,
                   "openssl ticket ring test: the new configured key does "
                   "not issue");

    nxt_openssl_lib.server_free(t->task, prev);

    if (nxt_openssl_si_test_ring_find(t, 40999, k1, NXT_AGAIN) != NXT_OK
        || nxt_openssl_si_test_ring_find(t, 41000, k1, NXT_DECLINED) != NXT_OK
        || nxt_openssl_si_test_ring_issue(t, 41999, c) != NXT_OK)
    {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(t->thr->log, memcmp(c, k2, 16) == 0,
                   "openssl ticket ring test: the configured key gave way "
                   "within its lifetime");

    /* A configured key that is not replaced gives way to a generated one. */

    if (nxt_openssl_si_test_ring_issue(t, 42000, c) != NXT_OK) {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(t->thr->log, memcmp(c, k2, 16) != 0,
                   "openssl ticket ring test: the configured key issues "
                   "after its lifetime");

    /*
     * A configured key that expired is not taken back by a reconfiguration
     * that still lists it: its old tickets would work again.
     */

    if (nxt_openssl_si_test_ring_find(t, 51999, k2, NXT_AGAIN) != NXT_OK
        || nxt_openssl_si_test_ring_find(t, 52000, k2, NXT_DECLINED) != NXT_OK)
    {
        return NXT_ERROR;
    }

    prev = t->conf;

    if (nxt_openssl_si_test_ring_conf(t, 52000, 10, key2) != NXT_OK
        || nxt_openssl_si_test_ring_issue(t, 52000, c) != NXT_OK
        || nxt_openssl_si_test_ring_find(t, 52000, k2, NXT_DECLINED) != NXT_OK)
    {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(t->thr->log, memcmp(c, k2, 16) != 0,
                   "openssl ticket ring test: an expired configured key "
                   "issues again after a reconfiguration");

    nxt_openssl_lib.server_free(t->task, prev);

    nxt_openssl_test_ticket_msec = 0;

    return nxt_openssl_si_test_free(t, "ticket ring");
}


nxt_int_t
nxt_openssl_server_init_test(nxt_thread_t *thr)
{
    char                   path[256];
    const char             *tmpdir;
    nxt_int_t              ret;
    nxt_openssl_si_test_t  t;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "openssl server init test started");

    nxt_memzero(&t, sizeof(nxt_openssl_si_test_t));

    t.thr = thr;
    t.task = thr->task;
    t.task->thread = thr;

    if (nxt_openssl_lib.library_init(t.task) != NXT_OK) {
        return NXT_ERROR;
    }

    tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] != '/') {
        tmpdir = "/tmp";
    }

    (void) nxt_sprintf((u_char *) path, (u_char *) path + sizeof(path),
                       "%s/nxt_openssl_server_init_test.XXXXXX%Z", tmpdir);

    ret = mkstemp(path);
    if (ret == -1) {
        nxt_log_alert(thr->log, "openssl server init test: "
                      "mkstemp(\"%s\") failed %E", path, nxt_errno);
        return NXT_ERROR;
    }

    (void) close(ret);

    t.path = path;

    ret = NXT_ERROR;

    t.mp = nxt_mp_create(1024, 128, 256, 32);
    if (t.mp == NULL) {
        goto done;
    }

    if (nxt_openssl_si_test_write_bundle(thr, path) != NXT_OK) {
        goto done;
    }

    ret = nxt_openssl_si_test_case(&t, "SSL_CTX_new() fails", 0,
                                   NXT_OPENSSL_TEST_FAIL_CTX_NEW);
    if (ret != NXT_OK) {
        goto done;
    }

    ret = nxt_openssl_si_test_case(&t, "SSL_CTX_new() fails, second bundle",
                                   1, NXT_OPENSSL_TEST_FAIL_CTX_NEW);
    if (ret != NXT_OK) {
        goto done;
    }

    ret = nxt_openssl_si_test_case(&t, "BIO_new() fails", 0,
                                   NXT_OPENSSL_TEST_FAIL_BIO_NEW);
    if (ret != NXT_OK) {
        goto done;
    }

    ret = nxt_openssl_si_test_case(&t, "BIO_new() fails, second bundle", 1,
                                   NXT_OPENSSL_TEST_FAIL_BIO_NEW);
    if (ret != NXT_OK) {
        goto done;
    }

    ret = nxt_openssl_si_test_case(&t, "success", 1, 0);
    if (ret != NXT_OK) {
        goto done;
    }

    ret = nxt_openssl_si_test_ring(&t);

done:

    (void) unlink(path);

    if (t.mp != NULL) {
        nxt_mp_destroy(t.mp);
    }

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log,
                      "openssl server init test passed");
    }

    return ret;
}
