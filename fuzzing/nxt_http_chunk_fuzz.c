/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>


/*
 * Fuzzes the chunked transfer-coding decoder, nxt_http_chunk_parse().  It
 * decodes a client request body and, on the proxy side, an upstream response
 * body, so both ends of a connection feed it.
 *
 * The decoder keeps its state across buffers and across calls, and a network
 * read can end on any byte.  So every input is decoded twice: once as a single
 * buffer, and once cut into pieces that are handed over as chains of one to
 * three buffers per call.  The two runs must agree on the verdict, on where
 * the message ended, and on every decoded byte.  A mismatch aborts, so a
 * state that is lost or misread at a buffer border is a crash, not a silent
 * difference.
 *
 * Input layout: the first byte seeds the cut points, the rest is the body.
 */


#define KMININPUTLENGTH 2
#define KMAXINPUTLENGTH 4096

#define NXT_CHUNK_FUZZ_MAX_PIECES  64


typedef struct {
    nxt_uint_t  verdict;
    size_t      consumed;
    size_t      length;
    size_t      capacity;
    u_char      *data;
} nxt_chunk_fuzz_result_t;


extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static nxt_int_t nxt_chunk_fuzz_run(nxt_task_t *task, nxt_mp_t *mp,
    const u_char *body, size_t size, uint32_t seed,
    nxt_chunk_fuzz_result_t *res);
static nxt_int_t nxt_chunk_fuzz_collect(nxt_buf_t *out,
    nxt_chunk_fuzz_result_t *res);
static nxt_uint_t nxt_chunk_fuzz_verdict(nxt_http_chunk_parse_t *hcp);
static uint32_t nxt_chunk_fuzz_rand(uint32_t *state);


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
    nxt_mp_t                 *mp;
    nxt_task_t               *task;
    nxt_thread_t             *thr;
    nxt_chunk_fuzz_result_t  whole, split;

    if (size < KMININPUTLENGTH || size > KMAXINPUTLENGTH) {
        return 0;
    }

    thr = nxt_thread();
    task = thr->task;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return 0;
    }

    /* A seed of 0 puts the whole body in one buffer: the reference run. */

    if (nxt_chunk_fuzz_run(task, mp, data + 1, size - 1, 0, &whole) != NXT_OK
        || nxt_chunk_fuzz_run(task, mp, data + 1, size - 1,
                              (uint32_t) data[0] + 1, &split)
           != NXT_OK)
    {
        goto done;
    }

    if (whole.verdict != split.verdict
        || whole.consumed != split.consumed
        || whole.length != split.length
        || memcmp(whole.data, split.data, whole.length) != 0)
    {
        nxt_thread_log_alert("chunk fuzz: split run differs: "
                             "verdict %ui/%ui consumed %uz/%uz "
                             "length %uz/%uz",
                             whole.verdict, split.verdict,
                             whole.consumed, split.consumed,
                             whole.length, split.length);
        abort();
    }

done:

    nxt_mp_destroy(mp);

    return 0;
}


/*
 * Every piece is a separate heap allocation of its exact size, so that a read
 * past the end of a buffer lands in an ASan redzone rather than in the next
 * piece.  The buffer headers and the decoder's output buffers come from "mp";
 * the pool is destroyed after both runs, which also frees the output buffers
 * that the decoder retained the pool for.
 */

static nxt_int_t
nxt_chunk_fuzz_run(nxt_task_t *task, nxt_mp_t *mp, const u_char *body,
    size_t size, uint32_t seed, nxt_chunk_fuzz_result_t *res)
{
    size_t                  offset, len, cut;
    size_t                  offsets[NXT_CHUNK_FUZZ_MAX_PIECES];
    nxt_int_t               ret;
    nxt_buf_t               *b, *out, *pieces[NXT_CHUNK_FUZZ_MAX_PIECES];
    nxt_uint_t              i, j, n, group;
    nxt_http_chunk_parse_t  hcp;

    nxt_memzero(res, sizeof(nxt_chunk_fuzz_result_t));

    res->data = nxt_mp_nget(mp, size);
    if (res->data == NULL) {
        return NXT_ERROR;
    }

    res->capacity = size;

    ret = NXT_ERROR;

    /* Cut the body. */

    n = 0;
    offset = 0;

    while (offset < size) {
        len = size - offset;

        if (seed != 0 && n < NXT_CHUNK_FUZZ_MAX_PIECES - 1) {
            cut = nxt_chunk_fuzz_rand(&seed) % 16 + 1;
            len = nxt_min(len, cut);
        }

        b = nxt_buf_mem_alloc(mp, 0, 0);
        if (b == NULL) {
            goto done;
        }

        b->mem.start = nxt_malloc(len);
        if (b->mem.start == NULL) {
            goto done;
        }

        nxt_memcpy(b->mem.start, body + offset, len);

        b->mem.pos = b->mem.start;
        b->mem.free = b->mem.start + len;
        b->mem.end = b->mem.free;

        offsets[n] = offset;
        pieces[n++] = b;

        offset += len;
    }

    /* Decode. */

    nxt_memzero(&hcp, sizeof(nxt_http_chunk_parse_t));

    hcp.mem_pool = mp;

    /*
     * The decoder would otherwise queue a buffer that yielded no data on the
     * engine's work queue, and there is no engine here.
     */
    hcp.retain_buffers = 1;

    for (i = 0; i < n; i += group) {
        group = 1;

        if (seed != 0) {
            cut = nxt_chunk_fuzz_rand(&seed) % 3 + 1;
            group = nxt_min(n - i, cut);
        }

        for (j = i; j < i + group - 1; j++) {
            pieces[j]->next = pieces[j + 1];
        }

        pieces[i + group - 1]->next = NULL;

        out = nxt_http_chunk_parse(task, &hcp, pieces[i]);

        if (nxt_chunk_fuzz_collect(out, res) != NXT_OK) {
            nxt_thread_log_alert("chunk fuzz: bad output buffer");
            abort();
        }

        if (hcp.error) {
            /* Out of memory, not a verdict on the input. */
            goto done;
        }

        if (hcp.chunk_error || hcp.last) {
            break;
        }
    }

    res->verdict = nxt_chunk_fuzz_verdict(&hcp);

    /*
     * The decoder stops inside the buffer that holds the byte that ended the
     * message or broke it, and has taken that byte.  Buffers after it are
     * untouched, and buffers before it may keep "pos" at "start" when they
     * held only chunk data, so the last piece that moved is the one.
     */
    if (hcp.chunk_error || hcp.last) {
        for (j = n; j > 0; j--) {
            b = pieces[j - 1];

            if (b->mem.pos != b->mem.start) {
                res->consumed = offsets[j - 1] + (b->mem.pos - b->mem.start);
                break;
            }
        }
    }

    ret = NXT_OK;

done:

    for (i = 0; i < n; i++) {
        nxt_free(pieces[i]->mem.start);
    }

    return ret;
}


/*
 * A decoded slice must lie inside the buffer it was cut from, which is its
 * parent, and the slices together cannot hold more bytes than the body.
 */

static nxt_int_t
nxt_chunk_fuzz_collect(nxt_buf_t *out, nxt_chunk_fuzz_result_t *res)
{
    size_t     len;
    nxt_buf_t  *b, *parent;

    for (b = out; b != NULL; b = b->next) {
        parent = b->parent;

        if (parent == NULL
            || b->mem.pos > b->mem.free
            || b->mem.pos < parent->mem.start
            || b->mem.free > parent->mem.end)
        {
            return NXT_ERROR;
        }

        len = b->mem.free - b->mem.pos;

        if (len > res->capacity - res->length) {
            return NXT_ERROR;
        }

        nxt_memcpy(res->data + res->length, b->mem.pos, len);
        res->length += len;
    }

    return NXT_OK;
}


static nxt_uint_t
nxt_chunk_fuzz_verdict(nxt_http_chunk_parse_t *hcp)
{
    if (hcp->chunk_error) {
        return 1;
    }

    if (hcp->last) {
        return 2;
    }

    return 0;
}


/* xorshift32: cheap and deterministic for a given input. */

static uint32_t
nxt_chunk_fuzz_rand(uint32_t *state)
{
    uint32_t  x;

    x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    *state = x;

    return x;
}
