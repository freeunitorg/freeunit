/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>

/* DO NOT TRY THIS AT HOME! */
#include "nxt_h1proto_websocket.c"


/*
 * Fuzzes the UTF-8 check the router runs on WebSocket text messages and
 * close reasons, nxt_h1p_ws_utf8_validate().  The client controls every byte
 * of it: the text, the mask, and how the message is split into frames.  The
 * check works on masked bytes, walks a frame that spans several buffers, and
 * carries a character cut by a frame border over to the next frame.
 *
 * The verdict is compared with a small independent validator written from
 * the table of well-formed byte sequences in the Unicode standard (Table 3-7).
 * RFC 6455 Section 8.1 wants a connection failed as soon as the text cannot
 * be valid any more, so the verdict must match after every frame, not only at
 * the end of the message.  A mismatch aborts.
 *
 * Input layout:
 *   byte 0      flags; bit 0 checks a close frame reason instead of a message
 *   byte 1      seeds the frame and buffer cuts
 *   bytes 2-5   the masking key
 *   the rest    the unmasked payload
 */


#define KMININPUTLENGTH 6
#define KMAXINPUTLENGTH 4096

#define NXT_WS_FUZZ_MAX_BUFS  8


typedef struct {
    uint8_t  need;
    u_char   lo;
    u_char   hi;
    uint8_t  failed;
} nxt_ws_fuzz_ref_t;


extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static nxt_int_t nxt_ws_fuzz_frame(nxt_mp_t *mp, nxt_h1p_ws_utf8_t *state,
    const u_char *payload, size_t size, size_t skip, const u_char *mask,
    nxt_uint_t final, uint32_t *seed, nxt_int_t *res);
static void nxt_ws_fuzz_ref(nxt_ws_fuzz_ref_t *ref, const u_char *p,
    size_t size);
static uint32_t nxt_ws_fuzz_rand(uint32_t *state);


extern char  **environ;


int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
    if (nxt_lib_start("fuzzing", NULL, &environ) != NXT_OK) {
        return NXT_ERROR;
    }

    return 0;
}


int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t              len, cut, skip;
    uint32_t            seed;
    nxt_mp_t            *mp;
    nxt_int_t           res, expect;
    nxt_uint_t          close, final;
    const u_char        *mask, *p, *end;
    nxt_ws_fuzz_ref_t   ref;
    nxt_h1p_ws_utf8_t   state;

    if (size < KMININPUTLENGTH || size > KMAXINPUTLENGTH) {
        return 0;
    }

    close = data[0] & 1;
    seed = (uint32_t) data[1] + 1;
    mask = &data[2];

    p = data + 6;
    end = data + size;

    /* A close frame has a two-byte code before the reason. */
    if (close && end - p < 2) {
        return 0;
    }

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return 0;
    }

    nxt_memzero(&state, sizeof(nxt_h1p_ws_utf8_t));
    nxt_memzero(&ref, sizeof(nxt_ws_fuzz_ref_t));

    do {
        len = end - p;
        skip = 0;

        if (close) {
            /* A close frame is a single, whole frame. */
            skip = 2;

        } else {
            /* Empty frames are legal, so a cut may be 0. */
            cut = nxt_ws_fuzz_rand(&seed) % 64;
            len = nxt_min(len, cut);
        }

        final = (p + len == end);

        if (nxt_ws_fuzz_frame(mp, &state, p, len, skip, mask, final, &seed,
                              &res)
            != NXT_OK)
        {
            goto done;
        }

        nxt_ws_fuzz_ref(&ref, p + skip, len - skip);

        expect = (ref.failed || (final && ref.need != 0)) ? NXT_ERROR : NXT_OK;

        if (res != expect) {
            nxt_thread_log_alert("ws utf8 fuzz: frame at %uz of %uz bytes: "
                                 "%i, expected %i",
                                 (size_t) (p - data - 6), len, res, expect);
            abort();
        }

        p += len;

    } while (res == NXT_OK && p < end);

done:

    nxt_mp_destroy(mp);

    return 0;
}


/*
 * Masks one frame's payload into a chain of buffers, each a heap allocation
 * of its exact size so that ASan catches a read past any of them, and runs
 * the check on it.  The first buffer starts with a few bytes that stand for
 * the frame header: the check must begin at "start", not at the buffer.
 */

static nxt_int_t
nxt_ws_fuzz_frame(nxt_mp_t *mp, nxt_h1p_ws_utf8_t *state, const u_char *payload,
    size_t size, size_t skip, const u_char *mask, nxt_uint_t final,
    uint32_t *seed, nxt_int_t *res)
{
    size_t      hsize, len, cut, i, offset;
    u_char      *start;
    nxt_int_t   ret;
    nxt_buf_t   *b, *first, **prev;
    nxt_uint_t  n, k;
    u_char      *mem[NXT_WS_FUZZ_MAX_BUFS];

    ret = NXT_ERROR;

    first = NULL;
    prev = &first;
    start = NULL;

    n = 0;
    offset = 0;
    hsize = nxt_ws_fuzz_rand(seed) % 13 + 2;

    do {
        len = size - offset;

        if (n < NXT_WS_FUZZ_MAX_BUFS - 1) {
            cut = nxt_ws_fuzz_rand(seed) % 24 + 1;
            len = nxt_min(len, cut);
        }

        if (n == 0) {
            len += hsize;
        }

        b = nxt_buf_mem_alloc(mp, 0, 0);
        if (b == NULL) {
            goto done;
        }

        mem[n] = nxt_malloc(len);
        if (mem[n] == NULL) {
            goto done;
        }

        b->mem.start = mem[n++];
        b->mem.pos = b->mem.start;
        b->mem.free = b->mem.start + len;
        b->mem.end = b->mem.free;

        i = 0;

        if (n == 1) {
            nxt_memset(b->mem.start, 0x81, hsize);
            start = b->mem.start + hsize;
            i = hsize;
        }

        for ( /* void */ ; i < len; i++) {
            b->mem.start[i] = payload[offset] ^ mask[offset % 4];
            offset++;
        }

        *prev = b;
        prev = &b->next;

    } while (offset < size);

    *res = nxt_h1p_ws_utf8_validate(state, first, start, skip, size - skip,
                                    mask, final);

    ret = NXT_OK;

done:

    for (k = 0; k < n; k++) {
        nxt_free(mem[k]);
    }

    return ret;
}


/* Unicode Table 3-7, Well-Formed UTF-8 Byte Sequences. */

static void
nxt_ws_fuzz_ref(nxt_ws_fuzz_ref_t *ref, const u_char *p, size_t size)
{
    u_char        c;
    const u_char  *end;

    for (end = p + size; p < end && !ref->failed; p++) {
        c = *p;

        if (ref->need != 0) {
            if (c < ref->lo || c > ref->hi) {
                ref->failed = 1;
                return;
            }

            ref->need--;
            ref->lo = 0x80;
            ref->hi = 0xBF;
            continue;
        }

        ref->lo = 0x80;
        ref->hi = 0xBF;

        if (c < 0x80) {
            continue;
        }

        if (c >= 0xC2 && c <= 0xDF) {
            ref->need = 1;

        } else if (c == 0xE0) {
            ref->need = 2;
            ref->lo = 0xA0;

        } else if (c == 0xED) {
            ref->need = 2;
            ref->hi = 0x9F;

        } else if (c >= 0xE1 && c <= 0xEF) {
            ref->need = 2;

        } else if (c == 0xF0) {
            ref->need = 3;
            ref->lo = 0x90;

        } else if (c >= 0xF1 && c <= 0xF3) {
            ref->need = 3;

        } else if (c == 0xF4) {
            ref->need = 3;
            ref->hi = 0x8F;

        } else {
            ref->failed = 1;
            return;
        }
    }
}


/* xorshift32: cheap and deterministic for a given input. */

static uint32_t
nxt_ws_fuzz_rand(uint32_t *state)
{
    uint32_t  x;

    x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    *state = x;

    return x;
}
