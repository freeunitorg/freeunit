#!/usr/bin/env bash
# run-local-temp.sh — быстрый запуск тестов для разработки.
#
# Отличия от run-local.sh:
#   - Нет rsync: проект монтируется напрямую (не в tmp)
#   - Нет проверки/сборки образа: требует freeunit-test:local
#   - clang-ast отключён всегда
#   - Аргументы -t без test/ префикса → добавляется автоматически
#
# Usage:
#   ./test/run-local-temp.sh                        # полный suite
#   ./test/run-local-temp.sh test_proxy_chunked.py  # один файл
#   ./test/run-local-temp.sh test_tls.py::test_tls_certificate_change
#
# Требует: docker image freeunit-test:local (собери через run-local.sh)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="freeunit-test:local"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
    echo "ERROR: образ $IMAGE_NAME не найден. Собери: ./test/run-local.sh" >&2
    exit 1
fi

# Normalize test args: добавить test/ если нет пути
TEST_ARGS=()
if [[ $# -eq 0 ]]; then
    TEST_ARGS=("test")
else
    for arg in "$@"; do
        if [[ "$arg" == test_* ]]; then
            TEST_ARGS+=("test/$arg")
        else
            TEST_ARGS+=("$arg")
        fi
    done
fi

log "Running: pytest-3 --print-log ${TEST_ARGS[*]}"
log "(direct mount, no rsync, no clang-ast)"

exec docker run --rm --privileged \
    --name "freeunit-test-temp" \
    -v "${PROJECT_DIR}:/unit" \
    -w /unit \
    "${IMAGE_NAME}" \
    "${TEST_ARGS[@]}"
