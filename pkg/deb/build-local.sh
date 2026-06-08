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
#              already-built core deb (pkg/deb/debs/unit_*.deb). Faster.
#   -B         Build only — skip the smoke test.
#   -s         Smoke only — skip the build, test the existing pkg/deb/debs/*.deb.
#   -c         Combined smoke — serve the modules from a single container
#              instead of one isolated container per module. Each unit-php8.x
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
# Requirements: docker, network access (apt + Rust crates for otel, plus the
# sury repo when it is enabled — see -S).
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

# Smoke matrix — matches the build-deb.yml smoke-test matrix exactly.
# Fields: module|kind|type|port|expect
SMOKE_MATRIX=(
    "unit-php8.3|php|php|8083|OK-PHP-8.3"
    "unit-php8.4|php|php|8084|OK-PHP-8.4"
    "unit-php8.5|php|php|8085|OK-PHP-8.5"
    "unit-python3.13|py|python 3.13|8013|OK-PY-3.13"
)

# PHP versions packaged here — drives the sury auto-detect (each needs a
# matching libphpX.Y-embed runtime). Matches the php targets above.
PHP_VERSIONS="8.3 8.4 8.5"

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

# Build target list for the Makefile.
if $MODULES_ONLY; then
    BUILD_TARGETS="unit-php83 unit-php84 unit-php85 unit-python313"
else
    BUILD_TARGETS="unit unit-php83 unit-php84 unit-php85 unit-python313"
fi

info "============================================================"
info "FreeUnit local .deb build"
info "  Repo     : ${REPO_ROOT}"
info "  Version  : ${VERSION}"
info "  Image    : ${IMAGE}"
info "  Build    : ${DO_BUILD} (modules-only: ${MODULES_ONLY}, targets: ${BUILD_TARGETS})"
info "  Smoke    : ${DO_SMOKE} (combined: ${COMBINED_SMOKE})"
info "  Sury     : ${SURY_MODE} (php: ${PHP_VERSIONS})"
info "  Clean    : ${CLEAN}"
info "  Dry-run  : ${DRY_RUN}"
info "============================================================"

# ---------------------------------------------------------------------------
# Container scripts (streamed over stdin; quoted heredocs — no host expansion)
# ---------------------------------------------------------------------------

# The deb.sury.org enablement helper (setup_sury_if_needed) lives in
# pkg/deb/sury-setup.sh and is bind-mounted into every container at
# /sury-setup.sh, then sourced at the top of each script below. Keeping it in one
# file is what stops the CI workflow copy and this one from drifting apart.

# Build script. Consumes env: TARGETS, CLEAN, MODULES_ONLY, SURY, NEED_PHP.
read -r -d '' BUILD_SCRIPT <<'EOS' || true
set -eux
export DEBIAN_FRONTEND=noninteractive
. /sury-setup.sh

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
        rm -f pkg/deb/debs/unit-php8.3* pkg/deb/debs/unit-php8.4* \
              pkg/deb/debs/unit-php8.5* pkg/deb/debs/unit-python3.13*
    else
        rm -rf pkg/deb/debuild pkg/deb/debuild-* pkg/deb/debs \
               pkg/deb/unit pkg/deb/unit-* pkg/deb/check-build-depends-*
    fi
fi

apt-get update
# ca-certificates is needed for the otel Rust crate fetch over https.
apt-get install -y --no-install-recommends ca-certificates
# Enable sury only when the requested PHP runtimes are missing from base apt.
setup_sury_if_needed "${NEED_PHP:-8.3 8.4 8.5}"
# Core needs the Rust toolchain (--otel); modules reuse the common core config
# without it, but installing cargo/rustc unconditionally keeps this one path.
apt-get install -y --no-install-recommends \
    build-essential debhelper devscripts fakeroot lintian \
    libxml2-utils xsltproc pkg-config git \
    libssl-dev libpcre2-dev clang llvm cargo rustc \
    php8.3-dev libphp8.3-embed php8.4-dev libphp8.4-embed \
    php8.5-dev libphp8.5-embed \
    python3.13-dev

echo 'DEBUILD_LINTIAN=no' > "$HOME/.devscripts"
make -C pkg/deb $TARGETS
echo "=== produced debs ==="
ls -la pkg/deb/debs/
EOS

# Isolated smoke script — one module next to the core. Consumes env:
# MODULE, APP_KIND, APP_TYPE, PORT, EXPECT, VERSION.
read -r -d '' SMOKE_ONE_SCRIPT <<'EOS' || true
set -eux
export DEBIAN_FRONTEND=noninteractive
. /sury-setup.sh

# No init system in the container; stop maintainer scripts starting it.
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod +x /usr/sbin/policy-rc.d

apt-get update
# curl drives the control API and health probes; the unit packages do not pull
# it in, and sury setup (its only other installer) is skipped for native php8.4
# and python, so install it explicitly.
apt-get install -y --no-install-recommends curl
# Derive the PHP version this module needs (empty for python -> no sury).
case "$MODULE" in
    unit-php*) NEED_PHP="${MODULE#unit-php}" ;;
    *)         NEED_PHP="" ;;
esac
setup_sury_if_needed "$NEED_PHP"
# core + exactly one module (single PHP version per instance)
apt-get install -y --no-install-recommends /debs/unit_*.deb "/debs/${MODULE}_${VERSION}"*.deb

/usr/sbin/unitd
for _ in $(seq 1 30); do [ -S /var/run/control.unit.sock ] && break; sleep 0.5; done
test -S /var/run/control.unit.sock

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

curl -fsS -X PUT --unix-socket /var/run/control.unit.sock \
    --data-binary "{\"listeners\": {\"*:$PORT\": {\"pass\": \"applications/a\"}}, \"applications\": {\"a\": $app}}" \
    http://localhost/config

out=
for _ in $(seq 1 20); do
    if out=$(curl -fsS "http://localhost:$PORT/" 2>/dev/null) && printf '%s' "$out" | grep -q "$EXPECT"; then
        echo "PASS $MODULE -> $out"
        exit 0
    fi
    sleep 1
done
echo "FAIL $MODULE (expected '$EXPECT'), last response: '${out:-<none>}'"
cat /var/log/unit.log || true
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
. /sury-setup.sh

printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod +x /usr/sbin/policy-rc.d

apt-get update
# curl drives the control API and health probes; install it explicitly since the
# native-php8.4 path skips sury setup (its only other installer).
apt-get install -y --no-install-recommends curl
setup_sury_if_needed "$COMBINED_PHP"
apt-get install -y --no-install-recommends \
    /debs/unit_*.deb \
    "/debs/unit-php${COMBINED_PHP}_"*.deb \
    /debs/unit-python3.13_*.deb

echo "=== installed unit packages ==="
dpkg -l 'unit*' | grep '^ii' || true
ls -la /usr/lib/unit/modules/ || true

/usr/sbin/unitd
for _ in $(seq 1 30); do [ -S /var/run/control.unit.sock ] && break; sleep 0.5; done
test -S /var/run/control.unit.sock

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

curl -fsS -X PUT --unix-socket /var/run/control.unit.sock \
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

check() {
    url=$1; want=$2; out=
    for _ in $(seq 1 20); do
        if out=$(curl -fsS "$url" 2>/dev/null) && printf '%s' "$out" | grep -q "$want"; then
            echo "PASS $url -> $out"
            return 0
        fi
        sleep 1
    done
    echo "FAIL $url (expected '$want'), last response: '${out:-<none>}'"
    cat /var/log/unit.log || true
    return 1
}

check "http://localhost:${php_port}/" "OK-PHP-${COMBINED_PHP}"
check http://localhost:8013/ OK-PY-3.13
echo "ALL SMOKE CHECKS PASSED"
EOS

# ---------------------------------------------------------------------------
# Build phase
# ---------------------------------------------------------------------------
if $DO_BUILD; then
    info "Building .deb packages (${BUILD_TARGETS}) ..."
    if $DRY_RUN; then
        info "DRY-RUN: docker run --rm -v ${REPO_ROOT}:/unit -w /unit \\"
        info "         -v ${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro \\"
        info "         -e TARGETS=\"${BUILD_TARGETS}\" -e CLEAN=${CLEAN} -e MODULES_ONLY=${MODULES_ONLY} \\"
        info "         -e SURY=${SURY_MODE} -e NEED_PHP=\"${PHP_VERSIONS}\" \\"
        info "         ${IMAGE} bash -s   <<< (build script)"
    else
        printf '%s' "$BUILD_SCRIPT" | docker run --rm -i \
            -v "${REPO_ROOT}:/unit" -w /unit \
            -v "${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro" \
            -e TARGETS="${BUILD_TARGETS}" \
            -e CLEAN="${CLEAN}" \
            -e MODULES_ONLY="${MODULES_ONLY}" \
            -e SURY="${SURY_MODE}" \
            -e NEED_PHP="${PHP_VERSIONS}" \
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
        if [[ "$SKIPPED_PHP" != "$COMBINED_PHP" ]]; then
            info "Combined smoke SKIPS php: ${SKIPPED_PHP} (isolated mode covers every version)."
        fi
        if $DRY_RUN; then
            info "DRY-RUN: docker run --rm -v ${DEBS_DIR}:/debs:ro \\"
            info "         -v ${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro \\"
            info "         -e SURY=${SURY_MODE} -e COMBINED_PHP=${COMBINED_PHP} \\"
            info "         ${IMAGE} bash -s  <<< (combined smoke)"
        else
            printf '%s' "$SMOKE_ALL_SCRIPT" | docker run --rm -i \
                -v "${DEBS_DIR}:/debs:ro" \
                -v "${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro" \
                -e SURY="${SURY_MODE}" \
                -e COMBINED_PHP="${COMBINED_PHP}" \
                "${IMAGE}" bash -s
        fi
    else
        FAILED=()
        for row in "${SMOKE_MATRIX[@]}"; do
            IFS='|' read -r module kind type port expect <<< "$row"
            info "Smoke-testing ${module} (isolated) ..."
            if $DRY_RUN; then
                info "DRY-RUN: docker run --rm -v ${DEBS_DIR}:/debs:ro \\"
                info "         -v ${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro \\"
                info "         -e MODULE=${module} -e APP_KIND=${kind} -e APP_TYPE=\"${type}\" \\"
                info "         -e PORT=${port} -e EXPECT=${expect} -e VERSION=${VERSION} \\"
                info "         -e SURY=${SURY_MODE} \\"
                info "         ${IMAGE} bash -s   <<< (isolated smoke)"
                continue
            fi
            if printf '%s' "$SMOKE_ONE_SCRIPT" | docker run --rm -i \
                -v "${DEBS_DIR}:/debs:ro" \
                -v "${REPO_ROOT}/pkg/deb/sury-setup.sh:/sury-setup.sh:ro" \
                -e MODULE="${module}" \
                -e APP_KIND="${kind}" \
                -e APP_TYPE="${type}" \
                -e PORT="${port}" \
                -e EXPECT="${expect}" \
                -e VERSION="${VERSION}" \
                -e SURY="${SURY_MODE}" \
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
