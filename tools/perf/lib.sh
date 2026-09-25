# shellcheck shell=sh disable=SC2034
# Sourced by disasm-diff.sh and layout-check.sh: options, the toolchain id,
# and a scratch build that leaves the caller's ./Makefile as it was.

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT_DIR" || exit

CC=
UPDATE=0
ALLOW_ABI_BUMP=0
CONFIGURE_OPTS=

while [ $# -gt 0 ]; do
    case "$1" in
        --*=*)
            opt=${1%%=*}
            val=${1#*=}
            shift
            set -- "$opt" "$val" "$@"
            continue
            ;;
        --cc)             CC=$2; shift ;;
        --configure-opt)  CONFIGURE_OPTS="$CONFIGURE_OPTS $2"; shift ;;
        --update)         UPDATE=1 ;;
        --allow-abi-bump) ALLOW_ABI_BUMP=1 ;;
        -h|--help)        sed -n '5,/^$/s/^# \{0,1\}//p' "$0"; exit 0 ;;
        *)                echo "unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "error: '$1' not found on PATH" >&2
        exit 1
    }
}

[ -n "$CC" ] || { echo "error: --cc <compiler> is required" >&2; exit 1; }
need "$CC"

cc_name=$(basename "$CC")
cc_version=$("$CC" --dumpversion 2>/dev/null || "$CC" --version | head -1 \
             | grep -oE '[0-9]+(\.[0-9]+)+' | head -1 || true)

case "$cc_name" in
    *musl*) libc=musl ;;
    *)      libc=glibc-$(ldd --version 2>&1 | head -1 \
                         | grep -oE '[0-9]+\.[0-9]+$' || echo unknown) ;;
esac

TOOLCHAIN="$cc_name-${cc_version:-unknown}-$libc"

# ./configure writes ./Makefile; the builds use their own build-<name>-<cc>
# directories, so ./build is not touched.  Put ./Makefile back as it was, or
# remove it when there was none.
WORK=$(mktemp -d)
MAKEFILE_SAVE=$WORK/Makefile.orig
MAKEFILE_EXISTED=0

if [ -f Makefile ]; then
    cp -p Makefile "$MAKEFILE_SAVE"
    MAKEFILE_EXISTED=1
fi

cleanup() {
    if [ "$MAKEFILE_EXISTED" -eq 1 ]; then
        mv -f "$MAKEFILE_SAVE" Makefile
    else
        rm -f Makefile
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

# build <name>: configure and build unitd and the tests in build-<name>-<cc>.
build() {
    BUILD_DIR=build-$1-$cc_name

    echo "toolchain: $TOOLCHAIN, build dir: $BUILD_DIR"

    # shellcheck disable=SC2086
    NXT_BUILD_DIR="$BUILD_DIR" ./configure --cc="$CC" --tests $CONFIGURE_OPTS \
        >"$BUILD_DIR.configure.log" 2>&1 || {
        echo "configure failed, see $BUILD_DIR.configure.log" >&2
        exit 1
    }

    { make -j2 && make -j2 tests; } >"$BUILD_DIR.build.log" 2>&1 || {
        echo "build failed, see $BUILD_DIR.build.log" >&2
        exit 1
    }
}
