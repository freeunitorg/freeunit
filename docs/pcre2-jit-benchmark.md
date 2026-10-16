# PCRE2 JIT benchmark, 2026-10-03

These are exploratory measurements, not a release performance claim. Both
hosts had unrelated activity. The A/A runs were too variable to establish a
small noise floor. A confidence interval from the A/B repetitions does not
account for that uncontrolled activity.

## Setup

Both hosts used the same `fu-bench:latest` image:
`sha256:7c73528ebc8e61340b19f0be017847302d239b23d6c05f11ac1357c6164530e8`.
It contains GCC 14.2.0, glibc 2.41, PCRE2 10.46 and oha 1.16.0. The host
kernel was 6.8.0-136-generic on both machines.
Both have an Intel Xeon E3-1245 v5 with four physical cores and eight SMT
threads.

The source was this implementation on merged PR 520. Optimized strict builds
differed only by `--regex-jit`. Builds used `make -j4` inside a container
restricted to CPUs 0-3. At measurement time both hosts had identical binary
hashes:

```text
off 1b375f2e2b623cedd29e4b57c6c5c07e0e5575d5edfe169782d8d6fabfed48b7
on  7976f6fd0f40470dd0f57f83a29d579f03d737501d13bcfe7cc2c6dcebd04354
```

Later test-only changes updated debug metadata in the JIT binary. Removing
debug sections and the build ID with `objcopy --strip-debug
--remove-section=.note.gnu.build-id` gives byte-identical original and final
images, with SHA-256
`16244ddfded584eb26301e98e40a951b6e8aaeaa4675a95568818b7468a2eced`.

Each server ran in a private container network namespace. The router was
pinned to CPU 2. The load generator used CPUs 0 and 1, not CPU 6, which is
CPU 2's SMT sibling. The governor was `performance`. This does not reserve
those cores against unrelated host tasks.

The harness `tools/bench-release.sh` was adapted to load workload JSON,
request `/catalog/section/item42`, require exactly 100,000 successful
responses, use unique cell directories, and stop the owned process group.
Startup waited for the control socket rather than a fixed 1.2-second delay.
The existing router-thread CPU accounting and eight-cell ordering were kept.
The adapted script's SHA-256 is
`d558168e377f1f565bf61fc2b345c832bb54dc035922939c65de2e930e393bb4`.

Each workload had an A/A run, normal A/B run and swapped A/B run. Every run
had one discarded warm-up block and three measured blocks. Each block used
`A B B A B A A B`. Each cell made 100,000 requests with 16 connections and
a target rate of 60,000 requests/second. All measured cells completed with
exactly 100,000 HTTP 200 responses.

Reported CPU is router request-thread CPU time per request. The rate cap
means these are not maximum-throughput measurements. CPU-saving intervals
combine the normal and swapped per-block differences using Student's t
with four degrees of freedom. A/A intervals use two degrees of freedom.

## Workloads

All configurations set `listen_threads` to 1 and return an empty HTTP 200.

- `return`: no regex conditions.
- `regex1`: one matching anchored URI regex.
- `regex32`: 31 anchored URI regex misses followed by the matching regex.

The matching pattern is:

```text
^/(?:[[:alnum:]_.-]+/){0,4}(?:article|product|item)[0-9]+(?:/[[:alnum:]_-]+)?$
```

For miss number `i`, 0 through 30, replace `(article|product|item)` with
`(article|product|entry)` and append the decimal `i` before `[0-9]+`. The
actual groups are noncapturing, as in the matching pattern. A final
unconditional HTTP 200 route is present in every configuration.

These workloads do not measure unanchored callout overhead, compression,
application execution, configuration-load cost or executable-memory usage.

## Results

Positive CPU saved means the JIT build consumed less router CPU.

| Host | Workload | Off, us/request | On, us/request | CPU saved, us (95% CI) | A/A delta, 95% CI half-width |
|---|---|---:|---:|---:|---:|
| pro4s-2 | return | 12.09 | 12.38 | -0.29 [-1.73, 1.16] | +3.74% +/- 6.48% |
| pro4s-2 | regex1 | 11.86 | 11.57 | 0.29 [-1.61, 2.19] | +2.44% +/- 5.27% |
| pro4s-2 | regex32 | 16.42 | 14.25 | 2.16 [0.80, 3.53] | -11.04% +/- 38.78% |
| pro4s-4 | return | 10.24 | 10.71 | -0.47 [-0.86, -0.09] | -1.73% +/- 13.54% |
| pro4s-4 | regex1 | 11.75 | 10.77 | 0.98 [-0.94, 2.91] | +1.14% +/- 38.17% |
| pro4s-4 | regex32 | 17.05 | 13.62 | 3.43 [1.19, 5.67] | -8.08% +/- 46.39% |

The 32-regex point estimates correspond to about 13% and 20% less router
CPU. They are not validated speedup percentages. In particular, the
no-regex control moved on pro4s-4 although it never calls the matcher.
The next useful measurement is the same matrix on reserved, quiet physical
cores, with more repetitions and a stable A/A gate.

Raw TSVs, workload JSON and adapted scripts are retained on both hosts in
`/data/projects/unit-pcre2-jit-benchmark`. Builds are in the separate
`/data/projects/unit-pcre2-jit-bench-off` and
`/data/projects/unit-pcre2-jit-bench-on` directories.
