# FreeUnit Docker images

This directory contains Dockerfiles for all FreeUnit language variants and
a helper script (`build-local.sh`) for building them locally — mirroring
the behavior of `.github/workflows/release-docker.yml`.

## Security: PHP session upload progress

The PHP images set `session.upload_progress.enabled = 0` through the ini fragment
`/usr/local/etc/php/conf.d/zz-freeunit-security.ini`. The `RUN_php` value in the
`Makefile` creates the file. The command `make dockerfiles` copies the value into
`Dockerfile.php-*`.

The setting applies to every PHP application in the image. It also applies to the
applications that a user adds later through the control API. The `zz-` prefix makes
the file load last in `conf.d`, so it wins over the directives of the files that load
before it.

The PHP default is on, so the change removes the session upload progress feature
from the image. See `pkg/community/PHP.CVE.md` for the CVE map.

Check it in a built image:

```bash
docker run --rm --entrypoint sh ghcr.io/freeunitorg/freeunit:latest-php-8.5 \
    -c "php -i | grep -E '^session\.upload_progress\.enabled'"
# Expected: session.upload_progress.enabled => Off => Off
```

## Security: CVE-2026-31431 (AF_ALG privilege escalation)

CVE-2026-31431 is a **kernel bug** (CVSS 7.8) — local privilege escalation via
`algif_aead` / `AF_ALG` socket interface. The kernel patch is the definitive
fix; the seccomp profile below is a **container-level mitigation** that blocks
the attack surface until the host kernel is patched.

All FreeUnit images ship `/usr/share/unit/seccomp-no-af-alg.json` — a seccomp
profile that blocks `socket(AF_ALG, ...)` (domain 38).

> **Important:** the profile is **not applied automatically**. You must pass
> `--security-opt seccomp=...` explicitly on every `docker run`. Without it,
> containers run with Docker's default profile only, which does not block AF_ALG.

**Profile design note:** uses `defaultAction: SCMP_ACT_ALLOW` with an explicit
deny rule. This is intentional — libseccomp ORs multiple `NE` conditions on the
same argument index, making an `ERRNO`-default + `NE`-allowlist approach
unreliable for blocking specific socket domains. Using `--security-opt` replaces
Docker's default profile entirely, so this profile blocks only AF_ALG by design.
Do not change to `SCMP_ACT_ERRNO` without re-running
`test/security/seccomp/test-af-alg.sh`.

Apply at runtime (path must be on the **host** filesystem, run from repo root):

```bash
# Option 1: use the profile from this repo directly
docker run --security-opt seccomp=pkg/docker/seccomp-no-af-alg.json \
    ghcr.io/freeunitorg/freeunit:latest-php-8.4

# Option 2: extract from a pulled image
CNAME=$(docker create ghcr.io/freeunitorg/freeunit:latest-minimal)
docker cp "$CNAME":/usr/share/unit/seccomp-no-af-alg.json ./seccomp-no-af-alg.json
docker rm "$CNAME"
docker run --security-opt seccomp=./seccomp-no-af-alg.json \
    ghcr.io/freeunitorg/freeunit:latest-php-8.4
```

Verify the profile is active (AF_ALG socket must be denied):

```bash
# Should exit with "Operation not permitted" — confirms AF_ALG is blocked
docker run --rm --security-opt seccomp=pkg/docker/seccomp-no-af-alg.json \
    ghcr.io/freeunitorg/freeunit:latest-python-3.13-slim \
    python3 -c "import socket; socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET)"
# Expected: PermissionError: [Errno 1] Operation not permitted

# Normal TCP socket must still work — confirms no regression
docker run --rm --security-opt seccomp=pkg/docker/seccomp-no-af-alg.json \
    ghcr.io/freeunitorg/freeunit:latest-python-3.13-slim \
    python3 -c "import socket; s = socket.socket(); s.close(); print('OK')"
# Expected: OK
```

Host-level workaround (unpatched kernels):

```bash
echo "install algif_aead /bin/false" | sudo tee /etc/modprobe.d/disable-algif.conf
sudo rmmod algif_aead 2>/dev/null || true
```

## Prerequisites (Ubuntu 24.04 LTS)

### Docker Engine

```bash
# Remove any old Docker packages
sudo apt-get remove -y docker.io docker-doc docker-compose \
    docker-compose-v2 podman-docker containerd runc 2>/dev/null || true

# Add Docker's official apt repository
sudo apt-get update
sudo apt-get install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
    -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] \
  https://download.docker.com/linux/ubuntu \
  $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io \
    docker-buildx-plugin docker-compose-plugin

# Allow running docker without sudo (re-login required)
sudo usermod -aG docker "$USER"
```

### GNU parallel (optional, for `-j N` parallel builds)

```bash
sudo apt-get install -y parallel
```

### apt-cacher-ng (optional, speeds up repeated builds)

Caches downloaded `.deb` packages locally — subsequent builds skip re-downloading
~100 MB of build tools per variant. Start once, works automatically:

```bash
docker run -d --name apt-cacher-ng --restart=always \
  -p 3142:3142 \
  -v apt-cacher-ng:/var/cache/apt-cacher-ng \
  sameersbn/apt-cacher-ng
```

`build-local.sh` detects apt-cacher-ng automatically (checks port 3142) and
passes it as an APT proxy via `--build-arg`. No configuration needed.

### Verify installation

```bash
docker version
docker buildx version
```

## Building images locally

```bash
cd pkg/docker
```

| Command | Description |
|---------|-------------|
| `./build-local.sh` | Build **all** variants, `nproc/2` in parallel |
| `./build-local.sh minimal php-8.5` | Build only the listed variants |
| `./build-local.sh -j4` | Build 4 variants in parallel (requires `parallel`) |
| `./build-local.sh -b minimal php-8.5` | Fast build via pre-built builder images (no apt/Rust download) |
| `./build-local.sh -b` | Fast build all builder-supported variants |
| `./build-local.sh -v 1.35.2` | Pin a specific FreeUnit version |
| `./build-local.sh -p linux/arm64` | Build for a specific platform |
| `./build-local.sh -p linux/amd64,linux/arm64 -j2` | Multi-arch build (requires buildx) |
| `./build-local.sh -n` | Dry-run — print commands without executing |

If apt-cacher-ng is running on port 3142, it is detected automatically and used
as an APT proxy — no flags needed.

### Options

```
-v VERSION   FreeUnit version string to pin (default: current git branch name)
-p PLATFORM  Target platform (default: host arch, e.g. linux/amd64)
-j N         Number of parallel builds (default: nproc/2)
-b           Builder mode — fast local builds via pre-built builder images
             Skips apt install and Rust download; supported: minimal, wasm, php-8.5
-n           Dry-run — print commands, do not execute
-h           Show help
```

### Builder mode (-b): fast local iteration

For `minimal`, `wasm`, and `php-8.5` variants, `-b` uses pre-built builder images
that already contain all build tools and Rust. This eliminates ~100 MB apt download
and ~70 MB Rust download per build — useful for iterating locally.

Builder images (`Dockerfile.builder-*`) are pulled from GHCR automatically, or
built locally on first use if unavailable:

```
Dockerfile.builder-trixie   →  ghcr.io/freeunitorg/freeunit-builder:trixie-rust1.95.0
Dockerfile.builder-php8.5   →  ghcr.io/freeunitorg/freeunit-builder:php8.5-rust1.95.0
```

The `local/` subdirectory contains the corresponding multi-stage Dockerfiles used
with `-b`. These files are **not** used by CI — the GitHub Actions workflow always
uses single-stage `Dockerfile.*` files without builder images.

### Logs

Each build writes a log to `pkg/docker/build-logs/<variant>.log`.
A summary (OK / FAILED) is printed at the end.

## Available variants

| Variant | Base image |
|---------|-----------|
| `minimal` | debian:trixie-slim |
| `wasm` | debian:trixie-slim |
| `go-1.25` | golang:1.25 |
| `go-1.26` | golang:1.26 |
| `go-1.27` | golang:1.27 |
| `java-17` | eclipse-temurin:17-jdk-resolute |
| `java-21` | eclipse-temurin:21-jdk-resolute |
| `java-25` | eclipse-temurin:25-jdk-resolute |
| `java-26` | eclipse-temurin:26-jdk-resolute |
| `java-27` | eclipse-temurin:27-jdk-resolute |
| `node-22` | node:22 |
| `node-24` | node:24 |
| `node-26` | node:26 |
| `perl-5.38` | perl:5.38 |
| `perl-5.40` | perl:5.40 |
| `perl-5.42` | perl:5.42 |
| `perl-5.44` | perl:5.44 |
| `php-8.3` | php:8.3-cli |
| `php-8.4` | php:8.4-cli |
| `php-8.5` | php:8.5-cli-trixie |
| `python-3.12` | python:3.12 |
| `python-3.12-slim` | python:3.12-slim |
| `python-3.13` | python:3.13 |
| `python-3.13-slim` | python:3.13-slim |
| `python-3.14` | python:3.14 |
| `python-3.14-slim` | python:3.14-slim |
| `ruby-3.3` | ruby:3.3 |
| `ruby-3.4` | ruby:3.4 |
| `ruby-4.0` | ruby:4.0 |

## Alpine / musl allocator note

On musl (Alpine) FreeUnit uses 12 to 17 percent more server CPU per request
than on glibc, and the extra is system time: musl's allocator returns freed
blocks to the kernel at once, so steady-state allocations cost `mmap`,
`munmap` and page faults on every request. In the router this is largely
fixed since 1.37.1 (per-thread reuse of freed pool clusters); under Drupal
the gap lives in PHP's own allocations and preloading another allocator
closes it. Measured on Drupal 12 `/user/login` (arm64):

| variant | system us/req | page faults/req |
|---------|--------------|-----------------|
| musl mallocng | 1162 | 199 |
| musl + `LD_PRELOAD=libjemalloc.so.2` | 479 | 48 |
| glibc (Debian, same host) | 496 | 1.4 |

mimalloc (v3, built from source; the Alpine package ships v2) measures the
same way. The preload must reach every process of the instance — unitd and
all application workers — e.g. `Environment=LD_PRELOAD=...` in the service
file or `-e LD_PRELOAD=...` on `docker run`. Memory use of the preloaded
allocator was measured next to CPU in
https://github.com/freeunitorg/freeunit/issues/688 before choosing one.

Limitation: the preload travels through the environment, so it has no effect
where secure execution drops it — a set-id unitd or a unitd binary with file
capabilities (for example to bind privileged ports that way) keeps mallocng
with no warning. The test suite checks the same rule in
`test/test_capget_fallback.py:152-175`. Linking the allocator at build time
is the alternative for such deployments.

## Reference build environment

Measured full build (all 23 variants, `./build-local.sh`) on the maintainer's local machine:

| Parameter | Value |
|-----------|-------|
| CPU | AMD Ryzen 7 5700X — 8 cores / 16 threads |
| RAM | 32 GiB |
| Storage | NVMe SSD |
| Docker | 29.5.0, `overlay2` |
| Parallelism | 8 builds in parallel (`-j 8`, default `nproc/2`) |
| APT cache | apt-cacher-ng on port 3142 (auto-detected) |
| **Total time** | **~58 min** (23 variants, cold Docker layer cache) |

With apt-cacher-ng, repeated builds that share base layers are significantly faster.
Builder mode (`-b`) for `minimal`, `wasm`, and `php-8.5` cuts those three variants to
~2–3 min each by skipping apt and Rust downloads entirely.

### Build flags

`--cc-opt` and `--ld-opt` are the CFLAGS and LDFLAGS of `dpkg-buildflags`.
The hardening flags come from `./configure --hardening=default` (see `auto/cc/hardening`),
including `-D_FORTIFY_SOURCE`, which `dpkg-buildflags` puts only in CPPFLAGS.
The build stops when `build/autoconf.data` has no `-D_FORTIFY_SOURCE` after configure.
Do not add the CPPFLAGS as well: their `-D_FORTIFY_SOURCE=2` makes the level 3 probe fail.
unitd and libunit are not fortified yet: `src/nxt_unix.h` undefines `_FORTIFY_SOURCE`
(https://github.com/freeunitorg/freeunit/issues/773).
The `-specs=` flags are removed. `unitd --version` stores the configure options, and they
would name `/usr/share/dpkg/no-pie-*.specs`. libdpkg-perl owns those files, and the image
purges it with the build packages. A module build that reuses the stored options then fails
with "cannot read spec file". With `-fPIC` and `-pie` set, the specs did nothing.

## CI workflow

The GitHub Actions workflow (`.github/workflows/release-docker.yml`) builds and pushes
multi-arch (`linux/amd64`, `linux/arm64`) images to GHCR (`ghcr.io/freeunitorg/freeunit`)
and Docker Hub (`docker.io/freeunitorg/freeunit`) on every release tag or via
`workflow_dispatch`.

### Rebuilding individual variants without a new FreeUnit release

When an upstream runtime base image (for example `php:8.5-cli-trixie` or
`python:3.14-slim`) publishes a patch release or Debian security update, trigger
`Release Docker Images` via `workflow_dispatch` with:

- `tag`: the FreeUnit release tag to rebuild (e.g. `1.37.1`)
- `variants` *(optional)*: comma-separated list of exact variant names or language
  prefixes (e.g. `php` to rebuild `php-8.3`, `php-8.4`, `php-8.5`, or
  `php-8.5,python-3.14`); leave empty to build all variants in `pkg/eol.json`.

### Tagging and provenance policy

- **Floating tags** (`<version>-<variant>` and `latest-<variant>`, plus legacy
  `latest-<lang><ver>` aliases on GHCR): overwritten on rebuild so deployments
  pinning `1.37.1-php-8.5` automatically receive patched upstream runtimes while
  keeping FreeUnit core pinned to `1.37.1`.
- **Immutable build-date tag** (`<version>-<variant>-<YYYYMMDD>`, e.g.
  `1.37.1-php-8.5-20261028`): published alongside the floating tags so users who
  require an immutable tag reference can pin a specific rebuild date (or pin by
  `@sha256:<digest>`).
- **Temporary per-arch tags** (`<version>-<variant>-amd64`,
  `<version>-<variant>-arm64`): merged into the multi-arch index and deleted in the
  `cleanup` job.
- **OCI annotations and labels**:
  - `org.opencontainers.image.version` — FreeUnit core release version (`1.37.1`)
  - `org.opencontainers.image.revision` — exact git commit SHA of the FreeUnit tag
  - `org.opencontainers.image.base.name` — canonical upstream base image reference
    (e.g. `docker.io/library/php:8.5-cli-trixie`)
  - `org.opencontainers.image.base.digest` — upstream base image `sha256` digest
    pulled at build time (`pull: true`)
  - `org.freeunit.runtime.version` — exact upstream runtime patch version detected
    from the base image (e.g. `8.5.12`)
- **Build provenance and SBOM**: `provenance: mode=max` and `sbom: true` attach
  SLSA provenance and SPDX SBOM attestations to the image index, and
  `SOURCE_DATE_EPOCH` (set to the FreeUnit tag commit timestamp) ensures
  reproducible compilation of `unitd` and modules per PR #626.
