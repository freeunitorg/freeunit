
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_TLS_H_INCLUDED_
#define _NXT_TLS_H_INCLUDED_


#include <nxt_conf.h>


/*
 * The SSL/TLS libraries lack vector I/O interface yet add noticeable
 * overhead to each SSL/TLS record so buffering allows to decrease the
 * overhead.  The typical overhead size is about 30 bytes, however, TLS
 * supports also random padding up to 255 bytes.  The maximum SSLv3/TLS
 * record size is 16K.  However, large records increase decryption latency.
 * 4K is good compromise between 1-6% of SSL/TLS overhead and the latency.
 * 4K buffer allows to send one SSL/TLS record (4096-bytes data and up to
 * 224-bytes overhead) in three 1440-bytes TCP/IPv4 packets with timestamps
 * and compatible with tunnels.
 */

#define NXT_TLS_BUFFER_SIZE       4096

/* The most keys "tickets" can list when "ticket_lifetime" rotates them. */
#define NXT_TLS_TICKET_KEYS_MAX   8


typedef struct nxt_tls_conf_s         nxt_tls_conf_t;
typedef struct nxt_tls_bundle_conf_s  nxt_tls_bundle_conf_t;
typedef struct nxt_tls_init_s         nxt_tls_init_t;
typedef struct nxt_tls_ticket_s       nxt_tls_ticket_t;
typedef struct nxt_tls_tickets_s      nxt_tls_tickets_t;

typedef struct {
    nxt_int_t                     (*library_init)(nxt_task_t *task);
    void                          (*library_free)(nxt_task_t *task);

    nxt_int_t                     (*server_init)(nxt_task_t *task, nxt_mp_t *mp,
                                      nxt_tls_init_t *tls_init,
                                      nxt_bool_t last);
    void                          (*server_free)(nxt_task_t *task,
                                      nxt_tls_conf_t *conf);
} nxt_tls_lib_t;


typedef struct {
    nxt_tls_bundle_conf_t         *bundle;

    nxt_str_t                     name;
} nxt_tls_bundle_hash_item_t;


struct nxt_tls_bundle_conf_s {
    void                          *ctx;

    nxt_fd_t                      chain_file;
    nxt_str_t                     name;

    nxt_tls_bundle_conf_t         *next;
};


struct nxt_tls_conf_s {
    nxt_tls_bundle_conf_t         *bundle;
    nxt_lvlhsh_t                  bundle_hash;

    nxt_tls_tickets_t             *tickets;

    void                          (*conn_init)(nxt_task_t *task,
                                      nxt_tls_conf_t *conf, nxt_conn_t *c);

    const nxt_tls_lib_t           *lib;

    char                          *ciphers;

    char                          *ca_certificate;

    size_t                        buffer_size;

    uint8_t                       no_wait_shutdown;  /* 1 bit */
};


struct nxt_tls_init_s {
    size_t                        cache_size;
    nxt_time_t                    timeout;
    nxt_conf_value_t              *conf_cmds;
    nxt_conf_value_t              *tickets_conf;
    /* Rotation of ticket keys: the seconds a key issues for; 0 is off. */
    nxt_time_t                    ticket_lifetime;
    /* The keys belong to the listener of this name. */
    nxt_str_t                     listener;

    nxt_tls_conf_t                *conf;
};


#if (NXT_HAVE_OPENSSL)
extern const nxt_tls_lib_t        nxt_openssl_lib;

void nxt_cdecl nxt_openssl_log_error(nxt_task_t *task, nxt_uint_t level,
    const char *fmt, ...);
u_char *nxt_openssl_copy_error(u_char *p, u_char *end);

#if (NXT_TESTS)
/*
 * Makes nxt_openssl_server_init() take one of its failure paths, for
 * src/test/nxt_openssl_server_init_test.c.
 */
#define NXT_OPENSSL_TEST_FAIL_CTX_NEW  1
#define NXT_OPENSSL_TEST_FAIL_BIO_NEW  2

NXT_EXPORT extern nxt_uint_t  nxt_openssl_test_fail;

NXT_EXPORT nxt_uint_t nxt_openssl_test_log_level(int reason);

/*
 * For src/test/nxt_openssl_server_init_test.c: the clock of the ticket key
 * rings in milliseconds, if not 0, and a key of the ring of a configuration.
 * With "issue" the key that issues now is returned in "name"; else the key
 * "name" is looked up: NXT_OK if it also issues, NXT_AGAIN if it only
 * decrypts, NXT_DECLINED if the ring does not have it.
 */
NXT_EXPORT extern uint64_t  nxt_openssl_test_ticket_msec;

NXT_EXPORT nxt_int_t nxt_openssl_test_ticket_key(nxt_task_t *task,
    nxt_tls_conf_t *conf, u_char *name, nxt_bool_t issue);
#endif
#endif

#endif /* _NXT_TLS_H_INCLUDED_ */
