#!/bin/sh
# Build FreeUnit with the Python module on Alpine and run the tests that
# depend on timing or on the word size, once normally and once on one CPU.
#
# The 1.37.1 regressions (a WebSocket close frame lost when an exception
# followed it, and the 32-bit shm limits) passed the x86 CI and failed on
# Alpine builders: on one CPU, on 32-bit, or under load.  This runs the same
# tests in that shape.  Run inside an Alpine container, from the source tree:
#
#   docker run --rm --platform linux/arm/v7 -v "$PWD:/src" -w /src \
#       alpine:3.24 sh .github/scripts/alpine-timing.sh
#
# MODE=c builds and runs the C tests only (for emulated architectures, where
# the pytest process checks cannot see the process titles).

set -eu

MODE=${MODE:-pytest}
FILES=${FILES:-test/test_asgi_websockets.py test/test_python_shm_limit.py}

apk add --no-cache build-base linux-headers openssl-dev pcre2-dev \
    python3-dev py3-pytest py3-openssl procps-ng util-linux-misc > /dev/null

echo "arch: $(uname -m), userland: $(apk --print-arch), cpus: $(nproc)"

./configure --openssl --tests
make -j"$(nproc)"

if [ "$MODE" = c ]; then
    make -j"$(nproc)" tests
    ./build/tests
    exit 0
fi

./configure python --config=python3-config
make -j"$(nproc)"

rc=0

# A lost close frame leaves a test blocked in recv() until the harness
# guard fires (--test-timeout, 300 s by default).  The slowest test here
# takes about 5 s on armv7, so 60 s still fails a hang quickly in both
# runs below.
TIMEOUT=${TIMEOUT:-60}

# shellcheck disable=SC2086
python3 -m pytest -p no:cacheprovider --durations=10 \
    --test-timeout "$TIMEOUT" $FILES || rc=1

# One CPU: the router and the application share it, which is how the
# close-frame and queue tests failed on the builders.
# shellcheck disable=SC2086
taskset -c 0 python3 -m pytest -p no:cacheprovider --durations=10 \
    --test-timeout "$TIMEOUT" test/test_asgi_websockets.py || rc=1

exit $rc
