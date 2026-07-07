/*
 * Copyright (C) Andrew Clayton
 * Copyright (C) F5, Inc.
 */

#include <nxt_main.h>
#include <nxt_application.h>
#include <nxt_unit.h>
#include <nxt_unit_request.h>

#include "nxt_wasm.h"


#define NXT_WASM_VERSION        "0.1"

#define NXT_WASM_DO_HOOK(hook)  nxt_wops->exec_hook(&nxt_wasm_ctx, hook);


static uint32_t  compat[] = {
    NXT_VERNUM, NXT_DEBUG,
};

static nxt_wasm_ctx_t               nxt_wasm_ctx;

static const nxt_wasm_operations_t  *nxt_wops;

enum {
    NXT_WASM_HTTP_OK     = 200,
    NXT_WASM_HTTP_ERROR  = 500
};


void
nxt_wasm_do_response_end(nxt_wasm_ctx_t *ctx)
{
    nxt_unit_request_done(ctx->req, NXT_UNIT_OK);

    NXT_WASM_DO_HOOK(NXT_WASM_FH_RESPONSE_END);
}


void
nxt_wasm_do_send_headers(nxt_wasm_ctx_t *ctx, uint32_t offset)
{
    size_t                      fields_len, fields_table_end;
    unsigned int                i;
    nxt_wasm_response_fields_t  *rh;

    /*
     * `offset`, `nfields`, and every (name_off, name_len, value_off,
     * value_len) tuple arrive from the guest WASM module.  Bound them
     * against the linear-memory size before any dereference.
     */
    if (offset > NXT_WASM_MEM_SIZE - sizeof(nxt_wasm_response_fields_t)) {
        nxt_unit_req_alert(ctx->req,
                           "WASM send_headers offset %u out of range", offset);
        return;
    }

    rh = (nxt_wasm_response_fields_t *)(ctx->baddr + offset);

    /*
     * Bound the fields[] table.  Each entry is nxt_wasm_http_field_t;
     * compute end-offset with overflow-safe arithmetic.
     */
    if (rh->nfields > (NXT_WASM_MEM_SIZE - offset
                       - sizeof(nxt_wasm_response_fields_t))
                      / sizeof(nxt_wasm_http_field_t))
    {
        nxt_unit_req_alert(ctx->req,
                           "WASM send_headers nfields=%u out of range",
                           rh->nfields);
        return;
    }
    fields_table_end = offset + sizeof(nxt_wasm_response_fields_t)
                       + (size_t) rh->nfields * sizeof(nxt_wasm_http_field_t);

    /*
     * Bound each (name, value) range in the guest's memory.  Field
     * offsets are guest-relative to `rh`, so the absolute offset in
     * linear memory is `offset + field_off`.
     *
     * Two additional caps:
     *   - `name_len` is passed to nxt_unit_response_add_field() whose
     *     `name_length` parameter is uint8_t — any name_len > 255 would
     *     silently truncate, splicing the next bytes of guest memory
     *     into the emitted header.  Reject up front.
     *   - `fields_len` is the aggregate sum of every name+value length;
     *     nothing stops a guest from pointing many fields at the same
     *     in-bounds region and inflating the sum (or overflowing it).
     *     Accumulate with overflow checks and cap to the linear-memory
     *     size: no legitimate response header table can exceed that.
     */
    fields_len = 0;
    for (i = 0; i < rh->nfields; i++) {
        uint32_t  name_off = rh->fields[i].name_off;
        uint32_t  name_len = rh->fields[i].name_len;
        uint32_t  val_off  = rh->fields[i].value_off;
        uint32_t  val_len  = rh->fields[i].value_len;
        size_t    abs_name = (size_t) offset + name_off;
        size_t    abs_val  = (size_t) offset + val_off;

        if (abs_name < fields_table_end
            || abs_name > NXT_WASM_MEM_SIZE
            || name_len > NXT_WASM_MEM_SIZE - abs_name
            || abs_val < fields_table_end
            || abs_val > NXT_WASM_MEM_SIZE
            || val_len > NXT_WASM_MEM_SIZE - abs_val)
        {
            nxt_unit_req_alert(ctx->req,
                "WASM send_headers field[%u] out of range "
                "(name_off=%u name_len=%u val_off=%u val_len=%u)",
                i, name_off, name_len, val_off, val_len);
            return;
        }

        if (name_len > UINT8_MAX) {
            nxt_unit_req_alert(ctx->req,
                "WASM send_headers field[%u] name_len=%u exceeds 255",
                i, name_len);
            return;
        }

        if (val_len > NXT_WASM_MEM_SIZE - fields_len
            || name_len > NXT_WASM_MEM_SIZE - fields_len - val_len)
        {
            nxt_unit_req_alert(ctx->req,
                "WASM send_headers aggregate header size exceeds %zu "
                "at field[%u]", (size_t) NXT_WASM_MEM_SIZE, i);
            return;
        }

        fields_len += name_len + val_len;
    }

    nxt_unit_response_init(ctx->req, ctx->status, rh->nfields, fields_len);

    for (i = 0; i < rh->nfields; i++) {
        const char  *name;
        const char  *val;

        name = (const char *)rh + rh->fields[i].name_off;
        val = (const char *)rh + rh->fields[i].value_off;

        nxt_unit_response_add_field(ctx->req, name, rh->fields[i].name_len,
                                    val, rh->fields[i].value_len);
    }

    nxt_unit_response_send(ctx->req);
}


void
nxt_wasm_do_send_response(nxt_wasm_ctx_t *ctx, uint32_t offset)
{
    nxt_wasm_response_t      *resp;
    nxt_unit_request_info_t  *req = ctx->req;

    if (!nxt_unit_response_is_init(req)) {
        nxt_unit_response_init(req, ctx->status, 0, 0);
    }

    /*
     * `offset` arrives from the guest WASM module; bound it against
     * the linear-memory size before dereferencing.
     */
    if (offset > NXT_WASM_MEM_SIZE - sizeof(nxt_wasm_response_t)) {
        nxt_unit_req_alert(req,
                           "WASM send_response offset %u out of range", offset);
        return;
    }

    resp = (nxt_wasm_response_t *)(nxt_wasm_ctx.baddr + offset);

    if (resp->size > NXT_WASM_MEM_SIZE
        - offset - offsetof(nxt_wasm_response_t, data))
    {
        nxt_unit_req_alert(req, "WASM send_response size %u out of range",
                           resp->size);
        return;
    }

    nxt_unit_response_write(req, (const char *)resp->data, resp->size);
}


static void
nxt_wasm_request_handler(nxt_unit_request_info_t *req)
{
    int                    err;
    size_t                 offset, read_bytes, content_sent, content_len;
    ssize_t                bytes_read;
    nxt_unit_field_t       *sf, *sf_end;
    nxt_unit_request_t     *r;
    nxt_wasm_request_t     *wr;
    nxt_wasm_http_field_t  *df;

    /*
     * Publish the in-flight request on the shared ctx *before* any
     * hook or guarded jump so REQUEST_INIT / REQUEST_END always see
     * the right request — including the bounds-failure paths that
     * goto request_done before reaching the body-read block below.
     * Without this, REQUEST_END would fire with a stale (or NULL)
     * ctx->req on the first guarded exit of any worker.
     */
    nxt_wasm_ctx.req = req;

    NXT_WASM_DO_HOOK(NXT_WASM_FH_REQUEST_INIT);

    wr = (nxt_wasm_request_t *)nxt_wasm_ctx.baddr;

    /*
     * Each request field is copied into the WASM linear memory at the
     * running `offset`.  Without bounds checks, an unusually large
     * request (many headers, oversized header values) can drive the
     * memcpy past the end of the 32 MB sandbox region.
     */
    /*
     * Route bounds failures through the request_done label so the
     * REQUEST_END hook still fires — modules tracking per-request
     * state in request_init_handler need their matching cleanup.
     */
#define WASM_OFFSET_GUARD(need)                                                \
    do {                                                                       \
        if (offset > NXT_WASM_MEM_SIZE - 1                                     \
            || (size_t)(need) > NXT_WASM_MEM_SIZE - 1 - offset)                \
        {                                                                      \
            nxt_unit_req_alert(req,                                            \
                "WASM request buffer overflow at offset %zu", offset);         \
            nxt_unit_request_done(req, NXT_UNIT_ERROR);                        \
            goto request_done;                                                 \
        }                                                                      \
    } while (0)

#define SET_REQ_MEMBER(dmember, smember) \
    do { \
        const char *str = nxt_unit_sptr_get(&r->smember); \
        size_t      slen = strlen(str); \
        WASM_OFFSET_GUARD(slen); \
        wr->dmember##_off = offset; \
        wr->dmember##_len = slen; \
        memcpy((uint8_t *)wr + offset, str, slen + 1); \
        offset += slen + 1; \
    } while (0)

    r = req->request;
    offset = sizeof(nxt_wasm_request_t)
             + (r->fields_count * sizeof(nxt_wasm_http_field_t));

    if (offset > NXT_WASM_MEM_SIZE) {
        nxt_unit_req_alert(req, "WASM request header table exceeds linear "
                           "memory: fields_count=%u", r->fields_count);
        nxt_unit_request_done(req, NXT_UNIT_ERROR);
        goto request_done;
    }

    SET_REQ_MEMBER(path, path);
    SET_REQ_MEMBER(method, method);
    SET_REQ_MEMBER(version, version);
    SET_REQ_MEMBER(query, query);
    SET_REQ_MEMBER(remote, remote);
    SET_REQ_MEMBER(local_addr, local_addr);
    SET_REQ_MEMBER(local_port, local_port);
    SET_REQ_MEMBER(server_name, server_name);
#undef SET_REQ_MEMBER

    df = wr->fields;
    sf_end = r->fields + r->fields_count;
    for (sf = r->fields; sf < sf_end; sf++) {
        const char  *name = nxt_unit_sptr_get(&sf->name);
        const char  *value = nxt_unit_sptr_get(&sf->value);
        size_t      nlen = strlen(name);
        size_t      vlen = strlen(value);

        WASM_OFFSET_GUARD(nlen);
        df->name_off = offset;
        df->name_len = nlen;
        memcpy((uint8_t *)wr + offset, name, nlen + 1);
        offset += nlen + 1;

        WASM_OFFSET_GUARD(vlen);
        df->value_off = offset;
        df->value_len = vlen;
        memcpy((uint8_t *)wr + offset, value, vlen + 1);
        offset += vlen + 1;

        df++;
    }
#undef WASM_OFFSET_GUARD

    wr->tls = r->tls;
    wr->nfields = r->fields_count;
    wr->content_off = offset;
    wr->content_len = content_len = r->content_length;

    read_bytes = nxt_min(wr->content_len, NXT_WASM_MEM_SIZE - offset);

    bytes_read = nxt_unit_request_read(req, (uint8_t *)wr + offset, read_bytes);
    wr->content_sent = wr->total_content_sent = content_sent = bytes_read;

    wr->request_size = offset + bytes_read;

    nxt_wasm_ctx.status = NXT_WASM_HTTP_OK;
    err = nxt_wops->exec_request(&nxt_wasm_ctx);
    if (err) {
        goto out_err_500;
    }

    if (content_len == content_sent) {
        goto request_done;
    }

    offset = sizeof(nxt_wasm_request_t);
    do {
        read_bytes = nxt_min(content_len - content_sent,
                             NXT_WASM_MEM_SIZE - offset);
        bytes_read = nxt_unit_request_read(req, (uint8_t *)wr + offset,
                                           read_bytes);

        content_sent += bytes_read;
        wr->request_size = wr->content_sent = bytes_read;
        wr->total_content_sent = content_sent;
        wr->content_off = offset;

        err = nxt_wops->exec_request(&nxt_wasm_ctx);
        if (err) {
            goto out_err_500;
        }
    } while (content_sent < content_len);

    goto request_done;

out_err_500:
    nxt_unit_response_init(req, NXT_WASM_HTTP_ERROR, 0, 0);
    nxt_unit_request_done(req, NXT_UNIT_OK);

request_done:
    NXT_WASM_DO_HOOK(NXT_WASM_FH_REQUEST_END);
}


static nxt_int_t
nxt_wasm_start(nxt_task_t *task, nxt_process_data_t *data)
{
    nxt_int_t              ret;
    nxt_unit_ctx_t         *unit_ctx;
    nxt_unit_init_t        wasm_init;
    nxt_common_app_conf_t  *conf;

    conf = data->app;

    ret = nxt_unit_default_init(task, &wasm_init, conf);
    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_alert(task, "nxt_unit_default_init() failed");
        return ret;
    }

    wasm_init.callbacks.request_handler = nxt_wasm_request_handler;

    unit_ctx = nxt_unit_init(&wasm_init);
    if (nxt_slow_path(unit_ctx == NULL)) {
        return NXT_ERROR;
    }

    NXT_WASM_DO_HOOK(NXT_WASM_FH_MODULE_INIT);
    nxt_unit_run(unit_ctx);
    nxt_unit_done(unit_ctx);
    NXT_WASM_DO_HOOK(NXT_WASM_FH_MODULE_END);

    if (nxt_wasm_ctx.dirs != NULL) {
        char  **p;

        for (p = nxt_wasm_ctx.dirs; *p != NULL; p++) {
            nxt_free(*p);
        }
        nxt_free(nxt_wasm_ctx.dirs);
    }

    nxt_wops->destroy(&nxt_wasm_ctx);

    exit(EXIT_SUCCESS);
}


static nxt_int_t
nxt_wasm_setup(nxt_task_t *task, nxt_process_t *process,
               nxt_common_app_conf_t *conf)
{
    int                      n, i, err;
    nxt_conf_value_t         *dirs = NULL;
    nxt_wasm_app_conf_t      *c;
    nxt_wasm_func_handler_t  *fh;
    static const nxt_str_t   filesystem_str = nxt_string("filesystem");

    c = &conf->u.wasm;

    nxt_wops = &nxt_wasm_ops;

    nxt_wasm_ctx.module_path = c->module;

    fh = nxt_wasm_ctx.fh;

    fh[NXT_WASM_FH_REQUEST].func_name = c->request_handler;
    fh[NXT_WASM_FH_MALLOC].func_name = c->malloc_handler;
    fh[NXT_WASM_FH_FREE].func_name = c->free_handler;

    /* Optional function handlers (hooks) */
    fh[NXT_WASM_FH_MODULE_INIT].func_name = c->module_init_handler;
    fh[NXT_WASM_FH_MODULE_END].func_name = c->module_end_handler;
    fh[NXT_WASM_FH_REQUEST_INIT].func_name = c->request_init_handler;
    fh[NXT_WASM_FH_REQUEST_END].func_name = c->request_end_handler;
    fh[NXT_WASM_FH_RESPONSE_END].func_name = c->response_end_handler;

    /* Get any directories to pass through to the WASM module */
    if (c->access != NULL) {
        dirs = nxt_conf_get_object_member(c->access, &filesystem_str, NULL);
    }

    n = (dirs != NULL) ? nxt_conf_object_members_count(dirs) : 0;
    if (n == 0) {
        goto out_init;
    }

    nxt_wasm_ctx.dirs = nxt_zalloc((n + 1) * sizeof(char *));
    if (nxt_slow_path(nxt_wasm_ctx.dirs == NULL)) {
        return NXT_ERROR;
    }

    for (i = 0; i < n; i++) {
        nxt_str_t         str;
        nxt_conf_value_t  *value;

        value = nxt_conf_get_array_element(dirs, i);
        nxt_conf_get_string(value, &str);

        nxt_wasm_ctx.dirs[i] = nxt_zalloc(str.length + 1);
        memcpy(nxt_wasm_ctx.dirs[i], str.start, str.length);
    }

out_init:
    err = nxt_wops->init(&nxt_wasm_ctx);
    if (err) {
        exit(EXIT_FAILURE);
    }

    return NXT_OK;
}


NXT_EXPORT nxt_app_module_t  nxt_app_module = {
    .compat_length  = sizeof(compat),
    .compat         = compat,
    .type           = nxt_string("wasm"),
    .version        = NXT_WASM_VERSION,
    .mounts         = NULL,
    .nmounts        = 0,
    .setup          = nxt_wasm_setup,
    .start          = nxt_wasm_start,
};
