/*
 * Copyright (C) Andrew Clayton
 * Copyright (C) F5, Inc.
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <zlib.h>

#include "nxt_http_compression.h"


/*
 * The window and the hash size.  The window was fixed at 9 bits (512
 * bytes), inherited from NGINX Unit: deflate then finds no match farther
 * back than 512 bytes, and a 100 KB text page came out 28 % larger than
 * with the 32 KiB window, at 4 times the CPU (zlib level 1, 10 parts).  The
 * memory it saved was small: the hash, 2^(memLevel + 9) bytes, dominates.
 *
 * So the window is 15 bits and memLevel 8, the zlib defaults, as nginx
 * uses.  For a body of known length both shrink as in nginx's gzip filter:
 * a window larger than the body (less the 262 bytes zlib keeps back) buys
 * nothing.  An unknown length keeps the full size.
 */

static void
nxt_zlib_params(const nxt_http_comp_compressor_ctx_t *ctx, int *wbits,
                int *memlevel)
{
    *wbits = 15;
    *memlevel = 8;

    if (ctx->size_hint > 0) {
        while (*wbits > 9 && ctx->size_hint < (1 << (*wbits - 1)) - 262) {
            (*wbits)--;
            (*memlevel)--;
        }

        if (*memlevel < 1) {
            *memlevel = 1;
        }
    }
}


static int
nxt_zlib_gzip_init(nxt_http_comp_compressor_ctx_t *ctx)
{
    int       wbits, memlevel;
    z_stream  *z = &ctx->zlib_ctx;

    *z = (z_stream){};

    nxt_zlib_params(ctx, &wbits, &memlevel);

    return deflateInit2(z, ctx->level, Z_DEFLATED, wbits + 16, memlevel,
                        Z_DEFAULT_STRATEGY);
}


static int
nxt_zlib_deflate_init(nxt_http_comp_compressor_ctx_t *ctx)
{
    int       wbits, memlevel;
    z_stream  *z = &ctx->zlib_ctx;

    *z = (z_stream){};

    nxt_zlib_params(ctx, &wbits, &memlevel);

    return deflateInit2(z, ctx->level, Z_DEFLATED, wbits, memlevel,
                        Z_DEFAULT_STRATEGY);
}


static size_t
nxt_zlib_bound(const nxt_http_comp_compressor_ctx_t *ctx, size_t in_len)
{
    z_stream  *z = (z_stream *)&ctx->zlib_ctx;

    return deflateBound(z, in_len);
}


static ssize_t
nxt_zlib_deflate(nxt_http_comp_compressor_ctx_t *ctx, const uint8_t *in_buf,
                 size_t in_len, uint8_t *out_buf, size_t out_len, bool last)
{
    int       ret;
    uInt      avail_out;
    z_stream  *z = &ctx->zlib_ctx;

    if (in_len > UINT_MAX) {
        return -1;
    }

    /*
     * avail_out is an unsigned int.  The static path passes the rest of a
     * mapping sized for the whole file, and for a file of 3.5 GiB or more
     * that is above UINT_MAX on a 64-bit platform: the plain conversion kept
     * only the low 32 bits, so a 128 KiB piece could get a few bytes of
     * room, and the input it did not consume was dropped in silence.
     */
    avail_out = (out_len > UINT_MAX) ? UINT_MAX : (uInt) out_len;

    z->avail_in = (uInt) in_len;
    z->next_in = (z_const Bytef *)in_buf;

    z->avail_out = avail_out;
    z->next_out = out_buf;

    ret = deflate(z, last ? Z_FINISH : Z_SYNC_FLUSH);
    if (ret == Z_STREAM_ERROR || ret == Z_BUF_ERROR) {
        return -1;
    }

    /*
     * Z_FINISH that ran out of output space returns Z_OK and leaves the
     * stream unfinished.  The caller frees the stream after the last call,
     * so a short buffer here would send a truncated body as a success.
     */
    if (last && ret != Z_STREAM_END) {
        return -1;
    }

    return avail_out - z->avail_out;
}


static void
nxt_zlib_free(nxt_http_comp_compressor_ctx_t *ctx)
{
    deflateEnd(&ctx->zlib_ctx);
}


const nxt_http_comp_operations_t  nxt_http_comp_deflate_ops = {
    .init               = nxt_zlib_deflate_init,
    .bound              = nxt_zlib_bound,
    .deflate            = nxt_zlib_deflate,
    .free               = nxt_zlib_free,
};


const nxt_http_comp_operations_t  nxt_http_comp_gzip_ops = {
    .init               = nxt_zlib_gzip_init,
    .bound              = nxt_zlib_bound,
    .deflate            = nxt_zlib_deflate,
    .free               = nxt_zlib_free,
};
