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


/*
 * Without a size level 3 uses a 2 MiB window (windowLog 21), some 3.5 MiB
 * of state per stream.  A stream of unknown length gets windowLog 17,
 * 128 KiB.  For a known length the window is shrunk to the body, as for
 * zlib.  The length is not pledged: an application may send fewer bytes
 * than it declares, and a pledged size then fails the frame.
 */

#define NXT_ZSTD_STREAM_WINDOWLOG  17
#define NXT_ZSTD_MIN_WINDOWLOG     10
#define NXT_ZSTD_MAX_WINDOWLOG     21


static int
nxt_zstd_windowlog(const nxt_http_comp_compressor_ctx_t *ctx)
{
    int  wlog;

    if (ctx->size_hint <= 0) {
        return NXT_ZSTD_STREAM_WINDOWLOG;
    }

    wlog = NXT_ZSTD_MIN_WINDOWLOG;

    while (wlog < NXT_ZSTD_MAX_WINDOWLOG
           && ctx->size_hint > ((nxt_off_t) 1 << wlog))
    {
        wlog++;
    }

    return wlog;
}


static int
nxt_zstd_init(nxt_http_comp_compressor_ctx_t *ctx)
{
    ZSTD_CStream  **zstd = &ctx->zstd_ctx;

    *zstd = ZSTD_createCStream();
    if (*zstd == NULL) {
        return -1;
    }

    if (ZSTD_isError(ZSTD_CCtx_setParameter(*zstd, ZSTD_c_compressionLevel,
                                            ctx->level))
        || ZSTD_isError(ZSTD_CCtx_setParameter(*zstd, ZSTD_c_windowLog,
                                               nxt_zstd_windowlog(ctx))))
    {
        ZSTD_freeCStream(*zstd);
        *zstd = NULL;
        return -1;
    }

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
