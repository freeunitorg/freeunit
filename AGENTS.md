# Agent Instructions

This repository is FreeUnit, a community-maintained fork of NGINX Unit. Treat it
as a production C server with language runtimes, process isolation, dynamic JSON
configuration, and security-sensitive request parsing. For architecture, build,
and test details, see `CLAUDE.md`.

## Review Priorities

- Lead reviews with correctness, security, regressions, and missing tests.
- For C changes, scrutinize lifetime, ownership, allocator pairing, integer
  bounds, string length handling, member/flag-field initialization, fd/process
  cleanup, and async callback order.
- For HTTP, TLS, proxying, routing, static files, WebSocket, and config changes,
  check both normal behavior and malformed input paths.
- For isolation, mounts, cgroups, credentials, namespaces, and chroot/rootfs
  changes, assume privilege-boundary regressions are high severity.
- For language modules (`src/php`, `src/python`, `src/nodejs`, `src/ruby`,
  `src/perl`, `src/java`, `src/wasm*`, `go/`), verify module-specific lifecycle
  and version compatibility, not only the shared core behavior.
- For OpenAPI/docs/config schema changes, keep `docs/unit-openapi.yaml`, config
  validation, tests, and user-facing docs consistent.

## Verify Automated Findings

This file gives Claude, Codex, and Gemini a shared review baseline, but their
output is a starting point, not ground truth. Before trusting or acting on any
automated review:

- Re-read the actual diff and surrounding code for every finding; confirm the
  cited lines, types, and control flow really match the claim before repeating
  it. Automated reviewers routinely produce plausible-but-wrong findings.
- When applying an agent-suggested fix, verify it against the real code — that
  it builds, addresses the stated defect, and adds no regression — rather than
  committing on the strength of the suggestion alone.
- Prefer findings backed by a concrete failure scenario (input, state, and the
  resulting wrong behavior) over stylistic assertions.

## Project Layout

- `src/` contains the C core and language module integrations.
- `src/test/` contains C/unit-style test programs built when configured with
  `--tests`.
- `test/` contains the pytest integration suite and test harness.
- `test/unit/` contains Python helpers for control API, HTTP, status, logs, and
  Unit process lifecycle.
- `pkg/` contains packaging and Docker assets.
- `fuzzing/` contains libFuzzer targets and seed corpora.
- `docs/` contains manpages, changelog sources, the logo, and OpenAPI spec.
- `tools/` contains helper utilities, including `tools/unitctl/` — a separate
  Rust CLI built with cargo, not the root Makefile.

## Build And Test

Prefer the Docker test runners documented in `test/README.md`. Do not use host
builds as the primary signal for review conclusions because host dependencies
can hide CI failures.

Useful commands:

```bash
./test/run-local.sh -n
./test/run-local.sh -t test_tls.py
./test/run-local.sh python php
./test/run-local-full.sh -n
./test/run-local-full.sh
```

`./test/run-local.sh` runs the pytest suite in Docker. It copies the tree to a
temporary directory, configures with test support and common features, builds
needed modules, and runs `pytest-3 --print-log`.

`./test/run-local-full.sh` runs the clang-ast analysis build in Docker and is
the preferred extra check for C-core changes.

If you cannot run Docker in the current environment, say so clearly and fall
back to focused static inspection plus the narrowest available local command.

## Native Build Reference

Native commands are documented for reference and emergency debugging only:

```bash
./configure --openssl --njs --zlib --zstd --brotli --otel --tests
make -j"$(nproc)"
make tests
sudo pytest-3 --print-log test/
```

When reviewing or reporting verification, distinguish Docker-backed results
from host-only results.

## Coding Conventions

- Follow the existing C style in nearby files. Keep changes small and local.
- Avoid broad refactors while fixing a defect; preserve existing abstractions
  unless they are the defect.
- Prefer existing helpers for buffers, strings, JSON config values, queues,
  logging, and event scheduling.
- Keep public configuration behavior backward compatible unless the task is
  explicitly a breaking change.
- Update or add tests near the behavior changed. For integration behavior, use
  the pytest suite under `test/`; for low-level C helpers, use `src/test/` when
  an existing pattern fits.

## Review Checklist

- Identify the runtime path affected: controller, router, app process,
  language module, TLS, proxy, static, config validation, packaging, or docs.
- Check cleanup paths after every allocation, fd open, process spawn, mount, and
  async event registration.
- For structs obtained from non-zeroing allocators (`nxt_list_add` and similar),
  confirm every field — especially flag/skip bitfields — is assigned before use;
  leftover garbage bits cause nondeterministic, often CI-only failures. Prefer
  `nxt_list_zero_add` (or explicit zeroing) when the element is not fully set.
- Check reload/reconfiguration behavior, especially partial failures and rollback
  paths.
- Check logs and status output for sensitive data exposure and useful failures.
- Check tests for negative cases, restarts, repeated reconfiguration, and module
  prerequisites.
- For security fixes, look for adjacent variants and add regression coverage
  that fails without the fix.

## Git And PR Notes

- Do not revert user changes or unrelated untracked files, and do not modify or
  rely on untracked files in a contributor's working tree.
- PR titles should follow Conventional Commits, as described in
  `CONTRIBUTING.md`.
- Use labels consistently when working with GitHub issues or PRs: one type, one
  area, and severity when applicable.
