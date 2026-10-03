# Optional PCRE2 JIT

Configure with `--regex-jit` to try JIT compilation of configuration regexes.
It is off by default and requires the PCRE2 backend. PCRE 1 is unchanged.

The linked library is queried with `PCRE2_CONFIG_JIT`. No architecture
whitelist is used. A pattern that cannot be JIT compiled remains usable in
the interpreter, including when executable memory cannot be allocated.
Compilation happens while loading the configuration, never on a request.

Compiled patterns have a configuration-pool cleanup handler that calls
`pcre2_code_free()` before the pool storage is destroyed. This also covers
patterns compiled before a later configuration error. JIT executable memory
is not released merely by destroying the pool.

Matches use PCRE2's default bounded 32 KiB stack on the calling thread. No
custom mutable stack is shared by workers. If JIT exhausts that stack, the
match is retried once in the interpreter with the existing match limit.
The remaining whole-search callout budget is preserved across this retry.
Match-limit failures are never retried. The retry can add CPU work, but
allows patterns that need more stack to retain interpreted behaviour.

JIT honours the match limit but counts work differently. It does not enforce
the interpreter depth or heap limits. Unanchored patterns retain the
whole-search callout budget added by PR 520. That is a work counter, not a
wall-clock deadline. This option is not a ReDoS fix.

## Implementation and validation plan

1. Add opt-in configuration, capability detection and nonfatal fallback.
2. Add pattern-pool cleanup before enabling executable allocations.
3. Keep the default thread-local stack and a single bounded-policy retry.
4. Pin actual JIT execution, positive/negative results, compile fallback,
   stack retry and successful/failed configuration cleanup in C tests.
5. Run strict builds and hook-isolation checks with JIT enabled and disabled.
   Exercise routing/compression tests and the existing match-limit cases.
6. Validate with a no-JIT PCRE2 library and relevant architecture/package
   combinations. Forced failure tests supplement, not replace, those runs.
7. Measure representative routes, configuration cost and memory overhead.
   Use pinned unshared cores, an A/A baseline and uncertainty estimates.
   Do not claim an end-to-end performance gain before those measurements.

Builds use at most four jobs. The PR remains experimental until portability,
leak checks and performance measurements are complete.

## Validation on 2026-10-03

The implementation is based on merged PR 520, including its callout and heap
limits. Strict JIT-enabled builds passed with PCRE2 10.42. A PCRE2 10.46
library built with `--disable-jit` passed the existing limit tests with
`--regex-jit` configured in Unit. The JIT-specific execution cases explicitly
report a skip on that library.

JIT-enabled C tests passed with AddressSanitizer, UndefinedBehaviorSanitizer
and LeakSanitizer using PCRE2 10.46. The hook-isolation gate passed in the
normal, no-JIT-library and sanitizer builds. Hook counters verify actual JIT
execution, not just a successful call to the compiler.

The focused routing, compression and static-types suite passed 161 tests,
with two skips and three IPv6 deselections, in an isolated container.

Negative controls failed as intended:

- Disabling JIT compilation: `compiled 0, expected 128`.
- Omitting configuration cleanup: `freed 0, expected 128`.
- Removing stack retry: 8192-byte repeated group returned `-1`, expected `1`.
- Replenishing the retry budget: returned `1`, expected `-1`.

Native non-x86 architecture coverage, executable-memory accounting and
configuration-load overhead measurements remain outstanding. See
[the benchmark notes](pcre2-jit-benchmark.md) for the two-host measurements.

Related: https://github.com/freeunitorg/freeunit/issues/545 and
https://github.com/freeunitorg/freeunit/pull/520.

Vendor reference: [PCRE2 JIT support](https://www.pcre.org/current/doc/html/pcre2jit.html)
documents capability detection, executable-code lifetime and stack ownership.
