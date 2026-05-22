#!/usr/bin/env bash
# run.sh — builds and runs unit-eol-check inside Docker
# mirrors pkg/docker/template.Dockerfile for build environment
#
# Usage:
#   ./run.sh [OPTIONS] [-- unit-eol-check options]
#
# Options:
#   -n          Dry-run — print commands, do not execute
#   -h          Show this help
#
# Examples:
#   ./run.sh                           # human output, check all
#   ./run.sh --ci                      # CI mode: JSON + exit 1 on errors
#   ./run.sh --runtimes                # runtime versions only
#   ./run.sh --json pkg/eol.json       # custom eol.json path
#   ./run.sh --fix                     # output corrected runtime dates

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
IMAGE_NAME="freeunit-eol-check:latest"
DRY_RUN=false

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log()  { echo "[$(date '+%H:%M:%S')] $*"; }
info() { log "INFO  $*"; }
err()  { log "ERROR $*" >&2; }

usage() {
    sed -n '/^# Usage:/,/^[^#]/{ /^#/s/^# \{0,3\}//p }' "$0"
    exit 0
}

# ---------------------------------------------------------------------------
# Argument parsing — short flags only (-n, -h); all --long args pass through
# ---------------------------------------------------------------------------
while [[ $# -gt 0 && "$1" =~ ^-[nh]$ ]]; do
    case "$1" in
        -n) DRY_RUN=true ;;
        -h) usage ;;
    esac
    shift
done

EOL_ARGS=("$@")

info "============================================================"
info "FreeUnit EOL check"
info "  Image   : ${IMAGE_NAME}"
info "  Dry-run : ${DRY_RUN}"
info "  Args    : ${EOL_ARGS[*]:-none}"
info "============================================================"

# ---------------------------------------------------------------------------
# Build image
# ---------------------------------------------------------------------------
build_image() {
    if docker image inspect "$IMAGE_NAME" &>/dev/null; then
        info "Using cached image: $IMAGE_NAME"
        return 0
    fi

    info "Building image: $IMAGE_NAME"

    local DOCKERFILE="${SCRIPT_DIR}/Dockerfile"
    if [[ ! -f "$DOCKERFILE" ]]; then
        err "Dockerfile not found: $DOCKERFILE"; exit 1
    fi

    if $DRY_RUN; then
        info "Dry-run: would build image"
        return 0
    fi

    docker build \
        --tag "${IMAGE_NAME}" \
        --file "${DOCKERFILE}" \
        "${SCRIPT_DIR}" \
        2>&1 | tee "${SCRIPT_DIR}/build.log"

    info "Image built: $IMAGE_NAME"
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
run_check() {
    if $DRY_RUN; then
        info "Dry-run: would run unit-eol-check with args: ${EOL_ARGS[*]:-none}"
        return 0
    fi

    # Mount repo root as /repo; cargo registry cache avoids full redownload each run
    docker run --rm \
        -v "${PROJECT_DIR}:/repo" \
        -v "${HOME}/.cargo/registry:/root/.cargo/registry" \
        -w /repo \
        "${IMAGE_NAME}" \
        "${EOL_ARGS[@]:---help}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
build_image
run_check