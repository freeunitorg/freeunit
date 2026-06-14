# unit-eol-check — EOL Matrix Validator

## Overview

Rust CLI that validates `pkg/eol.json` against [endoflife.date](https://endoflife.date) API. Reports wrong dates, missed EOL flags, and upcoming expirations.

## Files

```
pkg/eol/
├── Cargo.toml         # Rust binary: serde_json only (no serde derive)
├── Cargo.lock
├── src/
│   └── main.rs        # All logic: parse, fetch API, compare, report
├── Dockerfile         # FROM rust:slim-trixie + curl; no build-time compilation
├── entrypoint.sh      # Runtime: cargo build --release → exec binary
├── run.sh             # Local runner: docker build (cached) + docker run
└── PLAN.md            # This file
```

## Architecture

Mirrors `test/fake_upstream/` pattern — zero external Rust deps (only `serde_json`).
HTTP fetch via `std::process::Command::new("curl")` — curl installed in image.
`date +%Y-%m` via `std::process::Command` for current date — no chrono dep.

### Build pattern: base image only + entrypoint.sh builds at runtime

Docker image (`freeunit-eol-check:latest`) contains only:
- `rust:slim-trixie` base (Rust toolchain pre-installed)
- `curl` + `ca-certificates` (apt)
- `entrypoint.sh` at `/usr/local/bin/`

Compilation happens inside the container on each run (`entrypoint.sh`):
1. `cd /repo/pkg/eol` (source mounted via `-v $PWD:/repo`)
2. `cargo build --release` (registry cached via `~/.cargo/registry` volume mount)
3. Move binary to `/usr/local/bin/unit-eol-check`
4. `rm -rf target`
5. `exec unit-eol-check --json /repo/pkg/eol.json "$@"`

### CLI Flags

| Flag | Description |
|------|-------------|
| `--json PATH` | eol.json path (default: `pkg/eol.json`) |
| `--os` | Check OS versions only |
| `--runtimes` | Check runtime versions only |
| `--new` | Detect new versions missing from matrix (compares latest API cycle vs matrix) |
| `--ci` | CI mode: JSON output, `exit 1` if errors |
| `--fix` | Output corrected runtime EOL dates to stdout |
| `--quiet` | Suppress `[ OK ]` lines |
| `--help` | Show help |

### Modules

| Module | Purpose |
|--------|---------|
| `main:cli` | Argument parsing |
| `main:parse_eol_json` | Parse `pkg/eol.json` → `OsEntry`, `RuntimeEntry`, `Config` structs |
| `main:fetch_api` | `curl -sL` to `https://endoflife.date/api/{cat}.json` |
| `main:api_eol_date` | Find entry by version, extract `eol` (date string or `false` bool → `"future"`) |
| `main:check_os_entries` | Compare OS EOL dates; flag wrong dates, upcoming EOLs |
| `main:check_runtime_entries` | Same for runtime versions; flag missed `(EOL)` notes |
| `main:detect_new_versions` | Check API latest cycle vs matrix; flag missing entries |
| `main:report_human` | Human-readable `[ WARN ]` / `[ ERROR ]` output |
| `main:report_ci` | Machine JSON + `exit 1` on errors |
| `main:generate_fix` | `--fix` mode: corrected runtime dates with `supported_until` recalc |

### Output Examples

```
$ ./pkg/eol/run.sh --runtimes
[ WARN  ] go: go 1.24 upstream EOL passed (2026-02), add (EOL) flag
[ ERROR ] go: go 1.25 matrix EOL 2026-08 != actual 2026-02

$ ./pkg/eol/run.sh --ci
{"errors": 1, "warnings": 2, "items": [...]}
# exit 1

$ ./pkg/eol/run.sh --new
[ ERROR ] NEW: python 3.15 released 2026-10-01, not in matrix — add to pkg/eol.json
```

## Docker Build

```bash
# Via runner (builds image if not cached, then runs)
./pkg/eol/run.sh                      # human output, check all
./pkg/eol/run.sh --ci                 # CI mode: JSON + exit 1 on errors
./pkg/eol/run.sh --runtimes           # runtime versions only
./pkg/eol/run.sh --os                 # OS versions only
./pkg/eol/run.sh --new                # new versions not in matrix
./pkg/eol/run.sh --fix                # corrected runtime dates to stdout

# Manual
docker build -t freeunit-eol-check:latest -f pkg/eol/Dockerfile .
docker run --rm -v "$(pwd):/repo" -w /repo freeunit-eol-check:latest --ci
```

Image tag: `freeunit-eol-check:latest` (run.sh caches by tag — rebuild: `docker rmi freeunit-eol-check:latest`).

## Known Issues / TODO

- Runtime compile on every run: cargo registry cached via `~/.cargo/registry` volume (fast re-download skip), but no `target/` persistence → full recompile each invocation (~5s). Prebuild binary in image layer eliminates this — see Future Extensions.

## `--docker` mode: sync Docker builds with EOL policy

### Goal

Cross-reference `pkg/eol.json` runtimes against `pkg/docker/Makefile` `VERSIONS_*` lines
and actual `Dockerfile.*` files. Report which builds are missing (new version, no Dockerfile
yet) and which are stale (past FreeUnit `supported_until` date, should be dropped).
Optionally patch `pkg/docker/Makefile` in place.

### Dockerfile naming convention

| eol.json category | Dockerfile pattern | Notes |
|-------------------|--------------------|-------|
| `go` | `Dockerfile.go-{version}` | `golang:{version}-trixie` base |
| `node` | `Dockerfile.node-{version}` | `node:{version}-trixie` base |
| `perl` | `Dockerfile.perl-{version}` | `perl:{version}-trixie` base |
| `php` | `Dockerfile.php-{version}` | `php:{version}-cli-trixie` base |
| `python` | `Dockerfile.python-{version}`, `Dockerfile.python-{version}-slim` | two variants |
| `ruby` | `Dockerfile.ruby-{version}` | `ruby:{version}-trixie` base |
| `java` | `Dockerfile.java-{version}` | `eclipse-temurin:{version}-jdk-noble` base |
| `wasm` | `Dockerfile.wasm` | no version suffix, skip EOL check |
| `minimal` | `Dockerfile.minimal` | no version suffix, skip EOL check |

### New CLI flag: `--docker [DIR]`

`DIR` defaults to `/repo/pkg/docker` (matches volume mount in run.sh).

**Algorithm:**

1. Load eol.json runtimes, compute today's date.
2. For each runtime entry where `version != null` and category has a Dockerfile pattern:
   - **Active** = `supported_until` > today (or null/future)
   - **Stale** = `supported_until` ≤ today (past FreeUnit drop date)
3. Scan `DIR/Dockerfile.*` → build set of `(category, version)` present on disk.
4. Emit:
   - `[ ADD   ]` — active in eol.json, no Dockerfile exists → needs to be added
   - `[ DROP  ]` — Dockerfile exists, `supported_until` is past → candidate for removal
   - `[ WARN  ]` — Dockerfile exists, `supported_until` within 90 days → approaching drop
   - `[ OK    ]` — present and active

**Output example:**

```
[ ADD   ] node 26: Dockerfile.node-26 missing — supported until 2029-04
[ ADD   ] perl 5.42: Dockerfile.perl-5.42 missing — supported until 2029-07
[ ADD   ] ruby 4.0: Dockerfile.ruby-4.0 missing — supported until 2030-03
[ DROP  ] node 20: Dockerfile.node-20 — FreeUnit drop date 2027-04
[ WARN  ] go 1.24: Dockerfile.go-1.24 — drop in 9 months (2027-02)
[ OK    ] php 8.3 8.4 8.5 — in sync
```

Exit code: `0` = in sync · `1` = missing or stale Dockerfiles

### New CLI flag: `--docker-makefile [DIR]`

Reads current `pkg/docker/Makefile`, outputs corrected `VERSIONS_*` lines for each
category based on active (non-stale) versions from eol.json. With `--fix`: applies
in-place via temp file + rename.

**Example output:**

```
VERSIONS_go ?= 1.25 1.26
VERSIONS_node ?= 22 24 26
VERSIONS_perl ?= 5.38 5.40 5.42
VERSIONS_ruby ?= 3.3 3.4 4.0
```

After patching Makefile:
```bash
cd pkg/docker && make clean && make dockerfiles
```
regenerates all Dockerfiles for the new active set.

### Design decisions

- **DROP on `supported_until`, not `eol`**: grace period is deliberate policy. A version past upstream EOL but within the 12/36-month window is still actively supported by FreeUnit — removing its Dockerfile early would break users. `DROP` fires only when `supported_until` ≤ today.
- **`--docker-makefile` only patches `VERSIONS_*` lines** — does not regenerate Dockerfiles itself. After patching, user runs `cd pkg/docker && make clean && make dockerfiles`. Keeps Makefile as the single source for Dockerfile generation logic.
- **python**: one eol.json entry → two Dockerfiles (`Dockerfile.python-{v}` + `Dockerfile.python-{v}-slim`). Both generated, both checked.
- **wasm / minimal**: no `version` field in eol.json → skip EOL check entirely. Dockerfiles always present.
- **java**: `eclipse-temurin:{v}-jdk-noble` base (not trixie). `VARIANT_java ?= noble` in Makefile — no change needed.

### Current gap (2026-05-20)

| Needed | Missing Dockerfile |
|--------|--------------------|
| node 26 | `Dockerfile.node-26` |
| perl 5.42 | `Dockerfile.perl-5.42` |
| ruby 4.0 | `Dockerfile.ruby-4.0` |

These are active in eol.json (`supported_until` 2029–2030) but absent from `pkg/docker/`.
`--docker` will report them as `[ ADD ]`. Resolve by updating `VERSIONS_*` in Makefile
and running `make dockerfiles`.

### Implementation notes

- Add `check_docker(docker_dir: &str, runtimes: &[RuntimeEntry], config: &Config)` function.
- Filename → (category, version) parse: strip `Dockerfile.` prefix, split on first digit run.
- `apply_makefile_fix(makefile_path: &str, versions_map: &HashMap<String, Vec<String>>)`: read file, replace `VERSIONS_{cat} ?= ...` lines, write to `{path}.tmp`, rename.
- `run.sh` docker dir pass-through: `./run.sh --docker` passes `/repo/pkg/docker` implicitly.

### Files changed

| File | Change |
|------|--------|
| `src/main.rs` | `check_docker()`, `apply_makefile_fix()`, `--docker`/`--docker-makefile` CLI flags |
| `PLAN.md` | This section |

## Future Extensions

- Prebuild binary to `packages.freeunit.org` (like fake_upstream) — eliminates runtime compile
- CI step in `.github/workflows/build-test.yml` (scheduled weekly, non-blocking warning)
- `--days N`: configurable warning threshold (default: 365 days = 12 months)
- `--fix --os`: correct OS EOL dates too (currently runtime-only)
- JSON output format for `--new` mode
- `generate_fix` for `--os`: correct OS EOL dates too
