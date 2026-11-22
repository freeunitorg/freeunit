
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_application.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_http_compression.h>
#include "nxt_tests.h"


extern char  **environ;

nxt_module_init_t  nxt_init_modules[1];
nxt_uint_t         nxt_init_modules_n;


/*
 * Whether the descriptor is still open in this process.  Several port tests
 * assert on a descriptor's real state rather than on what a handler returned,
 * because the bug they guard against is a close that did or did not happen.
 */

nxt_bool_t
nxt_test_fd_is_open(nxt_fd_t fd)
{
    return fcntl(fd, F_GETFD) != -1;
}


/*
 * Runs fn(data) in a child process.  Returns the exit status of the child,
 * or -1 when the child did not exit.
 */

int
nxt_test_in_child(nxt_thread_t *thr, const char *name, int (*fn)(void *),
    void *data)
{
    int    status;
    pid_t  child;

    child = fork();

    if (child == 0) {
        _exit(fn(data));
    }

    if (child == -1) {
        nxt_log_alert(thr->log, "%s: fork() failed %E", name, nxt_errno);
        return -1;
    }

    /* A signal may interrupt waitpid().  Then it is called again. */

    while (waitpid(child, &status, 0) != child) {
        if (nxt_errno != NXT_EINTR) {
            nxt_log_alert(thr->log, "%s: waitpid() failed %E", name,
                          nxt_errno);
            return -1;
        }
    }

    if (!WIFEXITED(status)) {
        nxt_log_alert(thr->log, "%s: child killed by signal %d", name,
                      WTERMSIG(status));
        return -1;
    }

    return WEXITSTATUS(status);
}


/*
 * The name test of nxt_http_comp_accept_encoding() compares 8, 4, 2 and 1
 * bytes at a time.  Its result must be that of nxt_strncasecmp() with
 * "Accept-Encoding", the compare that it replaced.  The names are every
 * case variant, every byte at every position of three spellings, and each
 * length from 0 to 16.  Each name ends where a PROT_NONE page starts, so a
 * read past the length kills the child.
 */

typedef struct {
    nxt_thread_t  *thr;
    u_char        *end;
} nxt_http_comp_name_test_ctx_t;


static nxt_int_t
nxt_http_comp_name_test_one(nxt_http_comp_name_test_ctx_t *ctx,
    const u_char *src, size_t length)
{
    bool    got, want;
    u_char  *name, *p;
    size_t  i;
    u_char  hex[2 * 16];

    static const u_char  digits[] = "0123456789abcdef";

    name = ctx->end - length;
    nxt_memcpy(name, src, length);

    got = nxt_http_comp_test_accept_encoding_name(name, length);
    want = (length == nxt_length("Accept-Encoding")
            && nxt_strncasecmp(name, (const u_char *) "Accept-Encoding",
                               length) == 0);

    if (got != want) {
        p = hex;

        for (i = 0; i < length; i++) {
            *p++ = digits[src[i] >> 4];
            *p++ = digits[src[i] & 0xf];
        }

        nxt_log_alert(ctx->thr->log, "accept-encoding name test failed: "
                      "name %*s (hex), length %uz, gave %d, not %d",
                      (size_t) (p - hex), hex, length, got, want);

        return NXT_ERROR;
    }

    return NXT_OK;
}


static int
nxt_http_comp_name_test_child(void *data)
{
    u_char                         buf[17];
    size_t                         len, pos;
    nxt_uint_t                     v, b, i, k;
    nxt_http_comp_name_test_ctx_t  *ctx;

    static const char  *bases[] = {
        "Accept-Encoding", "accept-encoding", "ACCEPT-ENCODING",
    };

    ctx = data;

    len = nxt_length("Accept-Encoding");

    /* Each of the 2^14 case variants of the 14 letters matches. */

    for (v = 0; v < (1u << 14); v++) {
        nxt_memcpy(buf, "accept-encoding", len);

        for (i = 0, k = 0; i < len; i++) {
            if (buf[i] == '-') {
                continue;
            }

            if (v & (1u << k)) {
                buf[i] &= (u_char) ~0x20;
            }

            k++;
        }

        if (nxt_http_comp_name_test_one(ctx, buf, len) != NXT_OK) {
            return 1;
        }
    }

    /*
     * Every byte at every position: NUL, the bytes next to the letters,
     * '-', '_', and the high bytes that differ from a letter only in the
     * high bit.
     */

    for (i = 0; i < nxt_nitems(bases); i++) {
        for (pos = 0; pos < len; pos++) {
            for (b = 0; b < 256; b++) {
                nxt_memcpy(buf, bases[i], len);
                buf[pos] = (u_char) b;

                if (nxt_http_comp_name_test_one(ctx, buf, len) != NXT_OK) {
                    return 1;
                }
            }
        }
    }

    /* Each length from 0 to 16, as a prefix of a longer name. */

    nxt_memcpy(buf, "Accept-Encodings", 16);

    for (pos = 0; pos <= 16; pos++) {
        if (nxt_http_comp_name_test_one(ctx, buf, pos) != NXT_OK) {
            return 1;
        }
    }

    return 0;
}


static nxt_int_t
nxt_http_comp_name_test(nxt_thread_t *thr)
{
    int                            ret;
    u_char                         *p;
    nxt_http_comp_name_test_ctx_t  ctx;

    p = mmap(NULL, 2 * nxt_pagesize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        nxt_log_alert(thr->log, "accept-encoding name test: mmap() failed %E",
                      nxt_errno);
        return NXT_ERROR;
    }

    if (mprotect(p + nxt_pagesize, nxt_pagesize, PROT_NONE) != 0) {
        nxt_log_alert(thr->log,
                      "accept-encoding name test: mprotect() failed %E",
                      nxt_errno);
        (void) munmap(p, 2 * nxt_pagesize);
        return NXT_ERROR;
    }

    ctx.thr = thr;
    ctx.end = p + nxt_pagesize;

    ret = nxt_test_in_child(thr, "accept-encoding name test",
                            nxt_http_comp_name_test_child, &ctx);

    (void) munmap(p, 2 * nxt_pagesize);

    NXT_TEST_CHECK(thr->log, ret == 0,
                   "accept-encoding name test failed: the child gave %d", ret);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "accept-encoding name test passed");

    return NXT_OK;
}


static nxt_int_t (*const nxt_security_tests[])(nxt_thread_t *) = {
    nxt_checked_test, nxt_port_mmap_read_test,
    nxt_router_response_parse_test, nxt_port_frag_test,
    nxt_port_release_test, nxt_process_use_test, nxt_nncq_bound_test,
    nxt_nncq_wrap_test,
    nxt_app_queue_test, nxt_size_bound_test, nxt_mp_cluster_cache_test,
    nxt_casecmp_test, nxt_http_comp_name_test,
#if (NXT_HAVE_REGEX)
    nxt_regex_test,
#endif
    nxt_usdt_test,
};


/* The function is defined here to prevent inline optimizations. */
static nxt_bool_t
nxt_msec_less(nxt_msec_t first, nxt_msec_t second)
{
    return (nxt_msec_diff(first, second) < 0);
}


/*
 * A stored "shm" over UINT32_MAX must not wrap to its low 32 bits.  A stored
 * -1 is mapped to SIZE_MAX, so the { SIZE_MAX, UINT32_MAX } case covers it.
 */

static nxt_int_t
nxt_app_shm_limit_test(nxt_thread_t *thr)
{
    size_t      r;
    nxt_uint_t  i;

    static const struct {
        size_t  in;
        size_t  out;
    } cases[] = {
        { 0, 0 },
        { 10 * 1024 * 1024, 10 * 1024 * 1024 },
        { UINT32_MAX, UINT32_MAX },
#if (NXT_SIZE_T_SIZE > 4)
        { (size_t) UINT32_MAX + 1, UINT32_MAX },
        { (size_t) UINT32_MAX + 10 * 1024 * 1024, UINT32_MAX },
#endif
        { SIZE_MAX, UINT32_MAX },
    };

    for (i = 0; i < nxt_nitems(cases); i++) {
        r = nxt_app_shm_limit(cases[i].in);

        NXT_TEST_CHECK(thr->log, r == cases[i].out,
                       "app shm limit test failed: %uz gave %uz, not %uz",
                       cases[i].in, r, cases[i].out);
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "app shm limit test passed");

    return NXT_OK;
}


/*
 * The steps of the restart back-off (#596): none without a failure, then
 * 100 ms doubled per round, and 5 s from the seventh round on, however many
 * rounds there are.
 */

static nxt_int_t
nxt_app_backoff_delay_test(nxt_thread_t *thr)
{
    nxt_uint_t  i;
    nxt_msec_t  r;

    static const struct {
        nxt_uint_t  rounds;
        nxt_msec_t  delay;
    } cases[] = {
        { 0, 0 },
        { 1, 100 },
        { 2, 200 },
        { 3, 400 },
        { 4, 800 },
        { 5, 1600 },
        { 6, 3200 },
        { 7, 5000 },
        { 8, 5000 },
        { 33, 5000 },
        { 64, 5000 },
        { (nxt_uint_t) -1, 5000 },
    };

    for (i = 0; i < nxt_nitems(cases); i++) {
        r = nxt_router_app_backoff_delay(cases[i].rounds);

        NXT_TEST_CHECK(thr->log, r == cases[i].delay,
                       "app backoff delay test failed: %ui rounds gave %M ms, "
                       "not %M ms", cases[i].rounds, r, cases[i].delay);
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "app backoff delay test passed");

    return NXT_OK;
}


int nxt_cdecl
main(int argc, char **argv)
{
    nxt_uint_t    i;
    nxt_task_t    task;
    nxt_thread_t  *thr;

    if (nxt_lib_start("tests", argv, &environ) != NXT_OK) {
        return 1;
    }

    nxt_main_log.level = NXT_LOG_INFO;
    task.log  = &nxt_main_log;

    thr = nxt_thread();
    thr->task = &task;

#if (NXT_TEST_RTDTSC)

    if (nxt_process_argv[1] != NULL
        && memcmp(nxt_process_argv[1], "rbm", 3) == 0)
    {
        if (nxt_rbtree1_mb_start(thr) != NXT_OK) {
            return 1;
        }

        if (nxt_rbtree_mb_start(thr) != NXT_OK) {
            return 1;
        }

        if (nxt_lvlhsh_test(thr, 500 * 1000, 0) != NXT_OK) {
            return 1;
        }

        nxt_rbtree1_mb_insert(thr);
        nxt_rbtree_mb_insert(thr);

        if (nxt_lvlhsh_test(thr, 500 * 1000, 0) != NXT_OK) {
            return 1;
        }

        nxt_rbtree1_mb_delete(thr);
        nxt_rbtree_mb_delete(thr);

        return 0;
    }

#endif

    if (nxt_random_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_term_parse_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_msec_diff_test(thr, nxt_msec_less) != NXT_OK) {
        return 1;
    }

    if (nxt_rbtree_test(thr, 100 * 1000) != NXT_OK) {
        return 1;
    }

    if (nxt_rbtree_test(thr, 1000 * 1000) != NXT_OK) {
        return 1;
    }

    if (nxt_rbtree1_test(thr, 100 * 1000) != NXT_OK) {
        return 1;
    }

    if (nxt_rbtree1_test(thr, 1000 * 1000) != NXT_OK) {
        return 1;
    }

    if (nxt_mp_get_align_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_mp_test(thr, 100, 40000, 128 - 1) != NXT_OK) {
        return 1;
    }

    if (nxt_mp_test(thr, 100, 1000, 4096 - 1) != NXT_OK) {
        return 1;
    }

    if (nxt_mp_test(thr, 1000, 100, 64 * 1024 - 1) != NXT_OK) {
        return 1;
    }

    if (nxt_lvlhsh_test(thr, 2, 1) != NXT_OK) {
        return 1;
    }

    if (nxt_lvlhsh_test(thr, 100 * 1000, 1) != NXT_OK) {
        return 1;
    }

    if (nxt_lvlhsh_test(thr, 100 * 1000, 0) != NXT_OK) {
        return 1;
    }

    if (nxt_lvlhsh_test(thr, 1000 * 1000, 1) != NXT_OK) {
        return 1;
    }

    if (nxt_gmtime_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_sprintf_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_malloc_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_utf8_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_utf8_sanitize_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_punycode_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_http_parse_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_strverscmp_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_base64_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_var_escape_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_string_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_buf_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_h1proto_conn_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_h1proto_fields_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_http_chunk_parse_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_http_validate_host_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_http_request_body_alloc_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_http_comp_select_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_conf_json_depth_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_http_route_addr_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_conf_map_object_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_conf_map_bound_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_conf_member_lookup_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_fail_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_fd_event_change_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_signalfd_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_poll_engine_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_use_unless_zero_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_mmap_range_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_mmaps_max_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_mmap_size_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_ready_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_conn_close_idle_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_listen_event_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_conn_io_accept_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_runtime_idle_close_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_conn_counters_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_new_port_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_start_fail_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_start_fail_soak_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_start_proto_gone_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_proto_wedge_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_proto_death_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_start_timeout_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_conf_overlap_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_app_timeout_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_remove_pid_soak_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_detached_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_sender_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_stale_joint_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_websocket_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_prepare_msg_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_engine_port_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_router_access_log_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_main_start_process_reply_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_app_shm_limit_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_app_backoff_delay_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_main_file_store_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_main_store_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_proto_creating_wedge_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_main_whoami_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_main_remove_child_pid_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_main_port_gate_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_app_gate_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_change_file_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_ctrunc_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_fd_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_rpc_fd_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_queued_fd_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_doorbell_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_port_share_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_cloexec_test(thr) != NXT_OK) {
        return 1;
    }

    for (i = 0; i < nxt_nitems(nxt_security_tests); i++) {
        if (nxt_security_tests[i](thr) != NXT_OK) {
            return 1;
        }
    }

    if (nxt_conn_close_test(thr) != NXT_OK) {
        return 1;
    }

#if (NXT_HAVE_OPENSSL)
    if (nxt_openssl_server_init_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_openssl_log_level_test(thr) != NXT_OK) {
        return 1;
    }
#endif

#if (NXT_HAVE_CGROUP)
    if (nxt_cgroup_test(thr) != NXT_OK) {
        return 1;
    }
#endif

    if (nxt_controller_peer_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_cpu_limit_test(thr) != NXT_OK) {
        return 1;
    }

    if (nxt_process_nofile_test(thr) != NXT_OK) {
        return 1;
    }

#if (NXT_HAVE_CLONE_NEWUSER)
    if (nxt_clone_creds_test(thr) != NXT_OK) {
        return 1;
    }
#endif

#if (NXT_HAVE_ISOLATION_ROOTFS)
    if (nxt_isolation_mount_dst_test(thr) != NXT_OK) {
        return 1;
    }
#endif

    return 0;
}
