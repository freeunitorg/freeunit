# tools/perf

Codegen and struct-layout gates for the IPC hot paths.

## disasm-diff.sh

Builds `unitd` with the given compiler, disassembles the functions listed
in `hot-functions.txt` (plus the `noinline` wrappers in
`harness/mca_harness.c` for the header-only queue inlines), normalizes
addresses and diffs them against a baseline. It exits non-zero on any
change.

```sh
tools/perf/disasm-diff.sh --cc clang            # check
tools/perf/disasm-diff.sh --cc clang --update   # write the baseline
```

The baseline depends on the exact compiler, so it is not in the tree: build
it on the same machine from the commit to compare against, then check the
change. It lands in `tools/perf/baseline/<cc>-<version>-<libc>/` (ignored).

```sh
git switch --detach <base>; tools/perf/disasm-diff.sh --cc clang --update
git switch -;               tools/perf/disasm-diff.sh --cc clang
```

clang is the gate; gcc is informational.

## layout-check.sh

Dumps `pahole -C` layouts of the shared-memory structs (`nxt_nncq_t`,
`nxt_app_nncq_t`, `nxt_port_queue_t`, `nxt_app_queue_t`, their item types,
`nxt_port_mmap_header_t`, `nxt_unit_request_t`) and of two process-local
ones (`struct nxt_port_s`, `nxt_unit_ctx_impl_t`), and diffs them against
`layout-baseline/<cc>-<libc>.txt`, which is in the tree.

```sh
tools/perf/layout-check.sh --cc clang                 # check
tools/perf/layout-check.sh --cc clang --update        # rewrite the baseline
tools/perf/layout-check.sh --cc musl-gcc --update --configure-opt=--no-regex
```

A change to a process-local struct is a warning. A change to a
shared-memory struct fails: processes built at different times read these
structs, so it is an ABI break.

**`--allow-abi-bump`** makes a check report a shared-memory diff as a
warning instead of failing; `--update` ignores it. For a reviewed ABI
change, rewrite the baseline with `--update` in the same commit, so the
baseline diff shows in review, and say so in the commit message.

## bench-queues.sh

```sh
./configure --tests && make -j2 tests
tools/perf/bench-queues.sh build
```

Runs `build/queue_bench` for every queue, under `perf stat` when hardware
counters work, otherwise with its own `clock_gettime()` timings.

## CI

`.github/workflows/perf-gates.yml` runs both gates, building the
disasm baseline from the PR's base commit. It is `continue-on-error` for
now.
