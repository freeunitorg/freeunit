/* SPDX-License-Identifier: Apache-2.0 */

/*
 * load-bench: time how the "wasm" (C API) language module gets a core
 * module ready, in three modes.
 *
 * "jit" replays nxt_wasmtime_init() in src/wasm/nxt_rt_wasmtime.c step for
 * step: wasm_engine_new() with the default config, store, linker with WASI,
 * fopen()/fread() of the module, wasmtime_module_new() (Cranelift compiles
 * every function), then instantiation.  The other modes change only how the
 * engine is made or how the module is obtained:
 *
 *   jit         wasm_engine_new(), wasmtime_module_new()
 *   cache       wasmtime_config_cache_config_load(cfg, TOML or NULL), then
 *               as jit; the second start with the same cache directory
 *               reads the compiled code instead of compiling
 *   precompile  as jit, then wasmtime_module_serialize() to OUT.cwasm
 *   aot         wasmtime_module_deserialize_file(OUT.cwasm), no compiler run
 *   aot-mem     fopen()/fread() of the .cwasm, wasmtime_module_deserialize()
 *
 * Host imports the guest expects from unitd are defined to trap, which is
 * enough to instantiate: nothing is called.
 *
 * Prints one line of key=value timings in milliseconds; "main" is measured
 * from process start (CLOCK_MONOTONIC vs. /proc/self/stat start time is too
 * coarse, so the caller adds exec/ld.so time from an outer wall clock).
 *
 * Build:  cc -O2 -o load-bench load-bench.c -I$W/include -L$W/lib \
 *             -Wl,-rpath,$W/lib -lwasmtime
 * with W=pkg/contrib/wasmtime/artifacts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wasm.h>
#include <wasi.h>
#include <wasmtime.h>


static double
now_ms(void)
{
    struct timespec  ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}


static void
die(const char *what, wasmtime_error_t *error, wasm_trap_t *trap)
{
    wasm_byte_vec_t  msg;

    if (error != NULL) {
        wasmtime_error_message(error, &msg);
    } else if (trap != NULL) {
        wasm_trap_message(trap, &msg);
    } else {
        fprintf(stderr, "load-bench: %s\n", what);
        exit(1);
    }

    fprintf(stderr, "load-bench: %s: %.*s\n", what, (int) msg.size, msg.data);
    exit(1);
}


static void
read_file(const char *path, wasm_byte_vec_t *out)
{
    FILE  *fp;
    long   size;

    fp = fopen(path, "r");
    if (fp == NULL) {
        die(path, NULL, NULL);
    }

    fseek(fp, 0L, SEEK_END);
    size = ftell(fp);
    rewind(fp);

    wasm_byte_vec_new_uninitialized(out, size);

    if (fread(out->data, size, 1, fp) != 1) {
        die("fread", NULL, NULL);
    }

    fclose(fp);
}


int
main(int argc, char **argv)
{
    const char          *mode, *path, *cwasm, *cache_toml;
    double               t0, t_engine, t_read, t_compile, t_inst, t_ser;
    wasm_config_t       *config;
    wasm_engine_t       *engine;
    wasm_byte_vec_t      wasm, ser;
    wasi_config_t       *wasi;
    wasmtime_store_t    *store;
    wasmtime_error_t    *error;
    wasmtime_linker_t   *linker;
    wasmtime_module_t   *module;
    wasmtime_context_t  *ctx;

    t0 = now_ms();

    if (argc < 3) {
        fprintf(stderr, "usage: %s jit|cache|precompile|aot|aot-mem "
                        "MODULE.wasm [OUT.cwasm] [CACHE.toml]\n", argv[0]);
        return 2;
    }

    mode = argv[1];
    path = argv[2];
    cwasm = argc > 3 ? argv[3] : NULL;
    cache_toml = argc > 4 ? argv[4] : NULL;

    if (strcmp(mode, "cache") == 0) {
        config = wasm_config_new();
        error = wasmtime_config_cache_config_load(config, cache_toml);
        if (error != NULL) {
            die("cache config", error, NULL);
        }
        engine = wasm_engine_new_with_config(config);

    } else {
        engine = wasm_engine_new();
    }

    store = wasmtime_store_new(engine, NULL, NULL);
    ctx = wasmtime_store_context(store);
    linker = wasmtime_linker_new(engine);

    error = wasmtime_linker_define_wasi(linker);
    if (error != NULL) {
        die("define wasi", error, NULL);
    }

    t_engine = now_ms();
    t_ser = 0;

    if (strcmp(mode, "aot") == 0) {
        t_read = t_engine;
        error = wasmtime_module_deserialize_file(engine, cwasm, &module);
        if (error != NULL) {
            die("deserialize_file", error, NULL);
        }

    } else if (strcmp(mode, "aot-mem") == 0) {
        read_file(cwasm, &ser);
        t_read = now_ms();
        error = wasmtime_module_deserialize(engine, (uint8_t *) ser.data,
                                            ser.size, &module);
        wasm_byte_vec_delete(&ser);
        if (error != NULL) {
            die("deserialize", error, NULL);
        }

    } else {
        read_file(path, &wasm);
        t_read = now_ms();
        error = wasmtime_module_new(engine, (uint8_t *) wasm.data, wasm.size,
                                    &module);
        wasm_byte_vec_delete(&wasm);
        if (error != NULL) {
            die("module_new", error, NULL);
        }
    }

    t_compile = now_ms();

    if (strcmp(mode, "precompile") == 0) {
        FILE  *fp;

        error = wasmtime_module_serialize(module, &ser);
        if (error != NULL) {
            die("serialize", error, NULL);
        }

        fp = fopen(cwasm, "w");
        if (fp == NULL || fwrite(ser.data, ser.size, 1, fp) != 1) {
            die("write cwasm", NULL, NULL);
        }

        fclose(fp);
        wasm_byte_vec_delete(&ser);
        t_ser = now_ms() - t_compile;
    }

    wasi = wasi_config_new();
    wasi_config_inherit_env(wasi);
    wasi_config_inherit_stdin(wasi);
    wasi_config_inherit_stdout(wasi);
    wasi_config_inherit_stderr(wasi);

    error = wasmtime_context_set_wasi(ctx, wasi);
    if (error != NULL) {
        die("set wasi", error, NULL);
    }

    error = wasmtime_linker_define_unknown_imports_as_traps(linker, module);
    if (error != NULL) {
        die("define imports", error, NULL);
    }

    /* As nxt_wasmtime_init(): instantiates, runs "_initialize". */
    error = wasmtime_linker_module(linker, ctx, "", 0, module);
    if (error != NULL) {
        die("instantiate", error, NULL);
    }

    t_inst = now_ms();

    printf("mode=%s engine=%.3f read=%.3f load=%.3f instantiate=%.3f "
           "serialize=%.3f main_total=%.3f\n",
           mode, t_engine - t0, t_read - t_engine, t_compile - t_read,
           t_inst - t_compile - t_ser, t_ser, t_inst - t0);

    wasmtime_module_delete(module);
    wasmtime_linker_delete(linker);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);

    return 0;
}
