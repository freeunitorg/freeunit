# FreeUnit Test Suite

## CRITICAL: Docker-only

**ALL build and test commands MUST run inside Docker.** Never run
`./configure`, `make`, `pytest-3`, `python3`, or any language runtime
directly on the host — host drift hides bugs that surface in CI.

Use `./test/run-local.sh` (preferred). For one-shot commands, override
the fixed `ENTRYPOINT` (which is `bash -c "...build...exec pytest-3 $@"`)
and mount at `/unit` (the image `WORKDIR`):

```bash
docker run --rm --entrypoint bash -v "$(pwd):/unit" -w /unit \
    freeunit-test:local -c '<cmd>'
```

See project-root `CLAUDE.md` for the full allowed/forbidden list.

## Running Tests

Tests require **root privileges** because Unit creates Unix domain sockets,
network namespaces, and cgroups during isolation tests.

### Local Testing via Docker (Recommended)

Build and run tests inside an isolated Docker container that mirrors the CI
environment. No host system dependencies needed — the image is based on
`pkg/docker/template.Dockerfile` and includes Rust, njs, OpenSSL, and all
build tools.

```bash
# Run the full test suite (~30 minutes)
./test/run-local.sh

# Run only Python tests
./test/run-local.sh python

# Run a specific test file
./test/run-local.sh -t test_tls.py

# Run a single test function
./test/run-local.sh -t test_tls.py::test_tls_certificate_change

# Run multiple module test suites
./test/run-local.sh python php perl

# Dry-run — print commands without executing
./test/run-local.sh -n -t test_tls.py

# Show help
./test/run-local.sh -h
```

The script (`test/run-local.sh`) builds a `freeunit-test:local` image
mirroring `pkg/docker/template.Dockerfile` (Debian trixie, Rust 1.94.1,
njs 0.9.6, system libssl-dev). Source code is mounted via Docker volume,
so changes on the host are immediately reflected. The container then
builds FreeUnit with `--tests --openssl --njs --zlib --zstd --brotli --otel`,
builds the Python module, and runs `sudo -E pytest-3 --print-log`.

To force a rebuild of the image (e.g. after Dockerfile changes):
```bash
docker rmi freeunit-test:local
./test/run-local.sh python
```

### Fast prototyping with the pre-built builder image

`run-local.sh` builds a full test image from scratch (downloads Rust, Go, njs)
— slow for tight iteration. For prototyping a **proxy / TLS** test (no language
runtime needed), reuse the pre-built builder image
`ghcr.io/freeunitorg/freeunit-builder:trixie-rust1.94.1` (Rust + all C build
deps already baked in) and just mount the working tree. Build + run is ~30 s:

```bash
docker run --rm --privileged -v "$(pwd):/unit" -w /unit \
  ghcr.io/freeunitorg/freeunit-builder:trixie-rust1.94.1 bash -c '
    apt-get update -qq && apt-get install -y -qq python3-pytest python3-openssl
    ./configure --openssl --tests
    make -j"$(nproc)" unitd
    cargo build --release --manifest-path test/fake_upstream/Cargo.toml
    cp test/fake_upstream/target/release/fake_upstream /usr/local/bin/
    pytest-3 --print-log test/test_proxy_chunked.py -q
  '
```

Iterate by editing tests on the host (tree is mounted) and re-running; the C
core and `fake_upstream` rebuild incrementally.

**Caveats:**

- **No language module is built**, so a test gated on one is skipped. A test
  file with `prerequisites = {'modules': {'python': 'any'}}` (and a
  `ApplicationPython()` client) skips entirely here. For proxy/TLS-only cases,
  base the test on `ApplicationProto` (plain) or `ApplicationTLS` (TLS) and
  omit the language `prerequisites` so it runs on the minimal `--openssl
  --tests` build.
- The build writes `build/` into the mounted tree as **root**. Run
  `sudo rm -rf build` on the host afterwards, or use `run-local.sh` (copies to
  a tmp dir) when you want isolation.
- This path is for prototyping only. Before pushing, validate with
  `./test/run-local.sh` (full matrix) and, for C changes, the clang-ast check.

### Running Tests Directly on Host

If you prefer to run tests natively (requires all dependencies installed):

```bash
# 1. Build FreeUnit with test support
./configure --openssl --njs --zlib --zstd --brotli --otel --tests
make -j$(nproc)

# 2. Build required language modules
sudo ./configure python --config=python3-config
sudo make python3

./configure php
make php

./configure ruby
make ruby

# ... etc.

# 3. Run the full test suite
sudo pytest-3 --print-log test/

# 4. Run a specific test file
sudo pytest-3 --print-log test/test_tls.py

# 5. Run a single test function
sudo pytest-3 --print-log test/test_tls.py::test_tls_certificate_change

# 6. Run with restart mode (Unit restarts after every test)
sudo pytest-3 --print-log --restart test/

# 7. Save logs after execution
sudo pytest-3 --print-log --save-log test/

# 8. Run clang-ast AST analysis (C-code quality check)
./test/run-local.sh --clang-ast
```

## Test Structure

```
test/
├── conftest.py           # pytest fixtures, Unit lifecycle management
├── pytest.ini            # pytest configuration
├── requirements.txt      # Python dependencies (pyOpenSSL, pytest)
├── run-local.sh          # Docker-based local test runner
├── unit/                 # Test utilities (HTTP helpers, status checks, logging)
├── test_*.py             # Core and Python tests
├── test_go*/             # Go application and isolation tests
├── test_java*/           # Java application and isolation tests
├── test_node*/           # Node.js application tests
├── test_php*/            # PHP application and isolation tests
├── test_ruby*/           # Ruby application and isolation tests
├── test_perl*/           # Perl application tests
└── test_wasm*/           # WebAssembly and WASI component tests
```

## Known Issues

- **`test_tls_certificate_change`** — may fail if a previous test left
  stale TLS state. Re-run the test individually to confirm.
- **`--restart` mode** — significantly slower but catches state leakage
  between tests.
- **Process isolation tests** — require `--privileged` in Docker or real
  root on the host (namespaces, cgroups, pivot_root).

## CI

All tests run on GitHub Actions for every PR and push to `master`. See
`.github/workflows/ci.yml` for the full matrix (PHP 8.2–8.5, Python 3.11–3.12,
Go 1.21–1.22, Node.js 20–21, Java 17/18/21, Ruby 3.3/3.4, WASM, WASI).
