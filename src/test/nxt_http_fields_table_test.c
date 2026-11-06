
/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_main.h>
#include "nxt_tests.h"


/*
 * nxt_http_field_find() against a scan of the table names: a field finds a
 * name when the field has the hash of that name and the names are equal
 * by nxt_strcasestr_eq().  That is what the lvlhsh lookup gave.  The
 * lookups run in a child, and each field name ends where a PROT_NONE page
 * starts.
 */

#define NXT_HTTP_TABLE_TEST_NAMES    64
#define NXT_HTTP_TABLE_TEST_LENGTH   256
#define NXT_HTTP_TABLE_TEST_RANDOM   10000
#define NXT_HTTP_TABLE_TEST_CRAFTED  128
#define NXT_HTTP_TABLE_TEST_PROBE    8


typedef struct {
    nxt_thread_t            *thr;
    nxt_http_fields_hash_t  *hash;
    nxt_http_field_proc_t   *items;
    nxt_uint_t              count;
    uint16_t                keys[NXT_HTTP_TABLE_TEST_NAMES];
    u_char                  *end;
    nxt_uint_t              checks;
    uint32_t                seed;
} nxt_http_fields_table_test_t;


/* Token bytes in lower case: the hash of a name in them needs no fold. */
static const u_char  nxt_http_fields_table_test_tchar[] =
    "abcdefghijklmnopqrstuvwxyz0123456789-";


/*
 * nxt_http_fields_hash() makes one allocation, at "slots", and a process
 * never frees its tables.  The test frees its own tables, else they leak.
 */

static void
nxt_http_fields_table_test_destroy(nxt_http_fields_hash_t *hash)
{
    nxt_free(hash->slots);
    nxt_memzero(hash, sizeof(nxt_http_fields_hash_t));
}


static uint32_t
nxt_http_fields_table_test_rand(nxt_http_fields_table_test_t *ctx)
{
    uint32_t  x;

    /* xorshift32: the same sequence on every run and platform. */

    x = ctx->seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    ctx->seed = x;

    return x;
}


static uint16_t
nxt_http_fields_table_test_key(const u_char *p, size_t length)
{
    u_char    c;
    uint32_t  key;

    key = NXT_HTTP_FIELD_HASH_INIT;

    while (length-- != 0) {
        c = *p++;
        key = nxt_http_field_hash_char(key, nxt_lowcase(c));
    }

    return (uint16_t) nxt_http_field_hash_end(key);
}


static nxt_int_t
nxt_http_fields_table_test_use(nxt_http_fields_table_test_t *ctx,
    nxt_http_fields_hash_t *hash, nxt_http_field_proc_t *items,
    nxt_uint_t count)
{
    nxt_uint_t  i;

    NXT_TEST_CHECK(ctx->thr->log, count <= NXT_HTTP_TABLE_TEST_NAMES,
                   "http fields table test: %ui names", count);

    ctx->hash = hash;
    ctx->items = items;
    ctx->count = count;

    for (i = 0; i < count; i++) {
        /* A name one byte longer must still fit in name_length. */

        NXT_TEST_CHECK(ctx->thr->log,
                       items[i].name.length < NXT_HTTP_TABLE_TEST_LENGTH - 1,
                       "http fields table test: name %ui too long", i);

        ctx->keys[i] = nxt_http_fields_table_test_key(items[i].name.start,
                                                      items[i].name.length);
    }

    return NXT_OK;
}


/* One lookup of a field with the name and the hash given. */

static nxt_int_t
nxt_http_fields_table_test_one(nxt_http_fields_table_test_t *ctx,
    const u_char *name, size_t length, uint16_t key)
{
    nxt_str_t              str;
    nxt_uint_t             i;
    nxt_http_field_t       field;
    nxt_http_field_proc_t  *got, *want;

    static nxt_str_t  nothing = nxt_string("(nothing)");

    nxt_memzero(&field, sizeof(nxt_http_field_t));

    field.hash = key;
    field.name_length = (uint8_t) length;
    field.name = ctx->end - length;

    nxt_memcpy(field.name, name, length);

    str.length = length;
    str.start = field.name;

    want = NULL;

    for (i = 0; i < ctx->count; i++) {
        if (ctx->keys[i] == key
            && nxt_strcasestr_eq(&str, &ctx->items[i].name))
        {
            want = &ctx->items[i];
            break;
        }
    }

    got = nxt_http_field_find(&field, ctx->hash);

    ctx->checks++;

    if (nxt_slow_path(got != want)) {
        nxt_log_alert(ctx->thr->log, "http fields table test: \"%*s\" "
                      "(hash %ud) found \"%V\", expected \"%V\"",
                      length, field.name, (unsigned) key,
                      (got != NULL) ? &got->name : &nothing,
                      (want != NULL) ? &want->name : &nothing);
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * Each table name as it is, in lower case and in upper case.  The name with
 * every hash, as an application can send any hash.  The name one byte
 * shorter and longer.  Every byte value at every position, with the hash of
 * the new name and with the hash of the table name.  Then an empty name
 * with every hash: a free slot must find nothing.
 */

static nxt_int_t
nxt_http_fields_table_test_names(nxt_http_fields_table_test_t *ctx)
{
    u_char      *name, buf[NXT_HTTP_TABLE_TEST_LENGTH];
    size_t      length;
    uint32_t    h;
    nxt_uint_t  i, j, b, form;

    for (i = 0; i < ctx->count; i++) {
        name = ctx->items[i].name.start;
        length = ctx->items[i].name.length;

        for (form = 0; form < 3; form++) {
            for (j = 0; j < length; j++) {
                buf[j] = (form == 0) ? name[j]
                         : (form == 1) ? nxt_lowcase(name[j])
                                       : nxt_upcase(name[j]);
            }

            if (nxt_http_fields_table_test_one(ctx, buf, length, ctx->keys[i])
                != NXT_OK)
            {
                return NXT_ERROR;
            }
        }

        for (h = 0; h <= 0xFFFF; h++) {
            if (nxt_http_fields_table_test_one(ctx, name, length, (uint16_t) h)
                != NXT_OK)
            {
                return NXT_ERROR;
            }
        }

        nxt_memcpy(buf, name, length);
        buf[length] = 's';

        if (nxt_http_fields_table_test_one(ctx, buf, length - 1,
                                           ctx->keys[i]) != NXT_OK
            || nxt_http_fields_table_test_one(ctx, buf, length - 1,
                   nxt_http_fields_table_test_key(buf, length - 1)) != NXT_OK
            || nxt_http_fields_table_test_one(ctx, buf, length + 1,
                                              ctx->keys[i]) != NXT_OK
            || nxt_http_fields_table_test_one(ctx, buf, length + 1,
                   nxt_http_fields_table_test_key(buf, length + 1)) != NXT_OK)
        {
            return NXT_ERROR;
        }

        for (j = 0; j < length; j++) {
            for (b = 0; b < 256; b++) {
                nxt_memcpy(buf, name, length);
                buf[j] = (u_char) b;

                if (nxt_http_fields_table_test_one(ctx, buf, length,
                        nxt_http_fields_table_test_key(buf, length)) != NXT_OK
                    || nxt_http_fields_table_test_one(ctx, buf, length,
                                                      ctx->keys[i]) != NXT_OK)
                {
                    return NXT_ERROR;
                }
            }
        }
    }

    for (h = 0; h <= 0xFFFF; h++) {
        if (nxt_http_fields_table_test_one(ctx, buf, 0, (uint16_t) h)
            != NXT_OK)
        {
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


/*
 * Random names of 1 to 32 token bytes in random case.  Every other one is a
 * table name with the case of its letters flipped at random, and now and
 * then a byte changed.
 */

static nxt_int_t
nxt_http_fields_table_test_random(nxt_http_fields_table_test_t *ctx)
{
    u_char        c, *name, buf[NXT_HTTP_TABLE_TEST_LENGTH];
    size_t        j, nt, length;
    uint16_t      key;
    uint32_t      r;
    nxt_uint_t    i, n;
    const u_char  *tc;

    tc = nxt_http_fields_table_test_tchar;
    nt = sizeof(nxt_http_fields_table_test_tchar) - 1;

    for (n = 0; n < NXT_HTTP_TABLE_TEST_RANDOM; n++) {
        r = nxt_http_fields_table_test_rand(ctx);

        if (r & 1) {
            length = 1 + (r >> 1) % 32;

            for (j = 0; j < length; j++) {
                r = nxt_http_fields_table_test_rand(ctx);
                c = tc[r % nt];
                buf[j] = ((r >> 16) & 1) ? nxt_upcase(c) : c;
            }

        } else {
            i = (r >> 1) % ctx->count;

            name = ctx->items[i].name.start;
            length = ctx->items[i].name.length;

            for (j = 0; j < length; j++) {
                r = nxt_http_fields_table_test_rand(ctx);
                c = name[j];

                if (r & 1) {
                    c = ((r >> 1) & 1) ? nxt_upcase(c) : nxt_lowcase(c);
                }

                if ((r >> 8) % 16 == 0) {
                    c = (u_char) (r >> 16);
                }

                buf[j] = c;
            }
        }

        key = nxt_http_fields_table_test_key(buf, length);

        if (nxt_http_fields_table_test_one(ctx, buf, length, key) != NXT_OK) {
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


/*
 * Writes "n" names of "length" bytes to "out" that have the hash "key" and
 * are not equal to "name" or to each other, ignoring case.  The first
 * length - 2 bytes are random, and the last two are searched.  Each name
 * gets a new prefix.
 */

static nxt_int_t
nxt_http_fields_table_test_craft(nxt_http_fields_table_test_t *ctx,
    const u_char *name, size_t length, uint16_t key, u_char *out,
    nxt_uint_t n)
{
    u_char        *p, *q;
    size_t        j;
    uint32_t      h, h0;
    nxt_uint_t    a, nt, tries, found;
    const u_char  *tc;

    NXT_TEST_CHECK(ctx->thr->log, length >= 2,
                   "http fields table test: no crafted names of %uz bytes",
                   length);

    tc = nxt_http_fields_table_test_tchar;
    nt = sizeof(nxt_http_fields_table_test_tchar) - 1;
    found = 0;
    p = out;

    for (tries = 0; found < n && tries < 100000; tries++) {
        h0 = NXT_HTTP_FIELD_HASH_INIT;

        for (j = 0; j + 2 < length; j++) {
            p[j] = tc[nxt_http_fields_table_test_rand(ctx) % nt];
            h0 = nxt_http_field_hash_char(h0, p[j]);
        }

        for (a = 0; a < nt * nt; a++) {
            h = nxt_http_field_hash_char(h0, tc[a / nt]);
            h = nxt_http_field_hash_char(h, tc[a % nt]);

            if ((uint16_t) nxt_http_field_hash_end(h) != key) {
                continue;
            }

            p[length - 2] = tc[a / nt];
            p[length - 1] = tc[a % nt];

            if (nxt_memcasecmp(p, name, length) == 0) {
                continue;
            }

            for (q = out; q < p; q += length) {
                if (nxt_memcasecmp(p, q, length) == 0) {
                    break;
                }
            }

            if (q == p) {
                found++;
                p += length;
            }

            break;
        }
    }

    NXT_TEST_CHECK(ctx->thr->log, found == n,
                   "http fields table test: %ui crafted names for \"%*s\"",
                   found, length, name);

    return NXT_OK;
}


/*
 * Names with the length and the 16-bit hash of a table name and other
 * bytes, 4 for each table name of 4 or more bytes, as they are and in upper
 * case.  Only the compare of the names refuses them.
 */

static nxt_int_t
nxt_http_fields_table_test_crafted(nxt_http_fields_table_test_t *ctx)
{
    u_char      *p, *name, up[NXT_HTTP_TABLE_TEST_LENGTH];
    u_char      buf[4 * NXT_HTTP_TABLE_TEST_LENGTH];
    size_t      j, length;
    nxt_uint_t  i, k, n;

    n = 0;

    for (i = 0; i < ctx->count && n < NXT_HTTP_TABLE_TEST_CRAFTED; i++) {
        name = ctx->items[i].name.start;
        length = ctx->items[i].name.length;

        if (length < 4) {
            continue;
        }

        if (nxt_http_fields_table_test_craft(ctx, name, length, ctx->keys[i],
                                             buf, 4)
            != NXT_OK)
        {
            return NXT_ERROR;
        }

        for (k = 0; k < 4 && n < NXT_HTTP_TABLE_TEST_CRAFTED; k++, n++) {
            p = buf + k * length;

            for (j = 0; j < length; j++) {
                up[j] = nxt_upcase(p[j]);
            }

            if (nxt_http_fields_table_test_one(ctx, p, length, ctx->keys[i])
                != NXT_OK
                || nxt_http_fields_table_test_one(ctx, up, length, ctx->keys[i])
                   != NXT_OK)
            {
                return NXT_ERROR;
            }
        }
    }

    NXT_TEST_CHECK(ctx->thr->log, n == NXT_HTTP_TABLE_TEST_CRAFTED,
                   "http fields table test: %ui crafted names", n);

    return NXT_OK;
}


/*
 * Tables that nxt_http_fields_hash() must refuse: a name twice ignoring
 * case, an empty name, a name longer than a field name can be, and a table
 * that is already built.  A table whose names share one hash, so that a
 * lookup must read more slots.  An empty table.
 */

static nxt_int_t
nxt_http_fields_table_test_tables(nxt_http_fields_table_test_t *ctx)
{
    u_char                  long_name[256];
    u_char                  coll[(NXT_HTTP_TABLE_TEST_PROBE + 1) * 4];
    uint16_t                key;
    nxt_int_t               rc;
    nxt_uint_t              i, k;
    nxt_http_field_slot_t   *slots;
    nxt_http_fields_hash_t  hash;
    nxt_http_field_proc_t   probe[NXT_HTTP_TABLE_TEST_PROBE + 2];

    static nxt_str_t  host = nxt_string("Host");

    static nxt_http_field_proc_t  dup1[] = {
        { nxt_string("Host"), &nxt_http_test_header_return, 0 },
        { nxt_string("Accept"), &nxt_http_test_header_return, 0 },
        { nxt_string("hoST"), &nxt_http_test_header_return, 0 },
    };

    static nxt_http_field_proc_t  dup2[] = {
        { nxt_string("Content-Length"), &nxt_http_test_header_return, 0 },
        { nxt_string("CONTENT-LENGTH"), &nxt_http_test_header_return, 0 },
    };

    static nxt_http_field_proc_t  empty[] = {
        { nxt_string("Host"), &nxt_http_test_header_return, 0 },
        { nxt_string(""), &nxt_http_test_header_return, 0 },
    };

    nxt_http_field_proc_t  too_long[] = {
        { { sizeof(long_name), long_name },
          &nxt_http_test_header_return, 0 },
    };

    struct {
        nxt_http_field_proc_t  *items;
        nxt_uint_t             count;
    } refused[] = {
        { dup1, nxt_nitems(dup1) },
        { dup2, nxt_nitems(dup2) },
        { empty, nxt_nitems(empty) },
        { too_long, nxt_nitems(too_long) },
    };

    nxt_memset(long_name, 'a', sizeof(long_name));
    nxt_memzero(&hash, sizeof(nxt_http_fields_hash_t));

    for (i = 0; i < nxt_nitems(refused); i++) {
        rc = nxt_http_fields_hash(&hash, refused[i].items, refused[i].count);

        NXT_TEST_CHECK(ctx->thr->log, rc == NXT_ERROR && hash.slots == NULL
                                      && hash.probes == 0,
                       "http fields table test: table %ui was not refused", i);
    }

    /* "Host" and 8 names with its hash: 7 in the table, 1 not. */

    key = nxt_http_fields_table_test_key(host.start, host.length);

    if (nxt_http_fields_table_test_craft(ctx, host.start, host.length, key,
                                         coll, NXT_HTTP_TABLE_TEST_PROBE)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    nxt_memcpy(&coll[NXT_HTTP_TABLE_TEST_PROBE * 4], host.start, host.length);

    for (k = 0; k < NXT_HTTP_TABLE_TEST_PROBE; k++) {
        probe[k].name.length = 4;
        probe[k].name.start = &coll[(k + 1) * 4];
        probe[k].handler = &nxt_http_test_header_return;
        probe[k].data = k;
    }

    nxt_str_set(&probe[k].name, "Cookie");
    probe[k].handler = &nxt_http_test_header_return;
    probe[k].data = k;
    k++;

    nxt_str_set(&probe[k].name, "Accept");
    probe[k].handler = &nxt_http_test_header_return;
    probe[k].data = k;
    k++;

    rc = nxt_http_fields_hash(&hash, probe, k);

    NXT_TEST_CHECK(ctx->thr->log, rc == NXT_OK
                                  && hash.probes >= NXT_HTTP_TABLE_TEST_PROBE
                                  && nxt_http_fields_hash_collisions(&hash)
                                     >= NXT_HTTP_TABLE_TEST_PROBE - 1,
                   "http fields table test: probe table rc %i, probes %ui",
                   rc, (nxt_uint_t) hash.probes);

    /* A table that is already built is refused and stays as it is. */

    slots = hash.slots;

    NXT_TEST_CHECK(ctx->thr->log,
                   nxt_http_fields_hash(&hash, probe, k) == NXT_ERROR
                   && hash.slots == slots,
                   "http fields table test: a built table was built again");

    if (nxt_http_fields_table_test_use(ctx, &hash, probe, k) != NXT_OK) {
        nxt_http_fields_table_test_destroy(&hash);
        return NXT_ERROR;
    }

    for (i = 0; i < k; i++) {
        if (nxt_http_fields_table_test_one(ctx, probe[i].name.start,
                                           probe[i].name.length, ctx->keys[i])
            != NXT_OK)
        {
            nxt_http_fields_table_test_destroy(&hash);
            return NXT_ERROR;
        }
    }

    /* The first crafted name is not in the table. */

    rc = nxt_http_fields_table_test_one(ctx, coll, 4, ctx->keys[0]);

    nxt_http_fields_table_test_destroy(&hash);

    if (rc != NXT_OK) {
        return NXT_ERROR;
    }

    /* An empty table is all zero, as a table that was never built. */

    rc = nxt_http_fields_hash(&hash, probe, 0);

    NXT_TEST_CHECK(ctx->thr->log, rc == NXT_OK && hash.slots == NULL
                                  && hash.probes == 0,
                   "http fields table test: empty table rc %i", rc);

    if (nxt_http_fields_table_test_use(ctx, &hash, probe, 0) != NXT_OK
        || nxt_http_fields_table_test_one(ctx, host.start, host.length, key)
           != NXT_OK)
    {
        return NXT_ERROR;
    }

    return NXT_OK;
}


static int
nxt_http_fields_table_test_child(void *data)
{
    u_char                        *p;
    nxt_int_t                     rc;
    nxt_http_fields_hash_t        hash;
    nxt_http_fields_table_test_t  *ctx;

    ctx = data;

    p = mmap(NULL, 2 * nxt_pagesize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | NXT_MEM_MAP_ANON, -1, 0);
    if (p == MAP_FAILED || mprotect(p + nxt_pagesize, nxt_pagesize, PROT_NONE)
                           != 0)
    {
        return 2;
    }

    ctx->end = p + nxt_pagesize;

    nxt_memzero(&hash, sizeof(nxt_http_fields_hash_t));

    if (nxt_http_fields_hash(&hash, nxt_http_test_bench_fields,
                             nxt_http_test_bench_nfields)
        != NXT_OK)
    {
        return 2;
    }

    rc = nxt_http_fields_table_test_use(ctx, &hash, nxt_http_test_bench_fields,
                                        nxt_http_test_bench_nfields);

    if (rc == NXT_OK) {
        rc = nxt_http_fields_table_test_names(ctx);
    }

    if (rc == NXT_OK) {
        rc = nxt_http_fields_table_test_random(ctx);
    }

    if (rc == NXT_OK) {
        rc = nxt_http_fields_table_test_crafted(ctx);
    }

    nxt_http_fields_table_test_destroy(&hash);

    if (rc == NXT_OK) {
        rc = nxt_http_fields_table_test_tables(ctx);
    }

    if (rc != NXT_OK) {
        return 1;
    }

    nxt_log_error(NXT_LOG_NOTICE, ctx->thr->log,
                  "http fields table test: %ui lookups", ctx->checks);

    return 0;
}


nxt_int_t
nxt_http_fields_table_test(nxt_thread_t *thr)
{
    int                           rc;
    nxt_http_fields_table_test_t  ctx;

    nxt_memzero(&ctx, sizeof(nxt_http_fields_table_test_t));

    ctx.thr = thr;
    ctx.seed = 0x9e3779b9;

    rc = nxt_test_in_child(thr, "http fields table test",
                           nxt_http_fields_table_test_child, &ctx);

    NXT_TEST_CHECK(thr->log, rc == 0,
                   "http fields table test failed (child status %d)", rc);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "http fields table test passed");

    return NXT_OK;
}
