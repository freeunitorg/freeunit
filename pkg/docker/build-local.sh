#!/usr/bin/env bash
# build-local.sh — locally builds all (or selected) Docker variants
# from pkg/docker/, mirroring .github/workflows/docker.yml
#
# Usage:
#   ./build-local.sh [OPTIONS] [VARIANT...]
#
# Options:
#   -v VERSION   FreeUnit version string to pin (default: git branch name)
#   -p PLATFORM  Target platform (default: current arch)
#                Examples: linux/amd64  linux/arm64  linux/amd64,linux/arm64
#   -j N         Parallel builds (default: 1)
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
#   ./build-local.sh minimal php8.5         # build only these two variants
#   ./build-local.sh -j4                    # build 4 at a time
#   ./build-local.sh -v 1.35.2 go1.25      # pin specific version
#   ./build-local.sh -p linux/amd64,linux/arm64 -j2   # multi-arch (needs buildx)

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="${SCRIPT_DIR}/build-logs"
PARALLEL=1
DRY_RUN=false
PLATFORM=""
APT_PROXY=""

# Auto-detect apt-cacher-ng on port 3142
if nc -z 127.0.0.1 3142 2>/dev/null; then
    APT_PROXY="http://host-gateway:3142"
fi

# Derive default version from git branch
VERSION="$(git -C "${SCRIPT_DIR}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "local")"
VERSION="${VERSION//\//-}"   # replace / with - (e.g. feat/foo → feat-foo)

# ---------------------------------------------------------------------------
# All variants (matches docker.yml matrix)
# ---------------------------------------------------------------------------
ALL_VARIANTS=(
    minimal
    wasm
    go1.24
    go1.25
    go1.26
    jsc17
    jsc21
    node20
    node22
    node24
    perl5.38
    perl5.40
    php8.3
    php8.4
    php8.5
    python3.12
    python3.12-slim
    python3.13
    python3.13-slim
    python3.14
    python3.14-slim
    ruby3.3
    ruby3.4
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
while getopts ":v:p:j:nh" opt; do
    case $opt in
        v) VERSION="$OPTARG" ;;
        p) PLATFORM="$OPTARG" ;;
        j) PARALLEL="$OPTARG" ;;
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
info "============================================================"

# ---------------------------------------------------------------------------
# Build function (runs in subshell for parallelism)
# ---------------------------------------------------------------------------
build_variant() {
    local VARIANT="$1"
    local DOCKERFILE="${SCRIPT_DIR}/Dockerfile.${VARIANT}"
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

export -f build_variant
export VERSION SCRIPT_DIR LOG_DIR USE_BUILDX PLATFORM DRY_RUN APT_PROXY

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
