# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) and other AI
assistants when working with code in this repository. For review priorities and
the PR review checklist, see `AGENTS.md`.

## Project

FreeUnit is a community-maintained fork of NGINX Unit — a multi-process,
multi-language application server written in C. The fork's primary motivation is
PHP 8.4/8.5 support and ongoing security maintenance. Upstream was archived in
October 2025.

## Build

Unit uses a hand-written `configure` script (not autotools) that generates
`build/Makefile` and `build/include/nxt_auto_config.h`. Out-of-tree builds are
not supported; artifacts land under `build/`.

```console
$ ./configure --openssl --otel        # core daemon + TLS + OpenTelemetry
$ ./configure --debug                 # debug build
$ ./configure --tests                 # also build C test programs (src/test/)
$ ./configure --help                  # full option list
$ make                                # builds unitd + libunit
$ sudo make unitd-install
```

Language modules are configured separately and appended to the same Makefile.
The pattern is: re-run `./configure` in "module mode" for each runtime, then
`make <module>`:

```console
$ ./configure python --config=python3-config --module=python3
$ make python3

$ ./configure php --module=php
$ make php
```

Module subcommands live in `auto/modules/` (php, python, perl, ruby, go,
nodejs, java, wasm, wasm-wasi-component). After configuring a module, its build
rules are *appended* to `build/Makefile` — a `make clean` / re-`./configure`
cycle is usually needed when switching module configurations.

## Tests

Prefer the Docker test runners documented in `test/README.md` — host
dependencies can hide CI failures, so Docker-backed results are the primary
signal:

```console
$ ./test/run-local.sh -n                 # list what would run
$ ./test/run-local.sh -t test_tls.py     # single test file
$ ./test/run-local.sh python php         # select language modules
$ ./test/run-local-full.sh               # clang-ast analysis build (C-core changes)
```

`./test/run-local.sh` copies the tree to a temporary directory, configures with
test support and common features, builds the needed modules, and runs
`pytest-3 --print-log`.

Natively, tests are Python/pytest under `test/` and require **root** (they
spawn `unitd`, bind sockets, test isolation/chroot). `test/pytest.ini` sets
`-vvv -s --print-log` by default.

```console
$ sudo pytest-3 test/                           # full suite
$ sudo pytest-3 test/test_php_application.py    # single file
$ sudo pytest-3 test/test_php_application.py::test_php_application_variables
$ sudo pytest-3 --user=nobody --restart test/   # restart unitd between tests
```

Shared pytest helpers live in `test/unit/` (imported as
`from unit.http import HTTP1`, etc.). `conftest.py` discovers language
prerequisites automatically and skips tests whose runtime is missing.
Per-language app fixtures are under `test/python/`, `test/php/`, `test/go/`,
etc. Low-level C helper tests live in `src/test/` and are built with
`./configure --tests && make tests`. libFuzzer targets and seed corpora live
under `fuzzing/`.

## Architecture

Unit is a set of cooperating processes communicating over Unix socket pairs and
shared memory. Understanding which process owns what is essential.

**Processes** (see `src/nxt_process_type.h`, `src/nxt_main_process.c`,
`src/nxt_controller.c`, `src/nxt_router.c`):

- **main** — parent; spawns and supervises everything else, owns privileged
  operations (setuid, cgroups, mount namespaces).
- **controller** — serves the REST control API on the control socket
  (`control.unit.sock`). Validates config via `nxt_conf_validation.c`,
  distributes it to router.
- **router** — accepts client connections, parses HTTP (`nxt_h1proto.c`),
  routes (`nxt_http_route.c`), serves static files, proxies, terminates TLS,
  and forwards requests to app processes.
- **app processes** (one per configured application) — language-specific; load
  the user's code via a SAPI/embedding layer.
- **discovery** — short-lived helper that enumerates available language modules
  at startup.

All IPC between processes uses `nxt_port_*` (socket pair + message framing in
`src/nxt_port*.c`) and shared memory buffers (`nxt_port_memory.c`,
`nxt_shmem.*`). Request bodies and response data flow through shared memory to
avoid copying.

**Event loop** (`src/nxt_event_engine.c`): platform-abstracted reactor with
backends for epoll/kqueue/eventport/poll/select/devpoll/pollset. Each process
runs one event engine.

**Configuration** (`src/nxt_conf*.c`): internal JSON representation with
schema-driven validation. The controller is the single source of truth; changes
are serialized to disk and pushed to the router. The REST control API is
described in `docs/unit-openapi.yaml` — keep it, config validation, tests, and
user-facing docs consistent.

**libunit** (`src/nxt_unit.c`, `src/nxt_unit.h`): the C ABI that language
modules link against. It handles the app side of the port protocol, exposes
request/response primitives, and is what `nxt_php_sapi.c`, `nxt_python_wsgi.c`,
`nxt_python_asgi.c`, `nxt_ruby.c`, `nxt_perl.c`, `nxt_go_*`, `nxt_java_*`, and
`src/nodejs/unit-http/` build on top of.

**Language SAPIs** (`src/nxt_php_sapi.c`, `src/python/`, `src/perl/`,
`src/ruby/`, `src/java/`, `src/nodejs/`, `src/wasm*/`, `src/go/` — Go is a
user-imported package, not a module binary): Each module embeds the runtime
in-process and bridges its request model to libunit. PHP-specific fork logic
lives in `nxt_php_sapi.c`; Python has separate WSGI and ASGI entrypoints with
the latter split across `_http`, `_lifespan`, `_websocket`.

**Isolation** (`src/nxt_isolation.c`, `src/nxt_clone.c`, `src/nxt_cgroup.c`,
`src/nxt_fs_mount.c`, `src/nxt_capability.c`): Linux namespaces, cgroups v2,
rootfs/chroot, and capability dropping for app processes. Tested in
`test/test_*_isolation*.py`.

**Key headers** to read when adding features: `src/nxt_main.h` (common
prelude), `src/nxt_router.h` and `src/nxt_router_request.h` (request
lifecycle), `src/nxt_unit.h` (module ABI), `src/nxt_conf.h` (config tree).

## Code style

Existing C style: tabs/indentation and brace placement mirror nginx
conventions. No clang-format config is committed — follow what surrounding code
does. Commit subjects imperative mood, ~50 chars; body wrapped at 72.
Conventional-commit prefixes (`fix:`, `feat:`, `tests:`, `build:`, `docs:`) are
used in recent history; see `CONTRIBUTING.md`.

## Repo notes

- `tools/unitctl/` is a separate Rust CLI; build it with cargo, not the root
  Makefile.
- Docker images and CI live under `pkg/docker/` and `.github/workflows/`.
- `nxt_list_add()` returns *uninitialized* storage. If the element struct has
  flag/skip bitfields, unassigned bits carry garbage and cause nondeterministic,
  often CI-only bugs (e.g. headers randomly dropped). Use `nxt_list_zero_add()`
  or assign every field explicitly. The same "assign before use" rule applies to
  any non-zeroing pool/array allocation.
- Do not modify or rely on untracked files in a contributor's working tree.
