# Fuzzing unit

These tests are generally advised to run only on GNU/Linux.

## Build fuzzers using libFuzzer.

Running `sh fuzzing/build-fuzz.sh` can build all the fuzzers with standard
`ASan` and `UBSan`.

### More comprehensive How-to Guide.

#### Export flags that are to be used by Unit for fuzzing.

Note that in `CFLAGS` and `CXXFLAGS`, any type of sanitizers can be added.

- [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html),
    [ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html),
    [MemorySanitizer](https://clang.llvm.org/docs/MemorySanitizer.html),
    [UndefinedBehaviorSanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html),
    [LeakSanitizer](https://clang.llvm.org/docs/LeakSanitizer.html).

```shell
$ export CC=clang
$ export CXX=clang++
$ export CFLAGS="-g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -fsanitize=fuzzer-no-link"
$ export CXXFLAGS="-g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION  -fsanitize=fuzzer-no-link"
$ export LIB_FUZZING_ENGINE="-fsanitize=fuzzer"
```

#### Build Unit for Fuzzing.

```shell
$ ./configure --no-regex --no-pcre2 --fuzz=$LIB_FUZZING_ENGINE
$ make fuzz -j$(nproc)
```

#### Running fuzzers.

```shell
$ mkdir -p build/fuzz_basic_seed
$ mkdir -p build/fuzz_http_controller_seed
$ mkdir -p build/fuzz_http_h1p_seed
$ mkdir -p build/fuzz_http_h1p_peer_seed
$ mkdir -p build/fuzz_json_seed

$ ./build/fuzz_basic            build/fuzz_basic_seed            fuzzing/fuzz_basic_seed_corpus
$ ./build/fuzz_http_controller  build/fuzz_http_controller_seed  fuzzing/fuzz_http_seed_corpus
$ ./build/fuzz_http_h1p         build/fuzz_http_h1p_seed         fuzzing/fuzz_http_seed_corpus
$ ./build/fuzz_http_h1p_peer    build/fuzz_http_h1p_peer_seed    fuzzing/fuzz_http_seed_corpus
$ ./build/fuzz_json             build/fuzz_json_seed             fuzzing/fuzz_json_seed_corpus
```

Here is more information about [LibFuzzer](https://llvm.org/docs/LibFuzzer.html).

## Build fuzzers using other fuzzing engines.

- [Honggfuzz](https://github.com/google/honggfuzz/blob/master/docs/PersistentFuzzing.md).
- [AFLplusplus](https://github.com/AFLplusplus/AFLplusplus/blob/stable/utils/aflpp_driver/README.md).


## Requirements.

You will likely need at least the following packages installed (package names
may vary).

```
clang, llvm & compiler-rt
```

## In CI

`.github/workflows/fuzzing.yml` builds these targets with clang, ASan and
UBSan and runs `fuzzing/run-ci.sh`, which gives each target a bounded
libFuzzer budget against its seed corpus and fails on the first crash.

```shell
$ fuzzing/run-ci.sh                                   # all five, 60s each
$ fuzzing/run-ci.sh -t 120 fuzz_http_h1p              # one target, longer
```

A pull request that touches `src/nxt_http*`, `src/nxt_h1proto*`,
`src/nxt_controller*`, `src/nxt_conf*` or `fuzzing/` runs all five targets --
roughly five minutes; a manual `workflow_dispatch` takes a budget and a
target list as inputs.  Reproducers land in `build/fuzz-artifacts/` and are
uploaded as an artifact when the job fails.

Each target finishes within a second or two of its budget.  `run-ci.sh` caps
it at the budget plus a minute anyway -- libFuzzer only checks the budget
between runs -- and treats hitting that cap as a warning rather than a
failure, while distinguishing it from a kill that arrives early: an OOM is a
finding, not slowness.

It sets `UBSAN_OPTIONS=halt_on_error=1`.  `-fsanitize=undefined` is
recoverable by default -- UBSan prints the report and carries on, libFuzzer
exits 0, and the check goes green on a finding.  It deliberately does *not*
set `print_stacktrace=1`, which reintroduces the symbolizer cost below; the
report keeps its file:line and the reproducer is saved, so rerun the
reproducer with `UBSAN_OPTIONS=print_stacktrace=1` when you want the trace.

It also passes `-print_funcs=0`, which is worth knowing about if you run the
fuzzers by hand.  libFuzzer symbolizes every newly covered function to print
its `NEW_FUNC` line, and llvm-symbolizer needs about thirteen seconds per call
against a 14 MB ASan+UBSan binary.  On a fresh corpus -- which is what CI has
every time -- that is the whole run: `fuzz_json` managed **7 executions in 90
seconds** with the default, and **1,190,884 in 31 seconds** without it.  Crash
reports keep their symbols either way; those come from the sanitizer's stack
printer, not from libFuzzer's coverage output.

This is a smoke gate, not a campaign.  It replaced an OSS-Fuzz CIFuzz job
that could not have worked here: the OSS-Fuzz `unit` project is marked
`disabled: true` ("Project is archived") and its `main_repo` is
`https://github.com/nginx/unit`, so a run from this repository would have
built upstream's source rather than the pull request's.
