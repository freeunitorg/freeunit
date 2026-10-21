/*
 * Copyright (C) Andrew Clayton
 * Copyright (C) F5, Inc.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <zstd.h>

#if (ZSTD_VERSION_NUMBER < 10400)
#error "zstd compression needs libzstd 1.4.0 or later (ZSTD_compressStream2)"
#endif

#include "nxt_http_compression.h"


static int
nxt_zstd_init(nxt_http_comp_compressor_ctx_t *ctx)
{
    ZSTD_CStream  **zstd = &ctx->zstd_ctx;

    *zstd = ZSTD_createCStream();
    if (*zstd == NULL) {
        return -1;
    }
    ZSTD_initCStream(*zstd, ctx->level);

    return 0;
}


static size_t
nxt_zstd_bound(const nxt_http_comp_compressor_ctx_t *ctx, size_t in_len)
{
    return ZSTD_compressBound(in_len);
}


/*
 * Each call flushes, as zlib (Z_SYNC_FLUSH) and brotli (BROTLI_OPERATION_FLUSH)
 * do, so a part of a streamed response leaves as soon as it arrives.  The
 * last call ends the frame.  The caller sizes the output buffer for that;
 * input or flushed output that does not fit is an error, not a partial
 * write.
 */

static ssize_t
nxt_zstd_compress(nxt_http_comp_compressor_ctx_t *ctx, const uint8_t *in_buf,
                  size_t in_len, uint8_t *out_buf, size_t out_len, bool last)
{
    size_t          ret;
    ZSTD_CStream    *zstd = ctx->zstd_ctx;
    ZSTD_inBuffer   zinb = { .src = in_buf, .size = in_len };
    ZSTD_outBuffer  zoutb = { .dst = out_buf, .size = out_len };

    ret = ZSTD_compressStream2(zstd, &zoutb, &zinb,
                               last ? ZSTD_e_end : ZSTD_e_flush);

    if (ZSTD_isError(ret) || ret != 0 || zinb.pos < zinb.size) {
        return -1;
    }

    return zoutb.pos;
}


static void
nxt_zstd_free(nxt_http_comp_compressor_ctx_t *ctx)
{
    ZSTD_freeCStream(ctx->zstd_ctx);
    ctx->zstd_ctx = NULL;
}


const nxt_http_comp_operations_t  nxt_http_comp_zstd_ops = {
    .init               = nxt_zstd_init,
    .bound              = nxt_zstd_bound,
    .deflate            = nxt_zstd_compress,
    .free               = nxt_zstd_free,
};
