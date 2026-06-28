#!/usr/bin/env bash
# build-local.sh — locally build and smoke-test the Debian trixie .deb packages,
# mirroring .github/workflows/build-deb.yml (build-trixie + smoke-test jobs).
#
# It builds the core + php8.3 / php8.4 / php8.5 / python3.13 packages inside a
# clean debian:trixie container (source mounted at /unit), then smoke-tests each
# module on its own fresh container — installing it next to the core only,
# exactly as the workflow does. The container scripts are streamed over stdin,
# so nothing is written to /tmp and there are no host-specific paths.
#
# Usage:
#   ./build-local.sh [OPTIONS]
#
# Options:
#   -m         Modules-only build — build the language modules and reuse an
#              already-built core deb (pkg/deb/debs/freeunit_*.deb). Faster.
#   -B         Build only — skip the smoke test.
#   -s         Smoke only — skip the build, test the existing pkg/deb/debs/*.deb.
#   -c         Combined smoke — serve the modules from a single container
#              instead of one isolated container per module. Each freeunit-php8.x
#              package bundles its own PHP embed runtime, and running several of
#              them in one instance is unsupported, so combined mode runs exactly
#              one PHP version (the Debian trixie native php8.4 by default, so no
#              sury is needed) alongside python; the other PHP versions are
#              covered only by the default isolated mode, which is what CI does.
#   -k         Keep build state — skip the pre-build clean of generated
#              artifacts (debuild*/debs/symlinks). Useful for incremental runs.
#   -C         Clean only — remove all generated artifacts (debuild*/debs/
#              symlinks/check-build-depends-*) and exit, without building or
#              smoke-testing. Runs the removal inside a container as root, since
#              the build writes those files as root into the mounted tree.
#   -I IMAGE   Base image (default: debian:trixie).
#   -S MODE    deb.sury.org PHP repo: auto (default) | on | off. In auto mode
#              sury is enabled only when a requested libphpX.Y-embed runtime is
#              missing from the base apt sources (Debian trixie main ships one
#              PHP line, so multi-version PHP normally needs it). Use off to
#              build against base sources only, on to force-enable.
#   -n         Dry-run — print the docker commands, do not execute.
#   -h         Show this help.
#
# Examples:
#   ./build-local.sh                 # full build + isolated smoke (mirrors CI)
#   ./build-local.sh -m              # rebuild only the modules, then smoke
#   ./build-local.sh -s              # smoke-test the debs already in debs/
#   ./build-local.sh -B              # build the debs, skip smoke
#   ./build-local.sh -C              # remove all generated artifacts and exit
#   ./build-local.sh -n              # show what would run
#
# Environment (local mirrors — all empty by default => upstream; forwarded into
# the build/smoke containers for reproducible or offline builds):
#   DEB_MIRROR          replacement origin for the Debian CDN (deb.debian.org),
#                       redirecting both the main archive and debian-security.
#                       Full mirror:  http://mirror.lan
#                       apt-cacher-ng: http://cache.lan:3142/deb.debian.org
#   SURY_MIRROR         replacement base for packages.sury.org (php8.3/8.5).
#   RUSTUP_DIST_SERVER  rustup toolchain dist server (default static.rust-lang.org).
#   RUSTUP_UPDATE_ROOT  rustup self-update root.
#   RUSTUP_INIT_URL     rustup installer URL (default https://sh.rustup.rs).
#   CARGO_MIRROR        crates.io registry replacement (e.g. sparse+https://host/index/).
#   Plus BRAND / RUNTIME (package identity) and RUST_TOOLCHAIN / RUSTUP_INIT_SHA256.
#
# Requirements: docker, network access (apt + Rust crates for otel, plus the
# sury repo when it is enabled — see -S), unless a local mirror is configured for
# each source above.
# The build runs as root inside the container and writes generated artifacts
# (debuild*/debs/) into the mounted tree; the default pre-build clean keeps
# repeated runs reproducible.

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel 2>/dev/null)"
REPO_ROOT="${REPO_ROOT:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"

IMAGE="debian:trixie"
DO_BUILD=true
DO_SMOKE=true
MODULES_ONLY=false
COMBINED_SMOKE=false
CLEAN=true
CLEAN_ONLY=false
DRY_RUN=false
SURY_MODE="auto"

# Read the version the Makefile will stamp into the .deb names.
VERSION="$(grep -m1 '^NXT_VERSION=' "${REPO_ROOT}/version" | cut -d= -f2)"
: "${VERSION:?could not determine version from the version file}"

# Brand/runtime identity, mirroring pkg/deb/Makefile defaults. BRAND drives the
# .deb package names (freeunit_*.deb, freeunit-<module>_*.deb); RUNTIME drives
# the on-disk artifacts the smoke tests probe (daemon $RUNTIME"d", the
# control.$RUNTIME.sock socket, /usr/lib/$RUNTIME/modules, /var/log/$RUNTIME.log).
# RUNDIR is the volatile runtime dir holding the control socket and pidfile
# (Makefile's RUNDIR ?= /var/run; set RUNDIR=/run for the pure FHS path). Override
# any of them to build/smoke a differently-branded or relocated set; all three are
# forwarded to make and into the smoke containers.
BRAND="${BRAND:-freeunit}"
RUNTIME="${RUNTIME:-freeunit}"
RUNDIR="${RUNDIR:-/var/run}"

# Rust toolchain for the otel/wasi crate compile. Pinned (not "stable") for
# reproducibility: the merged 1.35.6 otel stack needs rustc >= 1.88 while Debian
# trixie ships 1.85, so rustup installs this exact version. 1.88.0 is the stated
# floor; bump deliberately when a dependency needs newer. Set RUSTUP_INIT_SHA256
# to the known-good sha256 of sh.rustup.rs to verify the installer before it runs
# (empty = skip verification).
RUST_TOOLCHAIN="${RUST_TOOLCHAIN:-1.88.0}"
RUSTUP_INIT_SHA256="${RUSTUP_INIT_SHA256:-}"

# Optional local mirrors for apt + the Rust toolchain/crates, for reproducible
# or offline builds. All empty by default => upstream (current behaviour). Each
# is forwarded into the containers that touch the matching source; see the
# Environment block in the header for URL forms. DEB_MIRROR rewrites the Debian
# CDN (mirror-setup.sh), SURY_MIRROR the sury repo (sury-setup.sh), the RUSTUP_*
# / CARGO_MIRROR knobs the otel Rust build (BUILD_SCRIPT). Normalised here so
# every `-e VAR="${VAR}"` below is safe under `set -u`.
DEB_MIRROR="${DEB_MIRROR:-}"
SURY_MIRROR="${SURY_MIRROR:-}"
RUSTUP_DIST_SERVER="${RUSTUP_DIST_SERVER:-}"
RUSTUP_UPDATE_ROOT="${RUSTUP_UPDATE_ROOT:-}"
RUSTUP_INIT_URL="${RUSTUP_INIT_URL:-https://sh.rustup.rs}"
CARGO_MIRROR="${CARGO_MIRROR:-}"

# PHP versions packaged here. php8.4 ships in the Debian trixie base archive;
# php8.3 and php8.5 come from deb.sury.org, so with sury disabled (-S off) only
# the native line is buildable and the active set narrows to it. PHP_VERSIONS is
# finalised from this pair once -S is known (see below) and is the single source
# the build targets, container dev packages, and smoke matrix all derive from.
PHP_VERSIONS_ALL="8.3 8.4 8.5"
PHP_VERSIONS_NATIVE="8.4"

# PHP version the combined smoke (-c) runs: one instance hosts a single PHP embed
# runtime. Default to the Debian trixie native line (php8.4 — present in base
# apt, no sury round-trip); isolated mode still covers 8.3 and 8.5.
COMBINED_PHP_DEFAULT="8.4"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log()  { echo "[$(date '+%H:%M:%S')] $*"; }
info() { log "INFO  $*"; }
warn() { log "WARN  $*"; }
err()  { log "ERROR $*" >&2; }

# Run a one-shot package-QA container over the produced debs (read-only /debs,
# no source mount). Args: label, script. Honours DRY_RUN. Reads the DEBS_DIR /
# IMAGE / BRAND / RUNTIME / VERSION globals at call time.
run_oneshot() {
    local label="$1" script="$2"
    info "${label} ..."
    if $DRY_RUN; then
        info "DRY-RUN: docker run --rm -v ${DEBS_DIR}:/debs:ro \\"
        info "         -v ${REPO_ROOT}/pkg/deb/mirror-setup.sh:/mirror-setup.sh:ro \\"
        info "         -v ${REPO_ROOT}/pkg/deb/pkg-qa.sh:/pkg-qa.sh:ro \\"
        info "         -e BRAND=${BRAND} -e RUNTIME=${RUNTIME} -e VERSION=${VERSION} \\"
        info "         -e DEB_MIRROR=${DEB_MIRROR} \\"
        info "         ${IMAGE} bash -s   <<< (${label})"
        return 0
    fi
    printf '%s' "$script" | docker run --rm -i \
        -v "${DEBS_DIR}:/debs:ro" \
        -v "${REPO_ROOT}/pkg/deb/mirror-setup.sh:/mirror-setup.sh:ro" \
        -v "${REPO_ROOT}/pkg/deb/pkg-qa.sh:/pkg-qa.sh:ro" \
        -e BRAND="${BRAND}" \
        -e RUNTIME="${RUNTIME}" \
        -e VERSION="${VERSION}" \
        -e DEB_MIRROR="${DEB_MIRROR}" \
        "${IMAGE}" bash -s
}

usage() {
    sed -n '/^# Usage:/,/^[^#]/{ /^#/s/^# \{0,3\}//p }' "$0"
    exit 0
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while getopts ":mBscCkI:S:nh" opt; do
    case $opt in
        m) MODULES_ONLY=true ;;
        B) DO_SMOKE=false ;;
        s) DO_BUILD=false ;;
        c) COMBINED_SMOKE=true ;;
        C) CLEAN_ONLY=true ;;
        k) CLEAN=false ;;
        I) IMAGE="$OPTARG" ;;
        S) SURY_MODE="$OPTARG" ;;
        n) DRY_RUN=true ;;
        h) usage ;;
        :) err "Option -$OPTARG requires an argument."; exit 1 ;;
       \?) err "Unknown option: -$OPTARG"; exit 1 ;;
    esac
done

case "$SURY_MODE" in
    auto|on|off) ;;
    *) err "invalid -S '$SURY_MODE' (expected auto|on|off)"; exit 1 ;;
esac

# With sury disabled only the Debian trixie-native PHP line is buildable, so the
# active set (and everything derived from it — build targets, container dev
# packages, smoke matrix) narrows to it; otherwise the full set is in play.
if [ "$SURY_MODE" = "off" ]; then
    PHP_VERSIONS="$PHP_VERSIONS_NATIVE"
else
    PHP_VERSIONS="$PHP_VERSIONS_ALL"
fi

# Smoke matrix — one row per active PHP version plus python, mirroring the
# build-deb.yml smoke-test matrix. Fields: module|kind|type|port|expect.
# Port is 80 + the version digits (php8.3 -> 8083, php8.4 -> 8084, ...).
SMOKE_MATRIX=()
for v in $PHP_VERSIONS; do
    SMOKE_MATRIX+=("${BRAND}-php${v}|php|php|80${v//./}|OK-PHP-${v}")
done
SMOKE_MATRIX+=("${BRAND}-python3.13|py|python 3.13|8013|OK-PY-3.13")

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
if ! command -v docker &>/dev/null; then
    err "docker not found in PATH"; exit 1
fi

# Clean-only mode: wipe generated artifacts and exit. The build writes these as
# root inside the container, so removal runs the same way (a host-side rm would
# hit permission errors). Mirrors the full pre-build clean.
if $CLEAN_ONLY; then
    read -r -d '' CLEAN_SCRIPT <<'EOS' || true
set -eu
git config --global --add safe.directory /unit 2>/dev/null || true
rm -rf pkg/deb/debuild pkg/deb/debuild-* pkg/deb/debs \
       pkg/deb/unit pkg/deb/unit-* pkg/deb/check-build-depends-*
echo "cleaned: pkg/deb/{debuild*,debs,unit,unit-*,check-build-depends-*}"
EOS
    info "Cleaning generated artifacts in ${REPO_ROOT}/pkg/deb ..."
    if $DRY_RUN; then
        info "DRY-RUN: docker run --rm -v ${REPO_ROOT}:/unit -w /unit ${IMAGE} bash -s  <<< (clean script)"
    else
        printf '%s' "$CLEAN_SCRIPT" | docker run --rm -i \
            -v "${REPO_ROOT}:/unit" -w /unit "${IMAGE}" bash -s
    fi
    info "Clean-only done."
    exit 0
fi

if [[ -z "$VERSION" ]]; then
    err "could not read NXT_VERSION from ${REPO_ROOT}/version"; exit 1
fi
if ! $DO_BUILD && ! $DO_SMOKE; then
    err "-B and -s together do nothing"; exit 1
fi

# Build target list for the Makefile, derived from the active PHP set so a
# narrowed PHP_VERSIONS (e.g. -S off) never asks make for an unavailable module.
php_targets=""
for v in $PHP_VERSIONS; do
    php_targets+=" unit-php${v//./}"
done
if $MODULES_ONLY; then
    BUILD_TARGETS="${php_targets# } unit-python313"
else
    BUILD_TARGETS="unit${php_targets} unit-python313"
fi

info "============================================================"
info "FreeUnit local .deb build"
info "  Repo     : ${REPO_ROOT}"
info "  Version  : ${VERSION}"
info "  Image    : ${IMAGE}"
info "  Build    : ${DO_BUILD} (modules-only: ${MODULES_ONLY}, targets: ${BUILD_TARGETS})"
info "  Smoke    : ${DO_SMOKE} (combined: ${COMBINED_SMOKE})"
info "  Sury     : ${SURY_MODE} (php: ${PHP_VERSIONS})"
info "  Mirrors  : deb=${DEB_MIRROR:-<upstream>} sury=${SURY_MIRROR:-<upstream>} cargo=${CARGO_MIRROR:-<upstream>} rustup-dist=${RUSTUP_DIST_SERVER:-<upstream>}"
info "  Clean    : ${CLEAN}"
info "  Dry-run  : ${DRY_RUN}"
info "============================================================"

# ---------------------------------------------------------------------------
# Container scripts (streamed over stdin; quoted heredocs — no host expansion)
# ---------------------------------------------------------------------------

# The deb.sury.org enablement helper (setup_sury_if_needed) lives in
# pkg/deb/sury-setup.sh and is bind-mounted into every container at
# /sury-setup.sh, then sourced at the top of each script below. Keeping it in
# one file prevents the local smoke paths from drifting apart; the CI workflow
# sources it directly from the checked-out tree.

# Build script. Consumes env: TARGETS, CLEAN, MODULES_ONLY, SURY, NEED_PHP.
read -r -d '' BUILD_SCRIPT <<'EOS' || true
set -eux
export DEBIAN_FRONTEND=noninteractive
. /mirror-setup.sh
. /sury-setup.sh

# Redirect the Debian archive to DEB_MIRROR (no-op when unset) before any apt
# call, so the whole build resolves from the local mirror.
apply_deb_mirror

# git may run against the host-owned tree under a root container.
git config --global --add safe.directory /unit 2>/dev/null || true

if [ "$CLEAN" = "true" ]; then
    if [ "$MODULES_ONLY" = "true" ]; then
        rm -rf pkg/deb/debuild-* \
               pkg/deb/unit-php83 pkg/deb/unit-php84 pkg/deb/unit-php85 \
               pkg/deb/unit-python313 \
               pkg/deb/check-build-depends-php83 \
               pkg/deb/check-build-depends-php84 \
               pkg/deb/check-build-depends-php85 \
               pkg/deb/check-build-depends-python313
        rm -f pkg/deb/debs/${BRAND}-php8.3* pkg/deb/debs/${BRAND}-php8.4* \
              pkg/deb/debs/${BRAND}-php8.5* pkg/deb/debs/${BRAND}-python3.13*
    else
        rm -rf pkg/deb/debuild pkg/deb/debuild-* pkg/deb/debs \
               pkg/deb/unit pkg/deb/unit-* pkg/deb/check-build-depends-*
    fi
fi

apt_retry apt-get update
# ca-certificates + curl: curl is the pkg/contrib downloader for the njs,
# wasmtime and wasi-sysroot tarballs (it is tried before wget, then fetch); both
# are also needed for the otel Rust crate fetch over https. curl must not depend
# on sury being enabled (sury-off skips its setup), so install it here.
apt_retry apt-get install -y --no-install-recommends ca-certificates curl
# Enable sury only when the requested PHP runtimes are missing from base apt.
# NEED_PHP is always passed by the host; the fallback matches the PHP_PKGS one.
setup_sury_if_needed "${NEED_PHP:-8.4}"
# PHP dev/runtime packages for exactly the requested versions, so a narrowed set
# (sury off) never asks apt for an unavailable php line.
PHP_PKGS=""
for v in ${NEED_PHP:-8.4}; do
    PHP_PKGS="$PHP_PKGS php${v}-dev libphp${v}-embed"
done
# Core needs the Rust toolchain (--otel); modules reuse the common core config
# without it, but installing cargo/rustc unconditionally keeps this one path.
# lsb-release is a real build dep (pkg/deb/Makefile + debian/rules.in derive the
# CODENAME via `lsb_release -cs`); it must not depend on sury being enabled.
apt_retry apt-get install -y --no-install-recommends \
    build-essential debhelper devscripts fakeroot lintian lsb-release \
    libxml2-utils xsltproc pkg-config git \
    libssl-dev libpcre2-dev clang llvm cargo rustc \
    $PHP_PKGS \
    python3.13-dev

echo 'DEBUILD_LINTIAN=no' > "$HOME/.devscripts"

# The merged 1.35.6 otel stack (opentelemetry 0.32 -> tonic 0.14 / icu 2.2)
# needs rustc >= 1.88, but Debian trixie's apt rust is 1.85. Install a current
# stable toolchain via rustup and prepend it to PATH so it shadows the apt rustc
# during the build; the apt cargo/rustc above stay only to satisfy the dpkg
# build-dep checks (debian/control Build-Depends, check-build-depends-unit).
# debuild preserves PATH and RUSTUP_HOME (see pkg/deb/Makefile), so the newer
# toolchain reaches the otel crate compile.
export RUSTUP_HOME=/root/.rustup
export CARGO_HOME=/root/.cargo
# Optional local mirrors for the Rust toolchain + crates (offline/reproducible
# builds). rustup honors RUSTUP_DIST_SERVER / RUSTUP_UPDATE_ROOT natively; export
# them only when set so an empty value never shadows the upstream default (a bare
# `[ -n x ] && export` would also trip set -e on the empty case). The crates.io
# replacement is written to $CARGO_HOME/config.toml; debuild keeps HOME=/root, so
# the otel crate compile under `make` reads it via the default $HOME/.cargo even
# though CARGO_HOME itself is not preserved (pkg/deb/Makefile forwards only PATH
# + RUSTUP_HOME).
if [ -n "${RUSTUP_DIST_SERVER:-}" ]; then export RUSTUP_DIST_SERVER; fi
if [ -n "${RUSTUP_UPDATE_ROOT:-}" ]; then export RUSTUP_UPDATE_ROOT; fi
if [ -n "${CARGO_MIRROR:-}" ]; then
    mkdir -p "$CARGO_HOME"
    cat > "$CARGO_HOME/config.toml" <<CARGOCFG
[source.crates-io]
replace-with = "mirror"

[source.mirror]
registry = "${CARGO_MIRROR}"
CARGOCFG
    echo "cargo: crates.io -> ${CARGO_MIRROR}"
fi
# Fetch the installer to a file (not a blind curl|sh), optionally verify its
# checksum, then install the pinned toolchain. The default upstream fetch is
# pinned to =https; a custom RUSTUP_INIT_URL (e.g. an http local mirror)
# is taken as-is since the operator vouches for it, and RUSTUP_INIT_SHA256 below
# still guards integrity over any transport.
rustup_init="$(mktemp)"
trap 'rm -f "$rustup_init"' EXIT
if [ "${RUSTUP_INIT_URL:-https://sh.rustup.rs}" = "https://sh.rustup.rs" ]; then
    curl --proto '=https' -sSf https://sh.rustup.rs -o "$rustup_init"
else
    curl -fsSL "${RUSTUP_INIT_URL}" -o "$rustup_init"
fi
if [ -n "${RUSTUP_INIT_SHA256:-}" ]; then
    echo "${RUSTUP_INIT_SHA256}  ${rustup_init}" | sha256sum -c -
fi
sh "$rustup_init" -y --default-toolchain "${RUST_TOOLCHAIN}" --profile minimal
rm -f "$rustup_init"
export PATH="$CARGO_HOME/bin:$PATH"
rustc --version

make -C pkg/deb BRAND="$BRAND" RUNTIME="$RUNTIME" RUNDIR="$RUNDIR" $TARGETS
echo "=== produced debs ==="
ls -la pkg/deb/debs/
EOS

# Isolated smoke script — one module next to the core. Consumes env:
# MODULE, APP_KIND, APP_TYPE, PORT, EXPECT, VERSION.
read -r -d '' SMOKE_ONE_SCRIPT <<'EOS' || true
set -eux
export DEBIAN_FRONTEND=noninteractive
. /mirror-setup.sh
. /sury-setup.sh
. /smoke-asserts.sh

# No init system in the container; stop maintainer scripts starting it.
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod +x /usr/sbin/policy-rc.d

apply_deb_mirror
apt_retry apt-get update
# curl drives the control API and health probes; the unit packages do not pull
# it in, and sury setup (its only other installer) is skipped for native php8.4
# and python, so install it explicitly.
apt_retry apt-get install -y --no-install-recommends curl
# Derive the PHP version this module needs (empty for python -> no sury).
case "$MODULE" in
    ${BRAND}-php*) NEED_PHP="${MODULE#${BRAND}-php}" ;;
    *)             NEED_PHP="" ;;
esac
setup_sury_if_needed "$NEED_PHP"
# core + exactly one module (single PHP version per instance)
apt_retry apt-get install -y --no-install-recommends /debs/${BRAND}_*.deb "/debs/${MODULE}_${VERSION}"*.deb

# Packaging/rebrand assertions on the freshly installed set (shared with the
# combined path); fatal on a broken rebrand, before any request is served.
run_smoke_asserts

/usr/sbin/${RUNTIME}d
for _ in $(seq 1 30); do [ -S "${RUNDIR}/control.${RUNTIME}.sock" ] && break; sleep 0.5; done
test -S "${RUNDIR}/control.${RUNTIME}.sock"

mkdir -p /tmp/app
if [ "$APP_KIND" = php ]; then
    printf '<?php echo "OK-PHP-".PHP_VERSION;\n' > /tmp/app/index.php
    app="{\"type\": \"$APP_TYPE\", \"root\": \"/tmp/app\", \"script\": \"index.php\"}"
else
    cat > /tmp/app/wsgi.py <<'PY'
import sys


def application(environ, start_response):
    start_response("200 OK", [("Content-Type", "text/plain")])
    body = "OK-PY-%d.%d" % (sys.version_info[0], sys.version_info[1])
    return [body.encode()]
PY
    app="{\"type\": \"$APP_TYPE\", \"path\": \"/tmp/app\", \"module\": \"wsgi\"}"
fi
chmod -R a+rX /tmp/app

curl -fsS -X PUT --unix-socket "${RUNDIR}/control.${RUNTIME}.sock" \
    --data-binary "{\"listeners\": {\"*:$PORT\": {\"pass\": \"applications/a\"}}, \"applications\": {\"a\": $app}}" \
    http://localhost/config

# Round-trip: the controller must echo back the listener we applied (catches a
# config that is accepted then silently dropped).
assert_listener_echoed "\*:$PORT"

out=
for _ in $(seq 1 20); do
    if out=$(curl -fsS "http://localhost:$PORT/" 2>/dev/null) && printf '%s' "$out" | grep -q "$EXPECT"; then
        # The router itself must answer: a Server: header carrying the upstream
        # NXT_NAME ("Unit") confirms our daemon served the response.
        assert_server_header "http://localhost:$PORT/" "$MODULE"
        echo "PASS $MODULE -> $out (config round-trip + Server header OK)"
        assert_clean_shutdown
        exit 0
    fi
    sleep 1
done
echo "FAIL $MODULE (expected '$EXPECT'), last response: '${out:-<none>}'"
cat /var/log/${RUNTIME}.log || true
exit 1
EOS

# Combined smoke script — core + one PHP version + python in one container.
# The PHP embed runtimes are not validated side by side, so this installs only
# the single PHP version named by $COMBINED_PHP (the caller picks the trixie-
# native one); isolated mode is what covers every PHP version. Consumes env:
# SURY, COMBINED_PHP.
read -r -d '' SMOKE_ALL_SCRIPT <<'EOS' || true
set -eux
export DEBIAN_FRONTEND=noninteractive
. /mirror-setup.sh
. /sury-setup.sh
. /smoke-asserts.sh

printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod +x /usr/sbin/policy-rc.d

apply_deb_mirror
apt_retry apt-get update
# curl drives the control API and health probes; install it explicitly since the
# native-php8.4 path skips sury setup (its only other installer).
apt_retry apt-get install -y --no-install-recommends curl
setup_sury_if_needed "$COMBINED_PHP"
apt_retry apt-get install -y --no-install-recommends \
    /debs/${BRAND}_*.deb \
    "/debs/${BRAND}-php${COMBINED_PHP}_"*.deb \
    /debs/${BRAND}-python3.13_*.deb

echo "=== installed ${BRAND} packages ==="
dpkg -l "${BRAND}*" | grep '^ii' || true
ls -la /usr/lib/${RUNTIME}/modules/ || true

# Packaging/rebrand assertions on the freshly installed set (shared with the
# isolated path). Core checks are fatal; SysV /etc/default wiring is reported
# but non-fatal since debhelper's handling can vary by version.
run_smoke_asserts

/usr/sbin/${RUNTIME}d
for _ in $(seq 1 30); do [ -S "${RUNDIR}/control.${RUNTIME}.sock" ] && break; sleep 0.5; done
test -S "${RUNDIR}/control.${RUNTIME}.sock"

# php8.3 -> 8083, php8.4 -> 8084, php8.5 -> 8085 (matches the isolated matrix).
php_port="80${COMBINED_PHP//./}"
mkdir -p /tmp/php /tmp/py313
printf '<?php echo "OK-PHP-".PHP_VERSION;\n' > /tmp/php/index.php
cat > /tmp/py313/wsgi.py <<'PY'
import sys


def application(environ, start_response):
    start_response("200 OK", [("Content-Type", "text/plain")])
    body = "OK-PY-%d.%d" % (sys.version_info[0], sys.version_info[1])
    return [body.encode()]
PY
chmod -R a+rX /tmp/php /tmp/py313

curl -fsS -X PUT --unix-socket "${RUNDIR}/control.${RUNTIME}.sock" \
    --data-binary "{
      \"listeners\": {
        \"*:${php_port}\": {\"pass\": \"applications/php\"},
        \"*:8013\": {\"pass\": \"applications/py313\"}
      },
      \"applications\": {
        \"php\": {\"type\": \"php\", \"root\": \"/tmp/php\", \"script\": \"index.php\"},
        \"py313\": {\"type\": \"python 3.13\", \"path\": \"/tmp/py313\", \"module\": \"wsgi\"}
      }
    }" http://localhost/config

# Round-trip: the controller must echo back both listeners we applied.
for want_listener in "\*:${php_port}" "\*:8013"; do
    assert_listener_echoed "$want_listener"
done

check() {
    url=$1; want=$2; out=
    for _ in $(seq 1 20); do
        if out=$(curl -fsS "$url" 2>/dev/null) && printf '%s' "$out" | grep -q "$want"; then
            # The router must answer with a Server: header (upstream NXT_NAME
            # "Unit") — confirms our daemon, not something else, served it.
            assert_server_header "$url"
            echo "PASS $url -> $out (Server header OK)"
            return 0
        fi
        sleep 1
    done
    echo "FAIL $url (expected '$want'), last response: '${out:-<none>}'"
    cat /var/log/${RUNTIME}.log || true
    return 1
}

check "http://localhost:${php_port}/" "OK-PHP-${COMBINED_PHP}"
check http://localhost:8013/ OK-PY-3.13
assert_clean_shutdown
echo "ALL SMOKE CHECKS PASSED"
EOS

# Package-QA script — runs once over the produced .debs (no install). Asserts
# the drop-in-replacement control contract and runs lintian. Consumes env:
# BRAND, VERSION. lintian is installed here on demand because the smoke image is
# a plain debian:trixie (the build image has it, but -s reuses existing debs).
# Thin wrapper: apply any local mirror, then delegate to the shared gate in
# pkg-qa.sh (control fields + -dev pkg-config residual scan + lintian). The gate
# logic is shared verbatim with the build-deb.yml CI workflow.
read -r -d '' PKG_QA_SCRIPT <<'EOS' || true
set -eux
export DEBIAN_FRONTEND=noninteractive
. /mirror-setup.sh
apply_deb_mirror
. /pkg-qa.sh
pkg_qa_control_lintian
EOS

# Thin wrapper: apply any local mirror, block service starts (no init system in
# the container), update apt, then delegate to the shared lifecycle gate in
# pkg-qa.sh (install/remove/purge/reinstall + ExecStart assertion + systemd
# verify). Shared verbatim with the build-deb.yml CI workflow.
read -r -d '' PKG_LIFECYCLE_SCRIPT <<'EOS' || true
set -eux
export DEBIAN_FRONTEND=noninteractive
. /mirror-setup.sh
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod +x /usr/sbin/policy-rc.d
apply_deb_mirror
apt-get update
. /pkg-qa.sh
pkg_lifecycle
EOS

# Thin wrapper: apply any local mirror, block service starts, update apt, then
# delegate to the shared drop-in-upgrade gate in pkg-qa.sh (synthesize a stand-in
# upstream "unit" .deb and prove the Conflicts/Replaces takeover supersedes both
# its binary and its systemd unit). Shared verbatim with the build-deb.yml CI
# workflow.
read -r -d '' PKG_UPGRADE_SCRIPT <<'EOS' || true
set -eux
export DEBIAN_FRONTEND=noninteractive
. /mirror-setup.sh
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod +x /usr/sbin/policy-rc.d
apply_deb_mirror
apt-get update
. /pkg-qa.sh
pkg_dropin_upgrade
EOS

# ---------------------------------------------------------------------------
# Build phase
# ---------------------------------------------------------------------------
if $DO_BUILD; then
    info "Building .deb packages (${BUILD_TARGETS}) ..."
    if $DRY_RUN; then
        info "DRY-RUN: docker run --rm -v ${REPO_ROOT}:/unit -w /unit \\"
        info "         -v ${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro \\"
        info "         -v ${REPO_ROOT}/pkg/deb/mirror-setup.sh:/mirror-setup.sh:ro \\"
        info "         -e TARGETS=\"${BUILD_TARGETS}\" -e CLEAN=${CLEAN} -e MODULES_ONLY=${MODULES_ONLY} \\"
        info "         -e SURY=${SURY_MODE} -e NEED_PHP=\"${PHP_VERSIONS}\" -e RUNDIR=${RUNDIR} \\"
        info "         -e DEB_MIRROR=${DEB_MIRROR} -e SURY_MIRROR=${SURY_MIRROR} \\"
        info "         -e RUSTUP_DIST_SERVER=${RUSTUP_DIST_SERVER} -e RUSTUP_UPDATE_ROOT=${RUSTUP_UPDATE_ROOT} -e RUSTUP_INIT_URL=${RUSTUP_INIT_URL} -e CARGO_MIRROR=${CARGO_MIRROR} \\"
        info "         ${IMAGE} bash -s   <<< (build script)"
    else
        printf '%s' "$BUILD_SCRIPT" | docker run --rm -i \
            -v "${REPO_ROOT}:/unit" -w /unit \
            -v "${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro" \
            -v "${REPO_ROOT}/pkg/deb/mirror-setup.sh:/mirror-setup.sh:ro" \
            -e TARGETS="${BUILD_TARGETS}" \
            -e CLEAN="${CLEAN}" \
            -e MODULES_ONLY="${MODULES_ONLY}" \
            -e SURY="${SURY_MODE}" \
            -e NEED_PHP="${PHP_VERSIONS}" \
            -e BRAND="${BRAND}" \
            -e RUNTIME="${RUNTIME}" \
            -e RUNDIR="${RUNDIR}" \
            -e RUST_TOOLCHAIN="${RUST_TOOLCHAIN}" \
            -e RUSTUP_INIT_SHA256="${RUSTUP_INIT_SHA256}" \
            -e DEB_MIRROR="${DEB_MIRROR}" \
            -e SURY_MIRROR="${SURY_MIRROR}" \
            -e RUSTUP_DIST_SERVER="${RUSTUP_DIST_SERVER}" \
            -e RUSTUP_UPDATE_ROOT="${RUSTUP_UPDATE_ROOT}" \
            -e RUSTUP_INIT_URL="${RUSTUP_INIT_URL}" \
            -e CARGO_MIRROR="${CARGO_MIRROR}" \
            "${IMAGE}" bash -s
    fi
    info "Build phase done — debs in ${REPO_ROOT}/pkg/deb/debs/"
fi

# ---------------------------------------------------------------------------
# Smoke phase
# ---------------------------------------------------------------------------
if $DO_SMOKE; then
    DEBS_DIR="${REPO_ROOT}/pkg/deb/debs"
    if ! $DRY_RUN && [[ ! -d "$DEBS_DIR" ]]; then
        err "no debs directory at ${DEBS_DIR} — build first (drop -s)"; exit 1
    fi

    # Package QA over the produced debs (once, before serving requests):
    # control-field contract + lintian, the full install/remove/purge/reinstall
    # lifecycle + systemd unit verification, and the drop-in upgrade over the
    # upstream "unit" package.
    run_oneshot "Package QA: control fields + lintian" "$PKG_QA_SCRIPT"
    run_oneshot "Package lifecycle: remove/purge/reinstall + systemd verify" "$PKG_LIFECYCLE_SCRIPT"
    run_oneshot "Drop-in upgrade over upstream unit" "$PKG_UPGRADE_SCRIPT"

    if $COMBINED_SMOKE; then
        # One container can host only one PHP embed runtime, so combined mode
        # tests a single PHP (the trixie-native php8.4 by default, no sury) plus
        # python; isolated mode covers every version.
        COMBINED_PHP="$COMBINED_PHP_DEFAULT"
        SKIPPED_PHP=""
        for v in $PHP_VERSIONS; do
            [[ "$v" == "$COMBINED_PHP" ]] || SKIPPED_PHP+="${SKIPPED_PHP:+ }$v"
        done
        info "Combined smoke: core + php${COMBINED_PHP} + python3.13 in one container ..."
        if [[ -n "$SKIPPED_PHP" ]]; then
            info "Combined smoke SKIPS php: ${SKIPPED_PHP} (isolated mode covers every version)."
        fi
        if $DRY_RUN; then
            info "DRY-RUN: docker run --rm -v ${DEBS_DIR}:/debs:ro \\"
            info "         -v ${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro \\"
            info "         -v ${REPO_ROOT}/pkg/deb/mirror-setup.sh:/mirror-setup.sh:ro \\"
            info "         -v ${REPO_ROOT}/pkg/deb/smoke-asserts.sh:/smoke-asserts.sh:ro \\"
            info "         -e SURY=${SURY_MODE} -e COMBINED_PHP=${COMBINED_PHP} -e VERSION=${VERSION} -e RUNDIR=${RUNDIR} \\"
            info "         -e DEB_MIRROR=${DEB_MIRROR} -e SURY_MIRROR=${SURY_MIRROR} \\"
            info "         ${IMAGE} bash -s  <<< (combined smoke)"
        else
            printf '%s' "$SMOKE_ALL_SCRIPT" | docker run --rm -i \
                -v "${DEBS_DIR}:/debs:ro" \
                -v "${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro" \
                -v "${REPO_ROOT}/pkg/deb/mirror-setup.sh:/mirror-setup.sh:ro" \
                -v "${REPO_ROOT}/pkg/deb/smoke-asserts.sh:/smoke-asserts.sh:ro" \
                -e SURY="${SURY_MODE}" \
                -e COMBINED_PHP="${COMBINED_PHP}" \
                -e VERSION="${VERSION}" \
                -e BRAND="${BRAND}" \
                -e RUNTIME="${RUNTIME}" \
                -e RUNDIR="${RUNDIR}" \
                -e DEB_MIRROR="${DEB_MIRROR}" \
                -e SURY_MIRROR="${SURY_MIRROR}" \
                "${IMAGE}" bash -s
        fi
    else
        FAILED=()
        for row in "${SMOKE_MATRIX[@]}"; do
            IFS='|' read -r module kind type port expect <<< "$row"
            # Second guard (the matrix already tracks the active PHP set): skip a
            # module whose .deb is absent, e.g. a partial -m/-s run.
            if ! $DRY_RUN && ! ls "${DEBS_DIR}/${module}_${VERSION}"*.deb >/dev/null 2>&1; then
                warn "Skipping ${module}: no ${module}_${VERSION}*.deb in ${DEBS_DIR}"
                continue
            fi
            info "Smoke-testing ${module} (isolated) ..."
            if $DRY_RUN; then
                info "DRY-RUN: docker run --rm -v ${DEBS_DIR}:/debs:ro \\"
                info "         -v ${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro \\"
                info "         -v ${REPO_ROOT}/pkg/deb/mirror-setup.sh:/mirror-setup.sh:ro \\"
                info "         -v ${REPO_ROOT}/pkg/deb/smoke-asserts.sh:/smoke-asserts.sh:ro \\"
                info "         -e MODULE=${module} -e APP_KIND=${kind} -e APP_TYPE=\"${type}\" \\"
                info "         -e PORT=${port} -e EXPECT=${expect} -e VERSION=${VERSION} -e RUNDIR=${RUNDIR} \\"
                info "         -e SURY=${SURY_MODE} -e DEB_MIRROR=${DEB_MIRROR} -e SURY_MIRROR=${SURY_MIRROR} \\"
                info "         ${IMAGE} bash -s   <<< (isolated smoke)"
                continue
            fi
            if printf '%s' "$SMOKE_ONE_SCRIPT" | docker run --rm -i \
                -v "${DEBS_DIR}:/debs:ro" \
                -v "${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro" \
                -v "${REPO_ROOT}/pkg/deb/mirror-setup.sh:/mirror-setup.sh:ro" \
                -v "${REPO_ROOT}/pkg/deb/smoke-asserts.sh:/smoke-asserts.sh:ro" \
                -e MODULE="${module}" \
                -e APP_KIND="${kind}" \
                -e APP_TYPE="${type}" \
                -e PORT="${port}" \
                -e EXPECT="${expect}" \
                -e VERSION="${VERSION}" \
                -e SURY="${SURY_MODE}" \
                -e BRAND="${BRAND}" \
                -e RUNTIME="${RUNTIME}" \
                -e RUNDIR="${RUNDIR}" \
                -e DEB_MIRROR="${DEB_MIRROR}" \
                -e SURY_MIRROR="${SURY_MIRROR}" \
                "${IMAGE}" bash -s; then
                info "${module}: PASS"
            else
                err "${module}: FAIL"
                FAILED+=("${module}")
            fi
        done
        if ! $DRY_RUN && [[ ${#FAILED[@]} -gt 0 ]]; then
            err "smoke failures: ${FAILED[*]}"; exit 1
        fi
    fi
    info "Smoke phase done."
fi

info "All requested phases completed."
