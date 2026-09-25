#!/usr/bin/env bash
# run-local-full.sh — pre-commit / PR static analysis via clang-ast
#
# Runs the FreeUnit C build under the freeunitorg/clang-ast LLVM plugin
# inside Docker.  Catches API-misuse / lifetime / allocator violations the
# normal compile does not.  Intended to be run BEFORE every commit and PR.
#
# Scope: C core + otel — configure is `--otel --openssl --debug`, so the
# OpenTelemetry C glue (nxt_otel.c, the otel validators in
# nxt_conf_validation.c) is analyzed too.  The Rust otel library is built by
# cargo for linking but is not seen by the clang plugin.  Other module C code
# (njs, brotli, zlib, zstd) is still NOT analyzed; use ./test/run-local.sh for
# the full feature set.
#
# Usage:
#   ./test/run-local-full.sh           # build + clang-ast check
#   ./test/run-local-full.sh -n        # dry-run (print, don't execute)
#
# To force image rebuild: docker rmi freeunit-test-full:local

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="freeunit-test-full:local"
# Pre-built image (clang-ast plugin + rustc/cargo).  Pulled when present so the
# slow one-time apt+rust install is skipped; private — needs `docker login
# ghcr.io`.  Falls back to a local build when the pull fails.
REMOTE_IMAGE="ghcr.io/freeunitorg/freeunit-clang-ast:trixie"
DRY_RUN=false
TMP_DIR=""

log()  { echo "[$(date '+%H:%M:%S')] $*"; }
info() { log "INFO  $*"; }
err()  { log "ERROR $*" >&2; }

case "${1:-}" in
    -n) DRY_RUN=true ;;
    -h|--help)
        sed -n '/^# Usage:/,/^[^#]/{ /^#/s/^# \{0,3\}//p }' "$0"
        exit 0
        ;;
    "") ;;
    *)  err "Unknown option: $1"; exit 1 ;;
esac

if ! command -v docker &>/dev/null; then
    err "docker not found in PATH"; exit 1
fi

# ---------------------------------------------------------------------------
# Tmp copy — isolate source from live tree
# ---------------------------------------------------------------------------
prepare_tmp() {
    TMP_DIR="$(mktemp -d /tmp/freeunit-test-full.XXXXXX)"
    if $DRY_RUN; then
        info "Dry-run: would copy project → $TMP_DIR (skipping)"
        return 0
    fi
    info "Copying project → $TMP_DIR"
    rsync -a --exclude='.git' --exclude='/build' "${PROJECT_DIR}/" "${TMP_DIR}/"
}

cleanup_tmp() {
    local rc=$?
    if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
        if [[ $rc -eq 0 ]]; then
            info "Removing tmp dir $TMP_DIR"
            rm -rf "$TMP_DIR"
        else
            info "Build failed (rc=$rc) — tmp dir preserved at $TMP_DIR"
        fi
    fi
}
trap cleanup_tmp EXIT

# ---------------------------------------------------------------------------
# Build the clang-ast image if missing
# ---------------------------------------------------------------------------
build_image() {
    if docker image inspect "$IMAGE_NAME" &>/dev/null; then
        info "Using cached image: $IMAGE_NAME"
        return 0
    fi

    if $DRY_RUN; then
        info "Dry-run: would try 'docker pull $REMOTE_IMAGE', else build locally"
    elif docker pull "$REMOTE_IMAGE" 2>/dev/null; then
        info "Pulled pre-built image: $REMOTE_IMAGE"
        docker tag "$REMOTE_IMAGE" "$IMAGE_NAME"
        return 0
    else
        info "Pull failed ($REMOTE_IMAGE) — building locally"
    fi

    info "Building clang-ast image: $IMAGE_NAME (one-time, slow)"

    local DOCKERFILE
    DOCKERFILE="$(mktemp /tmp/Dockerfile.full.XXXXXX)"

    cat > "$DOCKERFILE" <<'EOF'
FROM debian:testing

LABEL org.opencontainers.image.title="FreeUnit (full / clang-ast)"
LABEL org.opencontainers.image.vendor="FreeUnit Community <team@freeunit.org>"

# CARGO_HOME under /tmp: the check runs as an unprivileged --user with no
# writable $HOME, and `--otel` invokes cargo to build the otel library.
ENV DEBIAN_FRONTEND=noninteractive \
    CARGO_HOME=/tmp/cargo

RUN set -ex \
    && apt-get update \
    && apt-get install --no-install-recommends --no-install-suggests -y \
         ca-certificates git build-essential libssl-dev libpcre2-dev \
         zlib1g-dev libzstd-dev libbrotli-dev curl pkg-config pkgconf \
         clang llvm-dev libclang-dev rustc cargo \
    && git clone https://github.com/freeunitorg/clang-ast.git -b unit /clang-ast \
    && make -C /clang-ast

WORKDIR /unit

# Source mounted via -v at runtime.  Configure + build WITH clang-ast plugin
# to catch API/lifetime/allocator violations.  Plugin emits diagnostics during
# compile; non-zero exit = failure.
ENTRYPOINT ["bash", "-c", "\
    set -ex && \
    NCPU=$(getconf _NPROCESSORS_ONLN) && \
    rm -rf build && \
    CC=clang ./configure \
        --cc-opt='-Xclang -load -Xclang /clang-ast/ngx-ast.so \
                  -Xclang -add-plugin -Xclang ngx-ast \
                  -Wno-default-const-init-field-unsafe' \
        --otel --openssl --debug && \
    make -j $NCPU && \
    echo '=== clang-ast check PASSED ===' \
"]
EOF

    if $DRY_RUN; then
        info "Dry-run: would build with:"
        cat "$DOCKERFILE"
        rm -f "$DOCKERFILE"
        return 0
    fi

    docker build --file "$DOCKERFILE" --tag "$IMAGE_NAME" "${PROJECT_DIR}"
    rm -f "$DOCKERFILE"
}

# ---------------------------------------------------------------------------
# Run clang-ast
# ---------------------------------------------------------------------------
run_check() {
    info "Running clang-ast AST analysis"

    if $DRY_RUN; then
        info "Dry-run: would execute:"
        echo "  docker run --rm --user $(id -u):$(id -g) -v ${TMP_DIR}:/unit -w /unit ${IMAGE_NAME}"
        return 0
    fi

    docker run --rm \
        --name "freeunit-test-full" \
        --user "$(id -u):$(id -g)" \
        -v "${TMP_DIR}:/unit" \
        -w /unit \
        "${IMAGE_NAME}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
prepare_tmp
build_image
run_check
