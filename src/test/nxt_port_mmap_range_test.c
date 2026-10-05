
/*
 * Copyright (C) NGINX, Inc.
 */

/*
 * nxt_port_mmap_chunk_range_valid() guards every consumer of a peer-authored
 * (chunk_id, size) pair: both fields are uint32_t values written by the other
 * side of a shared memory segment, and both feed pointer arithmetic over a
 * 10 MB mapping.
 *
 * The reason this test exists is the chunk count.  The idiomatic round-up
 *
 *     nchunks = (size + PORT_MMAP_CHUNK_SIZE - 1) / PORT_MMAP_CHUNK_SIZE;
 *
 * overflows when the addition leaves uint32_t range, i.e. for every size in
 * [0xFFFFC001, 0xFFFFFFFF] -- exactly 16383 values.  On each of them it yields
 * nchunks == 0, which passes the "fits in the segment" test and therefore
 * *accepts* a message claiming a ~4 GB payload.  The two-step form used by the
 * function -- divide first, adjust for the remainder afterwards, in size_t --
 * yields the correct 262144 for those sizes and rejects them.
 *
 * That one-liner compiles clean under -Wall -Wextra -Werror, reads better than
 * what it would replace, and passes every other test in this tree while
 * reopening a 16383-value acceptance hole.  The wrap-band rows below exist so
 * that "simplification" fails loudly instead.
 *
 * The table is written for the production shm geometry; NXT_MMAP_TINY_CHUNK
 * changes PORT_MMAP_DATA_SIZE by four orders of magnitude, so the expected
 * counts would all have to be recomputed for such a build.
 */

#include <nxt_main.h>
#include <nxt_port_memory_int.h>
#include "nxt_tests.h"


#if (PORT_MMAP_CHUNK_SIZE != 16384 || PORT_MMAP_CHUNK_COUNT != 640            \
     || PORT_MMAP_DATA_SIZE != 10485760 || PORT_MMAP_SIZE != 10489856)
#error src/test/nxt_port_mmap_range_test.c: the expected chunk counts are \
       derived from the production shm geometry; recompute the table.
#endif


nxt_int_t
nxt_port_mmap_range_test(nxt_thread_t *thr)
{
    size_t      nchunks;
    nxt_uint_t  i;
    nxt_bool_t  valid;

    static const struct {
        nxt_chunk_id_t  chunk_id;
        uint32_t        size;
        nxt_bool_t      valid;
        size_t          nchunks;

    } tests[] = {
        /* Boundaries.  A zero-length range is legal and spans no chunks. */
        {     0,          0,  1,      0 },
        {     0,          1,  1,      1 },
        {     0,      16384,  1,      1 },
        {     0,      16385,  1,      2 },

        /* Two chunks starting two chunks from the end, and one too many. */
        {   638,      32768,  1,      2 },
        {   638,      32769,  0,      3 },

        /* The last legal chunk, and the off-by-one on the far side. */
        {   639,          0,  1,      0 },
        {   639,      16384,  1,      1 },
        {   639,      16385,  0,      2 },

        /* The first illegal chunk_id, with and without a payload. */
        {   640,          0,  0,      0 },
        {   640,          1,  0,      1 },

        /* The whole data area, and one byte past it. */
        {     0,   10485760,  1,    640 },
        {     0,   10485761,  0,    641 },

        /*
         * The wrap band.  0xFFFFC000 is the last size for which the naive
         * round-up still fits in uint32_t, so both forms agree there; from
         * 0xFFFFC001 up the naive form wraps to nchunks == 0 and accepts.
         */
        {     0, 0xFFFFC000,  0, 262143 },
        {     0, 0xFFFFC001,  0, 262144 },
        {     0, 0xFFFFC002,  0, 262144 },
        {     0, 0xFFFFE000,  0, 262144 },
        {     0, 0xFFFFFFFE,  0, 262144 },
        {     0, 0xFFFFFFFF,  0, 262144 },

        /*
         * A huge count against a legal chunk_id near the end: rejected, and
         * the subtraction on the constant side never underflows getting
         * there.  This row does not distinguish the current form from a
         * rewrite to "chunk_id + nchunks > PORT_MMAP_CHUNK_COUNT" -- 639 +
         * 262144 does not wrap, so that form rejects here too.  The row that
         * fails such a rewrite is (640, 0) above, where 640 + 0 is not
         * greater than 640.
         */
        {   639, 0xFFFFFFFF,  0, 262144 },

        /*
         * The maximum peer-authored chunk_id.  Both this row and (640, 0)
         * above fail if the chunk_id test is deleted, but through different
         * arithmetic, so neither is redundant as documentation of why the
         * test has to stay:
         *
         *   (640, 0)         limit is 640 - 640 == 0 and nchunks is 0, so
         *                    "0 > 0" is false and the range is accepted;
         *   (0xFFFFFFFF, 1)  limit underflows to a huge value modulo
         *                    SIZE_MAX -- 18446744069414584961 where size_t
         *                    is 64 bits, 641 where it is 32 -- and nchunks
         *                    == 1 does not exceed either, so it is accepted.
         *
         * The second is the one worth having in view, because it shows the
         * "subtract on the constant side" form is underflow-safe only while
         * the chunk_id test runs first -- it is not a property of the
         * subtraction on its own.
         *
         * This row also fails a rewrite to "(uint32_t) (chunk_id + nchunks) >
         * PORT_MMAP_CHUNK_COUNT", where the sum wraps to 0 and is accepted.
         * nchunks must be >= 1 to reach 0, so (0xFFFFFFFF, 0) would not do.
         */
        { 0xFFFFFFFF,       1,  0,      1 },
    };

    nxt_thread_time_update(thr);

    for (i = 0; i < nxt_nitems(tests); i++) {
        nchunks = (size_t) -1;

        valid = nxt_port_mmap_chunk_range_valid(tests[i].chunk_id,
                                                tests[i].size, &nchunks);

        if ((valid != 0) != (tests[i].valid != 0)) {
            nxt_log_alert(thr->log,
                          "nxt_port_mmap_chunk_range_valid() test failed: "
                          "chunk_id %uD, size %uD: got %d, expected %d",
                          tests[i].chunk_id, tests[i].size,
                          (int) (valid != 0), (int) (tests[i].valid != 0));
            return NXT_ERROR;
        }

        if (nchunks != tests[i].nchunks) {
            nxt_log_alert(thr->log,
                          "nxt_port_mmap_chunk_range_valid() test failed: "
                          "chunk_id %uD, size %uD: got %uz chunks, "
                          "expected %uz",
                          tests[i].chunk_id, tests[i].size, nchunks,
                          tests[i].nchunks);
            return NXT_ERROR;
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log,
                  "nxt_port_mmap_chunk_range_valid() test passed");

    return NXT_OK;
}
