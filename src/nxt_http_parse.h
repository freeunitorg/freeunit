
/*
 * Copyright (C) NGINX, Inc.
 * Copyright (C) Valentin V. Bartenev
 */

#ifndef _NXT_HTTP_PARSER_H_INCLUDED_
#define _NXT_HTTP_PARSER_H_INCLUDED_


typedef enum {
    NXT_HTTP_PARSE_INVALID = 1,
    NXT_HTTP_PARSE_UNSUPPORTED_VERSION,
    NXT_HTTP_PARSE_TOO_LARGE_FIELD,
} nxt_http_parse_error_t;


typedef struct nxt_http_request_parse_s  nxt_http_request_parse_t;
typedef struct nxt_http_field_s          nxt_http_field_t;
typedef struct nxt_http_fields_hash_s    nxt_http_fields_hash_t;


struct nxt_http_field_s {
    uint16_t                  hash;
    uint8_t                   skip:1;
    uint8_t                   hopbyhop:1;
    /*
     * Only nxt_router_prepare_msg() uses it: it marks a request line that
     * it joins to an earlier line of the same name, and it clears the mark
     * before it returns.
     */
    uint8_t                   merged:1;
    uint8_t                   name_length;
    uint32_t                  value_length;
    u_char                    *name;
    u_char                    *value;
};


typedef union {
    u_char                    str[8] NXT_NONSTRING;
    uint64_t                  ui64;

    struct {
        u_char                prefix[5];
        u_char                major;
        u_char                point;
        u_char                minor;
    } s;
} nxt_http_ver_t;


struct nxt_http_request_parse_s {
    nxt_int_t                 (*handler)(nxt_http_request_parse_t *rp,
                                         u_char **pos, const u_char *end);

    nxt_str_t                 method;

    u_char                    *target_start;
    u_char                    *target_end;
    u_char                    *request_line_end;

    nxt_str_t                 path;
    nxt_str_t                 args;

    nxt_http_ver_t            version;

    nxt_http_field_t          inline_fields[16];

    nxt_list_t                *fields;
    nxt_mp_t                  *mem_pool;

    nxt_str_t                 field_name;
    nxt_str_t                 field_value;

    uint32_t                  field_hash;

    uint8_t                   skip_field;             /* 1 bit */
    uint8_t                   discard_unsafe_fields;  /* 1 bit */
    /* check the fields, but do not store them */
    uint8_t                   discard_fields;         /* 1 bit */

    /* target with "/." */
    uint8_t                   complex_target;         /* 1 bit */
    /* target with "%" */
    uint8_t                   quoted_target;          /* 1 bit */
    /* Preserve encoded '/' (%2F) and '%' (%25). */
    uint8_t                   encoded_slashes;        /* 1 bit */

    /*
     * After the flags, not in front of "fields": a byte there leaves seven
     * bytes of padding.
     */
    uint8_t                   num_inline_fields;
};


typedef nxt_int_t (*nxt_http_field_handler_t)(void *ctx,
                                              nxt_http_field_t *field,
                                              uintptr_t data);


typedef struct {
    nxt_str_t                 name;
    nxt_http_field_handler_t  handler;
    uintptr_t                 data;
} nxt_http_field_proc_t;


/*
 * A slot of nxt_http_fields_hash_t.  "low" holds the name in lower case and
 * then its fold mask, as nxt_casefold_set() writes them.  A free slot is all
 * zero: only an empty name with the hash 0 is equal to it, and that finds
 * no proc, as a name that is not in the table.
 */

typedef struct {
    uint16_t                  hash;
    uint8_t                   length;
    const u_char              *low;
    nxt_http_field_proc_t     *proc;
} nxt_http_field_slot_t;


/*
 * A table of field names, built once by nxt_http_fields_hash() and never
 * freed.  The table must be all zero when it is built, as a static table
 * is: nxt_http_fields_hash() refuses a table that is already built, because
 * a second build would leak the first.  The first slot of the hash "h" is
 * (h * mult) >> shift.  When it is taken, the name goes to the next free
 * slot (linear probing).  The multiplier is chosen so that few or no names
 * leave their first slot.  A lookup reads at most "probes" slots.  A table
 * that is all zero is empty, and a lookup in it reads no slot.
 */

struct nxt_http_fields_hash_s {
    nxt_http_field_slot_t     *slots;
    uint32_t                  mask;
    uint32_t                  mult;
    uint32_t                  shift;
    uint32_t                  probes;
};


#define nxt_http_fields_hash_slot(hash, key)                                  \
    (((uint32_t) (key) * (hash)->mult) >> (hash)->shift)


typedef struct {
    nxt_mp_t                  *mem_pool;
    uint64_t                  chunk_size;
    uint8_t                   state;
    uint8_t                   last;         /* 1 bit */
    uint8_t                   trailer;      /* 1 bit */
    uint8_t                   chunk_error;  /* 1 bit */
    uint8_t                   error;        /* 1 bit */
    /*
     * The caller owns the input buffers and consumes the returned slice chain
     * synchronously without running slice completion handlers, so
     * nxt_http_chunk_parse() must neither retain mem_pool/input buffers per
     * slice nor schedule input buffer completion handlers.  Set by the request
     * path and unit tests; left zero by the proxy response path.
     */
    uint8_t                   retain_buffers;  /* 1 bit */
} nxt_http_chunk_parse_t;


#define NXT_HTTP_FIELD_HASH_INIT        159406U
#define nxt_http_field_hash_char(h, c)  (((h) << 4) + (h) + (c))
#define nxt_http_field_hash_end(h)      (((h) >> 16) ^ (h))


typedef struct {
    nxt_http_field_t          *inline_fields;
    uint8_t                   num_inline_fields;
    uint8_t                   inline_index;
    nxt_list_t                *fields;
    nxt_list_part_t           *part;
    uintptr_t                 elt;
} nxt_http_fields_iter_t;


nxt_inline nxt_http_field_t *
nxt_http_fields_first(nxt_http_fields_iter_t *iter,
    nxt_http_field_t *inline_fields, uint8_t num_inline_fields,
    nxt_list_t *fields)
{
    iter->inline_fields = inline_fields;
    iter->num_inline_fields = num_inline_fields;
    iter->inline_index = 0;
    iter->fields = fields;
    iter->part = (fields != NULL) ? nxt_list_part(fields) : NULL;
    iter->elt = 0;

    if (iter->inline_index < iter->num_inline_fields) {
        return &iter->inline_fields[iter->inline_index++];
    }

    while (iter->part != NULL) {
        if (iter->part->nelts > 0) {
            iter->elt = 1;
            return (nxt_http_field_t *) nxt_list_data(iter->part);
        }

        iter->part = iter->part->next;
    }

    return NULL;
}


nxt_inline nxt_http_field_t *
nxt_http_fields_next(nxt_http_fields_iter_t *iter)
{
    nxt_http_field_t  *f;

    if (iter->inline_index < iter->num_inline_fields) {
        return &iter->inline_fields[iter->inline_index++];
    }

    while (iter->part != NULL) {
        if (iter->elt < iter->part->nelts) {
            f = (nxt_http_field_t *) nxt_list_data(iter->part) + iter->elt;
            iter->elt++;
            return f;
        }

        iter->part = iter->part->next;
        iter->elt = 0;
    }

    return NULL;
}


/*
 * Iterate the inline array then the spill list.
 *
 * NOTE: unlike nxt_list_each(), which nests a per-part loop inside a per-list
 * loop, this is a single flat loop -- so "break" leaves the iteration entirely
 * rather than only the current part and resuming at the next one.  "continue"
 * and "return" behave the same in both.  When converting a remaining
 * nxt_list_each() over a field store, re-check any "break" in its body.
 */
#define nxt_http_fields_each(field, inline_fields, num_inline_fields, fields) \
    do {                                                                      \
        nxt_http_fields_iter_t  _iter;                                        \
        for (field = nxt_http_fields_first(&_iter,                            \
                                            (nxt_http_field_t *) (inline_fields),\
                                            num_inline_fields, fields);       \
             field != NULL;                                                   \
             field = nxt_http_fields_next(&_iter))                            \
        {

#define nxt_http_fields_loop                                                  \
        }                                                                     \
    } while (0)


nxt_int_t nxt_http_parse_request_init(nxt_http_request_parse_t *rp,
    nxt_mp_t *mp);
nxt_int_t nxt_http_parse_request(nxt_http_request_parse_t *rp,
    nxt_buf_mem_t *b);
nxt_int_t nxt_http_parse_fields(nxt_http_request_parse_t *rp,
    nxt_buf_mem_t *b);

nxt_int_t nxt_http_fields_hash(nxt_http_fields_hash_t *hash,
    nxt_http_field_proc_t items[], nxt_uint_t count);
nxt_uint_t nxt_http_fields_hash_collisions(const nxt_http_fields_hash_t *hash);
nxt_int_t nxt_http_fields_process(nxt_http_field_t *inline_fields,
    uint8_t num_inline_fields, nxt_list_t *fields,
    const nxt_http_fields_hash_t *hash, void *ctx);

nxt_int_t nxt_http_parse_complex_target(nxt_http_request_parse_t *rp);
nxt_buf_t *nxt_http_chunk_parse(nxt_task_t *task, nxt_http_chunk_parse_t *hcp,
    nxt_buf_t *in);


/*
 * The proc of the field name, or NULL.  A slot matches when its hash and
 * its length are those of the field and the names are equal ignoring case.
 * The name is compared on every match of the hash: the hash has 16 bits, a
 * client chooses the names, and an application chooses the hash of each
 * response field.
 */

nxt_inline nxt_http_field_proc_t *
nxt_http_field_find(const nxt_http_field_t *field,
    const nxt_http_fields_hash_t *hash)
{
    uint32_t               i, n;
    nxt_http_field_slot_t  *slot;

    i = nxt_http_fields_hash_slot(hash, field->hash);

    for (n = hash->probes; n != 0; n--) {
        slot = &hash->slots[i];

        if (slot->hash == field->hash
            && slot->length == field->name_length
            && nxt_memcase_eq_fold(field->name, slot->low, slot->length))
        {
            return slot->proc;
        }

        i = (i + 1) & hash->mask;
    }

    return NULL;
}


nxt_inline nxt_int_t
nxt_http_field_process(nxt_http_field_t *field,
    const nxt_http_fields_hash_t *hash, void *ctx)
{
    nxt_http_field_proc_t  *proc;

    proc = nxt_http_field_find(field, hash);

    if (proc == NULL) {
        return NXT_OK;
    }

    return proc->handler(ctx, field, proc->data);
}


#endif /* _NXT_HTTP_PARSER_H_INCLUDED_ */
