
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_MP_H_INCLUDED_
#define _NXT_MP_H_INCLUDED_


/*
 * Memory pool keeps track of all allocations so they can be freed at once
 * on pool destruction.  A memory pool is not thread safe, so only one thread
 * must work with the pool.  If an allocation should be passed to another
 * thread, it should be allocated with nxt_mp_retain() and then should be
 * freed with nxt_mp_release().  These functions updates pool retention
 * counter.  Memory pools decrease number of malloc() and free() calls and
 * thus reduces thread contention on locks in malloc library.  Memory pools
 * allow to make both freeable and non-freeable allocations.  The freeable
 * memory is allocated in fixed size chunks to decrease memory fragmentaiton
 * on reallocations.  The non-freeable memory is intended to allocate
 * structures and other items which should be available until memory pool
 * destruction.  Due to allocation strategy described in nxt_mp.c memory pools
 * may also improve data cache locality.
 */

typedef struct nxt_mp_s  nxt_mp_t;


/*
 * Each thread keeps freed pool clusters for reuse (see nxt_mp.c).  The cache
 * needs __thread storage.  It is off with NXT_DEBUG_MEMORY, where a pool
 * allocates every block apart, and with NXT_HEAP_SANITIZER (nxt_clang.h): a
 * reused cluster would hide a use after free of a destroyed pool.  A build
 * also turns it off with --cc-opt=-DNXT_MP_NO_CLUSTER_CACHE=1, for example
 * for Valgrind, which sees a cached cluster as allocated memory.  The switch
 * cannot turn the cache on.  NXT_DEBUG_MEMORY is set the same way:
 * --cc-opt=-DNXT_DEBUG_MEMORY=1.
 */

#ifndef NXT_MP_NO_CLUSTER_CACHE
#define NXT_MP_NO_CLUSTER_CACHE  0
#endif

#if (NXT_HAVE_THREAD_STORAGE_CLASS && !(NXT_DEBUG_MEMORY)                     \
     && !(NXT_HEAP_SANITIZER) && !(NXT_MP_NO_CLUSTER_CACHE))
#define NXT_MP_CLUSTER_CACHE  1
#else
#define NXT_MP_CLUSTER_CACHE  0
#endif


/*
 * nxt_mp_create() creates a memory pool and sets the pool's retention
 * counter to 1.
 */
NXT_EXPORT nxt_mp_t *nxt_mp_create(size_t cluster_size, size_t page_alignment,
    size_t page_size, size_t min_chunk_size)
    NXT_MALLOC_LIKE;

/*
 * nxt_mp_destroy() destroys memory pool in spite of the pool's retention
 * counter.
 */
NXT_EXPORT void nxt_mp_destroy(nxt_mp_t *mp);

/*
 * nxt_mp_retain() increases memory pool retention counter.
 */
NXT_EXPORT void nxt_mp_retain(nxt_mp_t *mp);

/*
 * nxt_mp_release() decreases memory pool retention counter.
 * If the counter becomes zero the pool is destroyed.
 */
NXT_EXPORT void nxt_mp_release(nxt_mp_t *mp);

/*
 * nxt_mp_cluster_cache_drain() frees the cluster memory that the calling
 * thread keeps for reuse.  nxt_thread_exit() calls it, and so does the
 * cleanup handler of a canceled thread.
 */
NXT_EXPORT void nxt_mp_cluster_cache_drain(void);

#if (NXT_TESTS)
/*
 * Returns the current retention count.  Used by
 * src/test/nxt_port_fail_test.c to verify the "retain after
 * successful send" pattern leaves the pool's retain at its
 * baseline (1) when the send fails.
 */
NXT_EXPORT uint32_t nxt_mp_test_retain_count(nxt_mp_t *mp);

/*
 * Returns the number of nxt_memalign() calls for cluster memory, in all
 * threads, since the process started.  A call that fails counts too.
 */
NXT_EXPORT nxt_uint_t nxt_mp_test_cluster_alloc_count(void);

/*
 * Returns the number of clusters that nxt_mp_cluster_cache_drain() freed,
 * in all threads, since the process started.
 */
NXT_EXPORT nxt_uint_t nxt_mp_test_cluster_drain_count(void);

#if (NXT_MP_CLUSTER_CACHE)
/*
 * Gives the cluster memory "p" of the pool "mp" to the cache of the calling
 * thread twice, as a path that frees one cluster twice would.
 */
NXT_EXPORT void nxt_mp_test_cluster_put_twice(nxt_mp_t *mp, void *p);

/* Returns the number of clusters in the cache of the calling thread. */
NXT_EXPORT nxt_uint_t nxt_mp_test_cluster_cache_count(void);
#endif
#endif

/* nxt_mp_test_sizes() tests validity of memory pool parameters. */
NXT_EXPORT nxt_bool_t nxt_mp_test_sizes(size_t cluster_size,
    size_t page_alignment, size_t page_size, size_t min_chunk_size);

/* nxt_mp_is_empty() tests that pool is empty. */
NXT_EXPORT nxt_bool_t nxt_mp_is_empty(nxt_mp_t *mp);


/*
 * nxt_mp_alloc() returns aligned freeable memory.
 * The alignment is sutiable to allocate structures.
 */
NXT_EXPORT void *nxt_mp_alloc(nxt_mp_t *mp, size_t size)
    NXT_MALLOC_LIKE;


/*
 * nxt_mp_zalloc() returns zeroed aligned freeable memory.
 * The alignment is sutiable to allocate structures.
 */
NXT_EXPORT void *nxt_mp_zalloc(nxt_mp_t *mp, size_t size)
    NXT_MALLOC_LIKE;

/* nxt_mp_align() returns aligned freeable memory. */
NXT_EXPORT void *nxt_mp_align(nxt_mp_t *mp, size_t alignment, size_t size)
    NXT_MALLOC_LIKE;

/* nxt_mp_zalign() returns zeroed aligned freeable memory. */
NXT_EXPORT void *nxt_mp_zalign(nxt_mp_t *mp, size_t alignment, size_t size)
    NXT_MALLOC_LIKE;

/* nxt_mp_free() frees freeable memory. */
NXT_EXPORT void nxt_mp_free(nxt_mp_t *mp, void *p);


/* nxt_mp_nget() returns non-aligned non-freeable memory. */
NXT_EXPORT void *nxt_mp_nget(nxt_mp_t *mp, size_t size)
    NXT_MALLOC_LIKE;

/*
 * nxt_mp_get() returns aligned non-freeable memory.
 * The alignment is sutiable to allocate structures.
 */
NXT_EXPORT void *nxt_mp_get(nxt_mp_t *mp, size_t size)
    NXT_MALLOC_LIKE;

/*
 * nxt_mp_zget() returns zeroed aligned non-freeable memory.
 * The alignment is sutiable to allocate structures.
 */
NXT_EXPORT void *nxt_mp_zget(nxt_mp_t *mp, size_t size)
    NXT_MALLOC_LIKE;


NXT_EXPORT nxt_int_t nxt_mp_cleanup(nxt_mp_t *mp, nxt_work_handler_t handler,
    nxt_task_t *task, void *obj, void *data);


NXT_EXPORT void nxt_mp_thread_adopt(nxt_mp_t *mp);


NXT_EXPORT void *nxt_mp_lvlhsh_alloc(void *pool, size_t size);
NXT_EXPORT void nxt_mp_lvlhsh_free(void *pool, void *p);

#endif /* _NXT_MP_H_INCLUDED_ */
