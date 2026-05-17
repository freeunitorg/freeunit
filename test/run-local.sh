#!/usr/bin/env bash
# run-local.sh — runs FreeUnit tests inside a Docker container
# mirrors pkg/docker/template.Dockerfile for build process
#
# Usage:
#   ./run-local.sh [OPTIONS] [MODULE...]
#
# Options:
#   -m MODULE   Module to test (unit, python, php, go, java, node, perl,
#               ruby, wasm, wasm-wasi-component)
#               Default: unit (runs full test suite)
#   -t TEST     Test path to run (repeatable)
#               Examples: test_tls.py  test_tls.py::test_tls_certificate_change
#   --clang-ast Run clang-ast AST analysis (C-code quality check)
#               Auto-enabled if src/**/*.{c,h} changed; use --no-clang-ast to skip
#   --no-clang-ast Skip clang-ast even if C-code changed
#   -n          Dry-run — print commands, do not execute
#   -h          Show this help
#
# Examples:
#   ./run-local.sh                          # full test suite
#   ./run-local.sh python                   # Python tests only
#   ./run-local.sh php                      # PHP tests only
#   ./run-local.sh -t test_tls.py           # single test file
#   ./run-local.sh -t test_tls.py::test_tls_certificate_change  # single test
#   ./run-local.sh -t test_a.py -t test_b.py  # multiple test files
#   ./run-local.sh unit python php          # multiple modules
#   ./run-local.sh --clang-ast              # clang-ast C-code analysis only
#   ./run-local.sh -t test_tls.py --clang-ast  # run tests, then clang-ast
#
# To force rebuild: docker rmi freeunit-test:local

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="freeunit-test:local"
DRY_RUN=false
TMP_DIR=""
RUN_CLANG_AST=auto  # auto=enabled if C-code changed, explicit flags override
NO_CLANG_AST=false

# ---------------------------------------------------------------------------
# Known modules and their test paths (glob patterns for pytest)
# ---------------------------------------------------------------------------
declare -A MODULE_TESTS=(
    [unit]="test"
    [python]="test"
    [go]="test/test_go*"
    [java]="test/test_java*"
    [node]="test/test_node*"
    [perl]="test/test_perl*"
    [php]="test/test_php*"
    [ruby]="test/test_ruby*"
    [wasm]="test/test_wasm*"
    [wasm-wasi-component]="test/test_wasm-wasi-component*"
)

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
# Argument parsing
# ---------------------------------------------------------------------------
MODULES=()
TEST_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -m) MODULES+=("$2"); shift 2 ;;
        -t) TEST_ARGS+=("$2"); shift 2 ;;
        --clang-ast) RUN_CLANG_AST=true; NO_CLANG_AST=false; shift ;;
        --no-clang-ast) RUN_CLANG_AST=false; NO_CLANG_AST=true; shift ;;
        -n) DRY_RUN=true; shift ;;
        -h) usage ;;
        -*) err "Unknown option: $1"; exit 1 ;;
        *) MODULES+=("$1"); shift ;;
    esac
done

# Defaults: no -t and no modules → run full unit test suite
if [[ ${#TEST_ARGS[@]} -eq 0 ]] && [[ ${#MODULES[@]} -eq 0 ]]; then
    MODULES=("unit")
fi

# Expand modules → test path patterns (only when -t not given)
if [[ ${#TEST_ARGS[@]} -eq 0 ]] && [[ ${#MODULES[@]} -gt 0 ]]; then
    for m in "${MODULES[@]}"; do
        if [[ -n "${MODULE_TESTS[$m]:-}" ]]; then
            TEST_ARGS+=("${MODULE_TESTS[$m]}")
        else
            err "Unknown module: '$m'. Known: ${!MODULE_TESTS[*]}"
            exit 1
        fi
    done
fi

# ---------------------------------------------------------------------------
# Tmp copy — isolate source from live tree, avoid live-source mutation
# ---------------------------------------------------------------------------
prepare_tmp() {
    TMP_DIR="$(mktemp -d /tmp/freeunit-test.XXXXXX)"
    if $DRY_RUN; then
        info "Dry-run: would copy project → $TMP_DIR (skipping)"
        return 0
    fi
    info "Copying project → $TMP_DIR"
    rsync -a --exclude='.git' --exclude='/build' "${PROJECT_DIR}/" "${TMP_DIR}/"
}

cleanup_tmp() {
    if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
        info "Tmp dir left at $TMP_DIR (cleanup disabled)"
        # rm -rf "$TMP_DIR"
    fi
}
trap cleanup_tmp EXIT

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------
if ! command -v docker &>/dev/null; then
    err "docker not found in PATH"; exit 1
fi

# Auto-enable clang-ast if C-code changed (staged or unstaged) and not explicitly disabled
if [[ "$RUN_CLANG_AST" == "auto" ]] && ! $NO_CLANG_AST; then
    if { git -C "${PROJECT_DIR}" diff --name-only HEAD 2>/dev/null;
         git -C "${PROJECT_DIR}" diff --name-only --cached HEAD 2>/dev/null;
         git -C "${PROJECT_DIR}" ls-files --others --exclude-standard 2>/dev/null; } \
       | grep -qE '\.(c|h)$|^auto/'; then
        RUN_CLANG_AST=true
        info "clang-ast: auto-enabled (C-code changes detected)"
    else
        RUN_CLANG_AST=false
    fi
elif [[ "$RUN_CLANG_AST" == "auto" ]]; then
    RUN_CLANG_AST=false
fi

info "============================================================"
info "FreeUnit local test run"
info "  Modules  : ${MODULES[*]:--}"
info "  Test args: ${TEST_ARGS[*]}"
info "  clang-ast: ${RUN_CLANG_AST}"
info "  Dry-run  : ${DRY_RUN}"
info "============================================================"

# ---------------------------------------------------------------------------
# Build the test image if it doesn't exist
# ---------------------------------------------------------------------------
build_image() {
    if docker image inspect "$IMAGE_NAME" &>/dev/null; then
        info "Using cached image: $IMAGE_NAME"
        return 0
    fi

    info "Building test image: $IMAGE_NAME"

    local DOCKERFILE
    DOCKERFILE="$(mktemp /tmp/Dockerfile.test.XXXXXX)"

    # Note: <<'EOF' — no shell expansion; $@, $(...) are literal for Docker/bash
    cat > "$DOCKERFILE" <<'EOF'
FROM python:3.14-slim-trixie

LABEL org.opencontainers.image.title="FreeUnit (test)"
LABEL org.opencontainers.image.vendor="FreeUnit Community <team@freeunit.org>"

ENV DEBIAN_FRONTEND=noninteractive \
    CARGO_HOME=/usr/src/unit/cargo \
    RUSTUP_HOME=/usr/src/unit/rustup \
    PATH=/usr/src/unit/cargo/bin:/usr/local/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

RUN set -ex \
    && apt-get update \
    && apt-get install --no-install-recommends --no-install-suggests -y \
         ca-certificates git build-essential libssl-dev openssl libpcre2-dev \
         zlib1g-dev libzstd-dev libbrotli-dev curl wget pkg-config pkgconf \
         libclang-dev cmake python3-pytest python3-openssl sudo procps \
    && export RUST_VERSION=1.94.1 \
    && export RUSTUP_HOME=/usr/src/unit/rustup \
    && export CARGO_HOME=/usr/src/unit/cargo \
    && export PATH=/usr/src/unit/cargo/bin:$PATH \
    && dpkgArch="$(dpkg --print-architecture)" \
    && case "${dpkgArch##*-}" in \
         amd64) rustArch="x86_64-unknown-linux-gnu"; rustupSha256="6aeece6993e902708983b209d04c0d1dbb14ebb405ddb87def578d41f920f56d" ;; \
         arm64) rustArch="aarch64-unknown-linux-gnu"; rustupSha256="1cffbf51e63e634c746f741de50649bbbcbd9dbe1de363c9ecef64e278dba2b2" ;; \
         *) echo >&2 "unsupported architecture: ${dpkgArch}"; exit 1 ;; \
       esac \
    && url="https://static.rust-lang.org/rustup/archive/1.27.1/${rustArch}/rustup-init" \
    && curl -L -O "$url" \
    && echo "${rustupSha256} *rustup-init" | sha256sum -c - \
    && chmod +x rustup-init \
    && ./rustup-init -y --no-modify-path --profile minimal --default-toolchain $RUST_VERSION --default-host ${rustArch} \
    && rm rustup-init \
    && rustup --version && cargo --version && rustc --version \
    && mkdir -p /usr/lib/unit/modules /usr/lib/unit/debug-modules \
    && adduser --system --group --no-create-home unit

WORKDIR /unit

# Source is mounted via -v at runtime. Build happens here.
# $@ receives test path args from `docker run IMAGE <args>`.
# Unquoted $@ → word-split + glob-expand (handles test/test_go* patterns).
ENTRYPOINT ["bash", "-c", "\
    set -ex && \
    NCPU=$(getconf _NPROCESSORS_ONLN) && \
    DEB_HOST_MULTIARCH=$(dpkg-architecture -q DEB_HOST_MULTIARCH) && \
    make -j $NCPU -C pkg/contrib .njs && \
    export PKG_CONFIG_PATH=$(pwd)/pkg/contrib/njs/build && \
    ./configure \
        --prefix=/usr \
        --statedir=/var/lib/unit \
        --control=unix:/var/run/control.unit.sock \
        --runstatedir=/var/run \
        --pid=/var/run/unit.pid \
        --logdir=/var/log \
        --log=/var/log/unit.log \
        --tmpdir=/var/tmp \
        --user=nobody \
        --group=nogroup \
        --openssl \
        --njs \
        --otel \
        --zlib \
        --zstd \
        --brotli \
        --libdir=/usr/lib/$DEB_HOST_MULTIARCH \
        --cc-opt=-fPIC \
        --tests \
        --modulesdir=/usr/lib/unit/debug-modules \
        --debug && \
    make -j $NCPU unitd && \
    ./configure python --config=/usr/local/bin/python3-config && \
    printf 'NXT_INCS += -I%s/pkg/contrib/njs/src -I%s/pkg/contrib/njs/build\\n' \
        $(pwd) $(pwd) >> build/Makefile && \
    make python3 && \
    cargo build --release --manifest-path test/fake_upstream/Cargo.toml && \
    cp test/fake_upstream/target/release/fake_upstream /usr/local/bin/fake_upstream && \
    if [ \"$1\" = '--clang-ast' ]; then \
        apt-get update -qq && apt-get install -y -qq --no-install-recommends \
            clang llvm-dev libclang-dev ca-certificates git && \
        git clone https://github.com/freeunitorg/clang-ast.git -b unit /clang-ast && \
        make -C /clang-ast && \
        rm -rf build && \
        CC=clang ./configure --cc-opt='-Xclang -load -Xclang /clang-ast/ngx-ast.so \
            -Xclang -add-plugin -Xclang ngx-ast -Wno-default-const-init-field-unsafe' \
            --openssl --debug && \
        make -j $NCPU && \
        echo '=== clang-ast check PASSED ===' && exit 0; \
    fi && \
    exec pytest-3 --print-log $@ \
", "bash"]

CMD ["test"]
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
# Run tests (or clang-ast)
# ---------------------------------------------------------------------------
run_tests() {
    local RUN_ARGS=()

    if $RUN_CLANG_AST; then
        RUN_ARGS=("--clang-ast")
        info "Running: clang-ast AST analysis"
    else
        RUN_ARGS=("${TEST_ARGS[@]}")
        info "Running: pytest-3 --print-log ${TEST_ARGS[*]}"
    fi

    if $DRY_RUN; then
        info "Dry-run: would execute:"
        echo "  docker run --rm --privileged -v ${TMP_DIR}:/unit -w /unit ${IMAGE_NAME} ${RUN_ARGS[*]}"
        return 0
    fi

    docker run --rm --privileged \
        --name "freeunit-test" \
        -v "${TMP_DIR}:/unit" \
        -w /unit \
        "${IMAGE_NAME}" \
        "${RUN_ARGS[@]}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
prepare_tmp
build_image

# Run tests first (if specified)
if [[ ${#TEST_ARGS[@]} -gt 0 ]]; then
    run_tests
fi

# Run clang-ast (if specified)
if $RUN_CLANG_AST; then
    run_tests
fi

# If neither tests nor clang-ast specified, run default tests
if [[ ${#TEST_ARGS[@]} -eq 0 ]] && ! $RUN_CLANG_AST; then
    run_tests
fi
