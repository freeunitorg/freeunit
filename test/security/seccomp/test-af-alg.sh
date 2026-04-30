#!/bin/sh
# Verify seccomp-no-af-alg.json blocks AF_ALG (CVE-2026-31431) without
# breaking normal socket operations.
#
# Requires: docker; IMAGE must have python3 installed.
#
# Usage:
#   ./test/security/seccomp/test-af-alg.sh [IMAGE]
#   IMAGE defaults to python:3.13-slim-trixie; any image with python3 works

PROFILE="$(cd "$(dirname "$0")/../../.." && pwd)/pkg/docker/seccomp-no-af-alg.json"
IMAGE="${1:-python:3.13-slim-trixie}"
PASS=0
FAIL=0

run_test() {
    DESC="$1"
    CODE="$2"
    if output=$(docker run --rm --security-opt "seccomp=$PROFILE" "$IMAGE" \
            python3 -c "$CODE" 2>&1); then
        echo "PASS: $DESC"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $DESC — $output"
        FAIL=$((FAIL + 1))
    fi
}

echo "Profile: $PROFILE"
echo "Image:   $IMAGE"
echo ""

# AF_ALG (38) must be blocked
run_test "AF_ALG socket blocked" "
import socket, sys
try:
    socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET)
    print('not blocked', file=sys.stderr); sys.exit(1)
except PermissionError:
    pass
"

# AF_INET TCP must work
run_test "AF_INET TCP socket works" "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.close()
"

# AF_INET6 TCP must work
run_test "AF_INET6 TCP socket works" "
import socket
s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
s.close()
"

# AF_UNIX must work (Unit uses Unix sockets internally)
run_test "AF_UNIX socket works" "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.close()
"

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
