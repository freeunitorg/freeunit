#!/usr/bin/env bash
# build-local.sh — locally builds all (or selected) Docker variants
# from pkg/docker/, mirroring .github/workflows/release-docker.yml
#
# Usage:
#   ./build-local.sh [OPTIONS] [VARIANT...]
#
# Options:
#   -v VERSION   FreeUnit version string to pin (default: git branch name)
#   -p PLATFORM  Target platform (default: current arch)
#                Examples: linux/amd64  linux/arm64  linux/amd64,linux/arm64
#   -j N         Parallel builds (default: nproc/2)
#   -b           Builder mode — use pre-built builder images (local/Dockerfile.*)
#                Skips apt install and Rust download; builder image is built
#                automatically if not found locally or on GHCR.
#                Supported variants: minimal, wasm, php-8.5
#   -n           Dry-run — print commands, do not execute
#   -h           Show this help
#
# apt-cacher-ng (optional):
#   If apt-cacher-ng is running on port 3142, it is detected automatically and
#   used as an APT proxy — subsequent builds skip re-downloading .deb packages.
#   Start it once with:
#     docker run -d --name apt-cacher-ng --restart=always \
#       -p 3142:3142 -v apt-cacher-ng:/var/cache/apt-cacher-ng \
#       sameersbn/apt-cacher-ng
#
# Examples:
#   ./build-local.sh                        # build all variants sequentially
#   ./build-local.sh minimal php-8.5        # build only these two variants
#   ./build-local.sh -j4                    # build 4 at a time
#   ./build-local.sh -v 1.35.2 go-1.25     # pin specific version
#   ./build-local.sh -p linux/amd64,linux/arm64 -j2   # multi-arch (needs buildx)
#   ./build-local.sh -b minimal php-8.5    # fast local build via builder images

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="${SCRIPT_DIR}/build-logs"
# Each parallel Docker build runs make -j$(nproc) internally, so halving avoids
# CPU saturation when multiple builds compile simultaneously.
_NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
PARALLEL=$(( _NCPU / 2 < 1 ? 1 : _NCPU / 2 ))
DRY_RUN=false
PLATFORM=""
APT_PROXY=""
USE_BUILDER=false

# Derive default version from git branch
VERSION="$(git -C "${SCRIPT_DIR}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "local")"
VERSION="${VERSION//\//-}"   # replace / with - (e.g. feat/foo → feat-foo)

# ---------------------------------------------------------------------------
# All variants (matches release-docker.yml matrix)
# ---------------------------------------------------------------------------
ALL_VARIANTS=(
    minimal
    wasm
    go-1.25
    go-1.26
    java-17
    java-21
    node-20
    node-22
    node-24
    perl-5.38
    perl-5.40
    php-8.3
    php-8.4
    php-8.5
    python-3.12
    python-3.12-slim
    python-3.13
    python-3.13-slim
    python-3.14
    python-3.14-slim
    ruby-3.3
    ruby-3.4
)

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
while getopts ":v:p:j:bnh" opt; do
    case $opt in
        v) VERSION="$OPTARG" ;;
        p) PLATFORM="$OPTARG" ;;
        j) PARALLEL="$OPTARG" ;;
        b) USE_BUILDER=true ;;
        n) DRY_RUN=true ;;
        h) usage ;;
        :) err "Option -$OPTARG requires an argument."; exit 1 ;;
       \?) err "Unknown option: -$OPTARG"; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

# Remaining positional args are variant filters
REQUESTED_VARIANTS=("$@")
if [[ ${#REQUESTED_VARIANTS[@]} -eq 0 ]]; then
    VARIANTS=("${ALL_VARIANTS[@]}")
else
    VARIANTS=("${REQUESTED_VARIANTS[@]}")
    # Validate each requested variant
    for v in "${VARIANTS[@]}"; do
        valid=false
        for av in "${ALL_VARIANTS[@]}"; do
            [[ "$av" == "$v" ]] && valid=true && break
        done
        if ! $valid; then
            err "Unknown variant: '$v'. Known variants: ${ALL_VARIANTS[*]}"
            exit 1
        fi
    done
fi

# Detect host platform if none specified
if [[ -z "$PLATFORM" ]]; then
    ARCH="$(uname -m)"
    case "$ARCH" in
        x86_64)  PLATFORM="linux/amd64" ;;
        aarch64) PLATFORM="linux/arm64" ;;
        *)       PLATFORM="linux/${ARCH}" ;;
    esac
fi

# Auto-detect apt-cacher-ng on port 3142 (bash-native, no nc dependency)
# Placed after arg parsing so dry-run mode skips the network probe
if ! $DRY_RUN && (echo > /dev/tcp/127.0.0.1/3142) >/dev/null 2>&1; then
    APT_PROXY="http://host-gateway:3142"
fi

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
if ! command -v docker &>/dev/null; then
    err "docker not found in PATH"; exit 1
fi
if ! docker buildx version &>/dev/null; then
    warn "docker buildx not available — falling back to plain 'docker build'"
    USE_BUILDX=false
else
    USE_BUILDX=true
fi

mkdir -p "${LOG_DIR}"

info "============================================================"
info "FreeUnit local Docker build"
info "  Version  : ${VERSION}"
info "  Platform : ${PLATFORM}"
info "  Parallel : ${PARALLEL}"
info "  Variants : ${VARIANTS[*]}"
info "  Log dir  : ${LOG_DIR}"
info "  Dry-run  : ${DRY_RUN}"
info "  APT proxy: ${APT_PROXY:-none (start apt-cacher-ng on :3142 to enable)}"
info "  Builder  : ${USE_BUILDER} (-b uses local/Dockerfile.* with pre-built Rust)"
info "============================================================"

# ---------------------------------------------------------------------------
# Builder image management
# ---------------------------------------------------------------------------
ensure_builder() {
    local VARIANT="$1"
    # Maps variant → builder image tag and source Dockerfile
    local IMG BDF
    case "$VARIANT" in
        minimal|wasm) IMG="ghcr.io/freeunitorg/freeunit-builder:trixie-rust1.94.1"
                      BDF="${SCRIPT_DIR}/Dockerfile.builder-trixie" ;;
        php-8.5)      IMG="ghcr.io/freeunitorg/freeunit-builder:php8.5-rust1.94.1"
                      BDF="${SCRIPT_DIR}/Dockerfile.builder-php8.5" ;;
        *)            return 0 ;;  # no builder for this variant
    esac

    if docker image inspect "$IMG" &>/dev/null; then
        info "Builder image found locally: ${IMG}"
        return 0
    fi

    if $DRY_RUN; then
        info "DRY-RUN: would pull/build builder image: ${IMG}"
        return 0
    fi

    info "Builder image not found locally: ${IMG}"
    info "Trying to pull from GHCR..."
    if docker pull "$IMG" 2>/dev/null; then
        info "Pulled: ${IMG}"
        return 0
    fi

    info "Pull failed — building from ${BDF}"
    docker build -t "$IMG" -f "$BDF" "${SCRIPT_DIR}" \
        || { err "Failed to build builder image: ${IMG}"; return 1; }
}

# ---------------------------------------------------------------------------
# Build function (runs in subshell for parallelism)
# ---------------------------------------------------------------------------
build_variant() {
    local VARIANT="$1"
    local DOCKERFILE="${SCRIPT_DIR}/Dockerfile.${VARIANT}"

    # Builder mode: use local/Dockerfile.* if available for this variant
    if $USE_BUILDER && [[ -f "${SCRIPT_DIR}/local/Dockerfile.${VARIANT}" ]]; then
        ensure_builder "$VARIANT" || return 1
        DOCKERFILE="${SCRIPT_DIR}/local/Dockerfile.${VARIANT}"
    fi
    local IMAGE_TAG="freeunit:${VERSION}-${VARIANT}"
    local LOG_FILE="${LOG_DIR}/${VARIANT}.log"
    local TMP_DOCKERFILE
    TMP_DOCKERFILE="$(mktemp /tmp/Dockerfile.XXXXXX)"

    {
        echo "[$(date '+%H:%M:%S')] START  ${VARIANT}"

        if [[ ! -f "$DOCKERFILE" ]]; then
            echo "[$(date '+%H:%M:%S')] ERROR  Dockerfile not found: $DOCKERFILE"
            rm -f "$TMP_DOCKERFILE"
            return 1
        fi

        # Pin version (mirrors workflow sed step)
        sed \
            -e "s|-b [0-9][0-9.]*\( https://github.com/freeunitorg/freeunit\)|-b ${VERSION}\1|" \
            -e "s|image.version=\"[^\"]*\"|image.version=\"${VERSION}\"|" \
            "$DOCKERFILE" > "$TMP_DOCKERFILE"

        # Build command
        local CMD
        if $USE_BUILDX; then
            CMD=(docker buildx build
                --platform "${PLATFORM}"
                --file "$TMP_DOCKERFILE"
                --tag "${IMAGE_TAG}"
                --load
                "${SCRIPT_DIR}"
            )
        else
            CMD=(docker build
                --file "$TMP_DOCKERFILE"
                --tag "${IMAGE_TAG}"
                "${SCRIPT_DIR}"
            )
        fi

        if [[ -n "$APT_PROXY" ]]; then
            CMD+=(--add-host=host-gateway:host-gateway
                  --build-arg "http_proxy=${APT_PROXY}"
                  --build-arg "HTTP_PROXY=${APT_PROXY}"
            )
        fi

        echo "[$(date '+%H:%M:%S')] CMD    ${CMD[*]}"

        if $DRY_RUN; then
            echo "[$(date '+%H:%M:%S')] SKIP   dry-run mode"
            rm -f "$TMP_DOCKERFILE"
            return 0
        fi

        local START=$SECONDS
        if "${CMD[@]}" 2>&1; then
            local ELAPSED=$(( SECONDS - START ))
            echo "[$(date '+%H:%M:%S')] OK     ${VARIANT} — ${ELAPSED}s — image: ${IMAGE_TAG}"
        else
            local RC=$?
            echo "[$(date '+%H:%M:%S')] FAIL   ${VARIANT} — exit code ${RC}"
            rm -f "$TMP_DOCKERFILE"
            return $RC
        fi

        rm -f "$TMP_DOCKERFILE"
    } 2>&1 | tee "${LOG_FILE}"
}

export -f build_variant ensure_builder
export VERSION SCRIPT_DIR LOG_DIR USE_BUILDX PLATFORM DRY_RUN APT_PROXY USE_BUILDER

# ---------------------------------------------------------------------------
# Pre-build builder images (sequential, before parallel loop)
# Ensures minimal/wasm (share one builder) and php-8.5 are pulled/built once,
# so parallel build_variant calls hit only the fast docker-inspect path.
# ---------------------------------------------------------------------------
if $USE_BUILDER; then
    declare -A _BUILDER_SEEN
    for _V in "${VARIANTS[@]}"; do
        [[ -f "${SCRIPT_DIR}/local/Dockerfile.${_V}" ]] || continue
        case "$_V" in
            minimal|wasm) _BKEY="trixie" ;;
            php-8.5)      _BKEY="php8.5" ;;
            *)            continue ;;
        esac
        if [[ -z "${_BUILDER_SEEN[$_BKEY]:-}" ]]; then
            _BUILDER_SEEN[$_BKEY]=1
            ensure_builder "$_V" || exit 1
        fi
    done
    unset _BUILDER_SEEN _BKEY _V
fi

# ---------------------------------------------------------------------------
# Run builds
# ---------------------------------------------------------------------------
FAILED=()
SUCCESS=()

if [[ "$PARALLEL" -gt 1 ]] && command -v parallel &>/dev/null; then
    info "Running with GNU parallel (j=${PARALLEL})"
    # GNU parallel — collect exit codes via joblog
    JOBLOG="${LOG_DIR}/parallel.joblog"
    parallel --jobs "${PARALLEL}" --joblog "${JOBLOG}" \
        build_variant ::: "${VARIANTS[@]}" || true
    # Parse joblog for failures
    while IFS=$'\t' read -r seq host starttime runtime send receive exitval signal command _rest; do
        [[ "$seq" == "Seq" ]] && continue   # header line
        variant="${command##* }"             # last word is the variant name
        if [[ "$exitval" -ne 0 ]]; then
            FAILED+=("$variant")
        else
            SUCCESS+=("$variant")
        fi
    done < "${JOBLOG}"
else
    if [[ "$PARALLEL" -gt 1 ]]; then
        warn "GNU parallel not found — falling back to sequential builds"
    fi
    for VARIANT in "${VARIANTS[@]}"; do
        if build_variant "$VARIANT"; then
            SUCCESS+=("$VARIANT")
        else
            FAILED+=("$VARIANT")
            warn "Build failed for ${VARIANT} — continuing with remaining variants"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
info "============================================================"
info "SUMMARY"
info "  Built OK : ${#SUCCESS[@]}  — ${SUCCESS[*]:-none}"
info "  Failed   : ${#FAILED[@]}   — ${FAILED[*]:-none}"
info "  Logs     : ${LOG_DIR}/"
info "============================================================"

if [[ ${#FAILED[@]} -gt 0 ]]; then
    err "Some builds failed: ${FAILED[*]}"
    exit 1
fi
