/*
 * Copyright (C) NGINX, Inc.
 *
 * mca_harness.c
 *
 * Header-only `static inline` hot IPC functions (nxt_port_queue_*,
 * nxt_app_queue_*, nxt_nncq_*, nxt_app_nncq_*, nxt_port_mmap_*) get
 * folded into their single caller and disappear as standalone symbols
 * at -O1/-O2 with both gcc and clang, so disasm-diff.sh has to track
 * the smallest surviving *caller* instead (see tools/perf/hot-functions.txt).
 * That works, but the caller's own instructions are mixed in with the
 * inlined body, and a caller that gains/loses an unrelated inline
 * elsewhere shifts the diff for no reason related to the queue code.
 *
 * This TU instantiates each of those functions once behind a
 * noinline wrapper, so every one of them gets its own stable ELF
 * symbol that objdump/llvm-objdump/llvm-mca can target directly --
 * without touching the production headers or adding noinline to the
 * real call sites (which would defeat the point of them being
 * header-only inlines in the real binary).
 *
 * Compiled by tools/perf/disasm-diff.sh with the exact same CC/CFLAGS
 * and -I src -I <build>/include as the real build, so struct layouts,
 * atomics and codegen match the production binary for the toolchain
 * under test. It is never linked into unitd or libunit; only the
 * resulting .o's disassembly is used.
 */

#include "nxt_main.h"
#include "nxt_port_queue.h"
#include "nxt_app_queue.h"
#include "nxt_app_nncq.h"
#include "nxt_port_memory_int.h"

/*
 * Prototypes so -Wmissing-prototypes (part of this project's normal
 * -Werror build) doesn't fire; nothing outside this TU calls these,
 * they only exist to give the wrapped inline a stable symbol.
 */
nxt_int_t mca_port_queue_send(nxt_port_queue_t volatile *q, const void *p,
    uint8_t size, int *notify);
ssize_t mca_port_queue_recv(nxt_port_queue_t volatile *q, void *p);
nxt_int_t mca_app_queue_send(nxt_app_queue_t volatile *q, const void *p,
    uint8_t size, uint32_t tracking, int *notify, uint32_t *cookie);
ssize_t mca_app_queue_recv(nxt_app_queue_t volatile *q, void *p,
    uint32_t *cookie);
nxt_bool_t mca_app_queue_cancel(nxt_app_queue_t volatile *q, uint32_t cookie,
    uint32_t tracking);
nxt_nncq_atomic_t mca_nncq_dequeue(nxt_nncq_t volatile *q);
void mca_nncq_enqueue(nxt_nncq_t volatile *q, nxt_nncq_atomic_t val);
nxt_app_nncq_atomic_t mca_app_nncq_dequeue(nxt_app_nncq_t volatile *q);
void mca_app_nncq_enqueue(nxt_app_nncq_t volatile *q,
    nxt_app_nncq_atomic_t val);
nxt_chunk_id_t mca_port_mmap_get_free_chunk(nxt_free_map_t *m,
    nxt_chunk_id_t *c);


__attribute__((noinline)) nxt_int_t
mca_port_queue_send(nxt_port_queue_t volatile *q, const void *p,
    uint8_t size, int *notify)
{
    return nxt_port_queue_send(q, p, size, notify);
}


__attribute__((noinline)) ssize_t
mca_port_queue_recv(nxt_port_queue_t volatile *q, void *p)
{
    return nxt_port_queue_recv(q, p);
}


__attribute__((noinline)) nxt_int_t
mca_app_queue_send(nxt_app_queue_t volatile *q, const void *p,
    uint8_t size, uint32_t tracking, int *notify, uint32_t *cookie)
{
    return nxt_app_queue_send(q, p, size, tracking, notify, cookie);
}


__attribute__((noinline)) ssize_t
mca_app_queue_recv(nxt_app_queue_t volatile *q, void *p, uint32_t *cookie)
{
    return nxt_app_queue_recv(q, p, cookie);
}


__attribute__((noinline)) nxt_bool_t
mca_app_queue_cancel(nxt_app_queue_t volatile *q, uint32_t cookie,
    uint32_t tracking)
{
    return nxt_app_queue_cancel(q, cookie, tracking);
}


__attribute__((noinline)) nxt_nncq_atomic_t
mca_nncq_dequeue(nxt_nncq_t volatile *q)
{
    return nxt_nncq_dequeue(q);
}


__attribute__((noinline)) void
mca_nncq_enqueue(nxt_nncq_t volatile *q, nxt_nncq_atomic_t val)
{
    nxt_nncq_enqueue(q, val);
}


__attribute__((noinline)) nxt_app_nncq_atomic_t
mca_app_nncq_dequeue(nxt_app_nncq_t volatile *q)
{
    return nxt_app_nncq_dequeue(q);
}


__attribute__((noinline)) void
mca_app_nncq_enqueue(nxt_app_nncq_t volatile *q, nxt_app_nncq_atomic_t val)
{
    nxt_app_nncq_enqueue(q, val);
}


__attribute__((noinline)) nxt_chunk_id_t
mca_port_mmap_get_free_chunk(nxt_free_map_t *m, nxt_chunk_id_t *c)
{
    return nxt_port_mmap_get_free_chunk(m, c);
}
