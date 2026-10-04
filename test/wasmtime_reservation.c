/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) FreeUnit contributors. */

/*
 * LD_PRELOAD shim for test_wasm_memory_move.py.
 *
 * On a 64-bit host Wasmtime reserves 4 GiB of address space for a 32-bit
 * linear memory, so memory.grow never moves the memory.  This shim sets
 * memory_reservation and memory_reservation_for_growth to 0 for every engine
 * the process creates.  A memory then has no room to grow in place: each
 * memory.grow maps a new region, copies the memory and unmaps the old region.
 *
 * The shim also wraps wasmtime_memory_data().  It writes a line to stderr each
 * time the base differs from the base it returned before.  In a Unit
 * application stderr is unit.log, so the test can count the moves that the
 * host saw.
 *
 * Each wrapper calls the real function through dlsym(RTLD_NEXT).  RTLD_NEXT
 * finds libwasmtime.so because src/nxt_application.c loads modules with
 * RTLD_GLOBAL.  If a symbol is missing, the shim aborts the process, so that
 * the test fails instead of running with the default reservation.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


/* Opaque Wasmtime types.  The shim only passes pointers to them. */
typedef struct wasm_config_t     wasm_config_t;
typedef struct wasm_engine_t     wasm_engine_t;
typedef struct wasmtime_context  wasmtime_context_t;
typedef struct wasmtime_memory   wasmtime_memory_t;

typedef wasm_config_t *(*nxt_config_new_fn)(void);
typedef void (*nxt_config_set_fn)(wasm_config_t *config, uint64_t value);
typedef wasm_engine_t *(*nxt_engine_new_fn)(wasm_config_t *config);
typedef uint8_t *(*nxt_memory_data_fn)(wasmtime_context_t *store,
    const wasmtime_memory_t *memory);


wasm_engine_t *wasm_engine_new(void);
wasm_engine_t *wasm_engine_new_with_config(wasm_config_t *config);
uint8_t *wasmtime_memory_data(wasmtime_context_t *store,
    const wasmtime_memory_t *memory);


static void *
nxt_shim_real(const char *name)
{
    void  *sym;

    sym = dlsym(RTLD_NEXT, name);

    if (sym == NULL) {
        fprintf(stderr, "wasmtime_reservation: %s not found\n", name);
        abort();
    }

    return sym;
}


wasm_engine_t *
wasm_engine_new(void)
{
    nxt_config_new_fn  config_new;

    config_new = (nxt_config_new_fn) nxt_shim_real("wasm_config_new");

    return wasm_engine_new_with_config(config_new());
}


wasm_engine_t *
wasm_engine_new_with_config(wasm_config_t *config)
{
    nxt_config_set_fn  set_reservation, set_growth;
    nxt_engine_new_fn  engine_new;

    set_reservation = (nxt_config_set_fn)
        nxt_shim_real("wasmtime_config_memory_reservation_set");
    set_growth = (nxt_config_set_fn)
        nxt_shim_real("wasmtime_config_memory_reservation_for_growth_set");
    engine_new = (nxt_engine_new_fn)
        nxt_shim_real("wasm_engine_new_with_config");

    set_reservation(config, 0);
    set_growth(config, 0);

    fprintf(stderr, "wasmtime_reservation: engine with no reservation\n");

    return engine_new(config);
}


uint8_t *
wasmtime_memory_data(wasmtime_context_t *store,
    const wasmtime_memory_t *memory)
{
    uint8_t                    *base;
    static uint8_t             *last;
    static nxt_memory_data_fn  memory_data;

    if (memory_data == NULL) {
        memory_data = (nxt_memory_data_fn)
            nxt_shim_real("wasmtime_memory_data");
    }

    base = memory_data(store, memory);

    if (last != NULL && base != last) {
        fprintf(stderr, "wasmtime_reservation: memory base moved\n");
    }

    last = base;

    return base;
}
