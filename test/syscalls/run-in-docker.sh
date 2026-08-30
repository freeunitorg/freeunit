#!/bin/sh
#
# Build unitd and capture its syscall set inside a pinned container, so a
# developer's box and CI observe the same libc.
#
# Usage:
#   test/syscalls/run-in-docker.sh glibc|musl [-o OUTFILE] [--update] [--check]
#
#   -o OUTFILE  write the captured list here
#               (default: a temporary file, printed at the end)
#   --check     compare against the checked-in baseline (test/syscalls/check.sh)
#   --update    overwrite the checked-in baseline with what was captured
#
# Requires: docker.  The image is pulled for linux/amd64 explicitly -- the
# baselines are per-architecture and a silent arm64 pull would produce a
# different, wrong list.

set -eu

die() { echo "run-in-docker.sh: $*" >&2; exit 1; }

LIBC=${1:-}
shift 2>/dev/null || :
case "$LIBC" in
    glibc|musl) ;;
    *) die "usage: $0 glibc|musl [-o OUTFILE] [--update] [--check]" ;;
esac

OUT=
UPDATE=no
CHECK=no
while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUT=${2:?-o needs a path}; shift 2 ;;
        --update) UPDATE=yes; shift ;;
        --check)  CHECK=yes;  shift ;;
        *) die "unknown argument: $1" ;;
    esac
done

REPO=$(cd "$(dirname "$0")/../.." && pwd)
BASELINE="$REPO/test/syscalls/baseline-linux-$LIBC.txt"

# Pinned by digest, not by tag: the captured set depends on the libc, and
# debian:trixie-slim and alpine:3.22 are both mutable tags that would move a
# baseline with no change in this repository.  Bumping either digest is a
# baseline refresh (README.md).
#
# The digest pins the base image, not the packages apt/apk install on top of
# it.  Both are stable releases, so glibc and musl only move on a point
# release -- but that is a real residual, and one of the reasons the CI job
# is advisory rather than blocking.
if [ "$LIBC" = glibc ]; then
    IMAGE=debian:trixie-slim@sha256:d7e12182ce18b85b93007c1dedf31f2d29e01ccf3182cc4017c709b6259bc132
    INSTALL='apt-get -qq update \
        && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
             gcc make libc6-dev libpcre2-dev strace curl ca-certificates >/dev/null'
else
    IMAGE=alpine:3.22@sha256:14358309a308569c32bdc37e2e0e9694be33a9d99e68afb0f5ff33cc1f695dce
    INSTALL='apk add --no-cache gcc make musl-dev pcre2-dev strace curl >/dev/null'
fi

command -v docker >/dev/null 2>&1 || die "docker not found"

work=$(mktemp -d)
# A trapped signal does not end a POSIX shell, so each signal handler exits
# explicitly; the EXIT trap is cleared first so the cleanup runs once.
trap 'rm -rf "$work"' EXIT
trap 'trap - EXIT; rm -rf "$work"; exit 130' INT
trap 'trap - EXIT; rm -rf "$work"; exit 143' TERM

# With no -o the list goes to stdout, as the usage above says.  Writing it to
# a temporary the EXIT trap then deletes would make the plainest invocation
# the one that throws its own result away.
TO_STDOUT=no
[ -n "$OUT" ] || { OUT="$work/captured.txt"; TO_STDOUT=yes; }

# ptrace: Docker's default seccomp profile allows the ptrace syscall, but the
# default capability set has no CAP_SYS_PTRACE and the default AppArmor profile
# confines cross-process ptrace.  strace only ever traces its own descendants
# here, so SYS_PTRACE alone is enough on a stock daemon; seccomp is left at the
# default on purpose, to keep this close to how the image really runs.
docker run --rm --platform linux/amd64 \
    --cap-add=SYS_PTRACE \
    -v "$REPO:/src:ro" \
    -w /work \
    "$IMAGE" \
    sh -eu -c "
        $INSTALL
        mkdir -p /work
        # Copy out of the read-only mount: configure writes into the tree.
        tar -C /src -cf - --exclude=./.git --exclude=./build . | tar -C /work -xf -
        ./configure --prefix=/opt/unit >/dev/null
        make -j\"\$(nproc)\" >/dev/null
        exec test/syscalls/capture.sh
    " > "$OUT.raw" 2>"$work/stderr.log" || {
        cat "$work/stderr.log" >&2
        die "container run failed"
    }
cat "$work/stderr.log" >&2 || :

# The container writes progress to stderr and the list to stdout, but guard
# against a build that prints to stdout anyway: keep only syscall-shaped lines.
grep -E '^[a-z_][a-z0-9_]*$' "$OUT.raw" | LC_ALL=C sort -u > "$OUT"
rm -f "$OUT.raw"

n=$(wc -l < "$OUT" | tr -d ' ')
[ "$n" -ge 20 ] || die "captured only $n syscalls from $IMAGE -- not trusting that"
if [ "$TO_STDOUT" = yes ]; then
    echo "run-in-docker.sh: $LIBC ($IMAGE): $n syscalls" >&2
    cat "$OUT"
else
    echo "run-in-docker.sh: $LIBC ($IMAGE): $n syscalls -> $OUT" >&2
fi

if [ "$UPDATE" = yes ]; then
    {
        echo "# Syscalls issued by unitd on linux/amd64, $LIBC."
        echo "#"
        echo "# Regenerate:  test/syscalls/run-in-docker.sh $LIBC --update"
        echo "# Image:       $IMAGE"
        echo "# See test/syscalls/README.md before editing this by hand."
        cat "$OUT"
    } > "$BASELINE"
    echo "run-in-docker.sh: refreshed $BASELINE" >&2
fi

if [ "$CHECK" = yes ]; then
    "$REPO/test/syscalls/check.sh" "$BASELINE" "$OUT"
fi
