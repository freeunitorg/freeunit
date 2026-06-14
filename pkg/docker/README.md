# FreeUnit Docker images

This directory contains Dockerfiles for all FreeUnit language variants and
a helper script (`build-local.sh`) for building them locally — mirroring
the behavior of `.github/workflows/release-docker.yml`.

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
Dockerfile.builder-trixie   →  ghcr.io/freeunitorg/freeunit-builder:trixie-rust1.94.1
Dockerfile.builder-php8.5   →  ghcr.io/freeunitorg/freeunit-builder:php8.5-rust1.94.1
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
| `java-17` | eclipse-temurin:17-jdk-noble |
| `java-21` | eclipse-temurin:21-jdk-noble |
| `node-20` | node:20 |
| `node-22` | node:22 |
| `node-24` | node:24 |
| `perl-5.38` | perl:5.38 |
| `perl-5.40` | perl:5.40 |
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

## CI workflow

The GitHub Actions workflow (`.github/workflows/release-docker.yml`) builds and pushes
images to GHCR (`ghcr.io/freeunitorg/freeunit`) on every `v*` release tag or
via `workflow_dispatch`. It produces:

- Per-arch tags: `VERSION-VARIANT-amd64`, `VERSION-VARIANT-arm64`
- Multi-arch manifest: `VERSION-VARIANT`, `latest-VARIANT`
