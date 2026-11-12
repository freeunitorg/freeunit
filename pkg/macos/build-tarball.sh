#!/bin/sh
#
# Build a binary archive of unitd for the release.
#
#   pkg/macos/build-tarball.sh SRC OUT
#
# SRC is a FreeUnit source tree, OUT the directory that gets
# freeunit-X.Y.Z-macos-ARCH.tar.gz and its .sha256.  The archive is made
# for macOS; the script also runs on Linux, which makes a linux-ARCH
# archive, so that it can be tried without a Mac.
#
# OpenSSL and PCRE2 are built from checksum-verified sources and linked
# statically, so unitd needs only the system libraries.  zlib is the
# system one.  The archive has no language modules; applications of type
# "external" use the libunit in it.  unitd keeps the default paths of
# configure, under /usr/local; README.macos says how to run it elsewhere.
#
# The script checks the result: the libraries unitd loads, its version,
# and a configuration that returns 200 on a private socket and port.

set -eu

# OpenSSL: the same version and checksum as build-test.yml.  Bump both
# together; .github/scripts/version-sites checks the version.
OPENSSL_VERSION=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a

# PCRE2, from https://github.com/PCRE2Project/pcre2/releases.
PCRE2_VERSION=10.49
PCRE2_SHA256=53c156e1ba416a20da8e65395daa132da0d80e76910424caca3fcdae7831d384

if [ $# -ne 2 ]; then
    echo "usage: $0 SRC OUT" >&2
    exit 1
fi

here=$(CDPATH='' cd "$(dirname "$0")" && pwd)
src=$(CDPATH='' cd "$1" && pwd)
mkdir -p "$2"
out=$(CDPATH='' cd "$2" && pwd)

# The archive is reproducible: the same source gives the same bytes.
# configure, OpenSSL and the archive take their dates from
# SOURCE_DATE_EPOCH, by default the time of the last commit of SRC.  The
# work directory has a fixed name, because OpenSSL and configure keep its
# path in unitd.  POSIX has no mktemp; mkdir fails on a name that
# exists, so the directory is ours, and a second run at the same time
# stops.
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH=$(git -C "$src" log -1 --format=%ct 2>/dev/null) || {
        echo "$0: SRC is not a git checkout; set SOURCE_DATE_EPOCH" >&2
        exit 1
    }
fi
export SOURCE_DATE_EPOCH

work=${TMPDIR:-/tmp}/freeunit-tarball
mkdir -m 700 "$work" || {
    echo "$0: $work exists; remove it, or wait for the other build" >&2
    exit 1
}
pid=
cleanup() {
    [ -z "$pid" ] || kill "$pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
deps=$work/deps

case $(uname -s) in
Darwin)
    os=macos
    jobs=$(sysctl -n hw.ncpu)
    # The oldest macOS that the archive supports.  OpenSSL, PCRE2 and
    # unitd are all built for it.
    MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}
    export MACOSX_DEPLOYMENT_TARGET
    ;;
*)
    os=$(uname -s | tr '[:upper:]' '[:lower:]')
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
    ;;
esac

arch=$(uname -m)
case $arch in
aarch64) arch=arm64 ;;
esac

if command -v gmake >/dev/null 2>&1; then
    make='gmake'
else
    make='make'
fi

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

# Download URL to FILE in $work and check its sha256.
fetch() {
    curl -fsSL --retry 3 -o "$work/$2" "$1"
    sum=$(sha256 "$work/$2")
    if [ "$sum" != "$3" ]; then
        echo "$0: $2: sha256 $sum, expected $3" >&2
        exit 1
    fi
}

echo "== OpenSSL $OPENSSL_VERSION"
fetch "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz" \
    openssl.tar.gz "$OPENSSL_SHA256"
gzip -dc "$work/openssl.tar.gz" | (cd "$work" && tar -xf -)
# The certificates of the system are in /etc/ssl on macOS and on most
# Linux systems.  The prefix is a directory under /tmp, and OpenSSL
# compiles its module and engine directories in.  no-dso (which implies
# no-module) builds the providers into libcrypto and leaves out the
# loader, and no-engine the engines, so unitd never loads code from there.
(
    cd "$work/openssl-$OPENSSL_VERSION" &&
    ./Configure no-shared no-dso no-engine no-tests no-docs \
        --prefix="$deps" --libdir=lib --openssldir=/etc/ssl > ../openssl.log &&
    $make -j"$jobs" >> ../openssl.log &&
    $make install_sw >> ../openssl.log
) || { tail -50 "$work/openssl.log" >&2; exit 1; }

echo "== PCRE2 $PCRE2_VERSION"
fetch "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$PCRE2_VERSION/pcre2-$PCRE2_VERSION.tar.bz2" \
    pcre2.tar.bz2 "$PCRE2_SHA256"
bzip2 -dc "$work/pcre2.tar.bz2" | (cd "$work" && tar -xf -)
(
    cd "$work/pcre2-$PCRE2_VERSION" &&
    ./configure --prefix="$deps" --disable-shared --enable-static \
        --disable-pcre2grep-libz --disable-pcre2grep-libbz2 > ../pcre2.log &&
    $make -j"$jobs" >> ../pcre2.log &&
    $make install >> ../pcre2.log
) || { tail -50 "$work/pcre2.log" >&2; exit 1; }

echo "== FreeUnit"
# Build in a copy, so that the source tree stays clean.
cp -R "$src" "$work/unit"
cd "$work/unit"
rm -rf build

version=$(sed -n 's/^NXT_VERSION=//p' version)
name=freeunit-$version-$os-$arch

# configure finds PCRE2 with pcre2-config, which is first in PATH.  Only
# the static libraries are in $deps/lib, so the linker takes them.
# The paths are the defaults of configure.  unitctl finds the socket, the
# pid file and the log of a running unitd only when they are given.
{
    PATH=$deps/bin:$PATH ./configure --openssl --zlib \
        --control=unix:/usr/local/var/run/unit/control.unit.sock \
        --pid=/usr/local/var/run/unit/unit.pid \
        --log=/usr/local/var/log/unit/unit.log \
        --cc-opt="-I$deps/include" --ld-opt="-L$deps/lib" > "$work/unit.log" &&
    $make -j"$jobs" unitd manpage >> "$work/unit.log" &&
    $make build/lib/libunit.a >> "$work/unit.log"
} || { tail -50 "$work/unit.log" >&2; exit 1; }

echo "== Check $name"
unitd=$work/unit/build/sbin/unitd

case $os in
macos)
    otool -L "$unitd"
    extra=$(otool -L "$unitd" | tail -n +2 \
            | grep -Ev '^[[:space:]]+(/usr/lib/|/System/)' || true)
    minos=$(otool -l "$unitd" | grep -A4 LC_BUILD_VERSION \
            | sed -n 's/^[[:space:]]*minos[[:space:]]*//p')
    echo "minos $minos"
    if [ "$minos" != "$MACOSX_DEPLOYMENT_TARGET" ]; then
        echo "$0: unitd is built for macOS $minos," \
             "not $MACOSX_DEPLOYMENT_TARGET" >&2
        exit 1
    fi
    ;;
*)
    ldd "$unitd"
    extra=$(ldd "$unitd" | grep -E 'libssl|libcrypto|libpcre' || true)
    ;;
esac

if [ -n "$extra" ]; then
    echo "$0: unitd loads libraries that the archive does not have:" >&2
    echo "$extra" >&2
    exit 1
fi

# no-dso and no-engine leave the OpenSSL loaders out; keep it so.
if strings "$unitd" | grep -q -e 'dso_dlfcn' -e 'crypto/engine/'; then
    echo "$0: unitd has the module or engine loader of OpenSSL" >&2
    exit 1
fi

"$unitd" --version 2>&1 | grep -q "unit version: $version" || {
    echo "$0: unitd --version does not say $version" >&2
    exit 1
}

t=$work/run
mkdir -p "$t/state"
port=$((20000 + $$ % 20000))
"$unitd" --no-daemon --control "unix:$t/control.sock" \
    --statedir "$t/state" --pid "$t/unit.pid" --log "$t/unit.log" \
    --tmpdir "$t" &
pid=$!
i=0
while [ ! -S "$t/control.sock" ] && [ $i -lt 30 ]; do
    sleep 1
    i=$((i + 1))
done
ok=no
if curl -fsS -X PUT --unix-socket "$t/control.sock" \
        --data-binary "{\"listeners\": {\"127.0.0.1:$port\": {\"pass\": \"routes\"}},
                        \"routes\": [{\"action\": {\"return\": 200}}]}" \
        http://localhost/config \
   && [ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/")" = 200 ]
then
    ok=yes
fi
# unitd may have exited already; then its log says why.
kill "$pid" 2>/dev/null || true
wait "$pid" || true
pid=
if [ $ok != yes ]; then
    echo "$0: unitd did not serve the test configuration" >&2
    cat "$t/unit.log" >&2
    exit 1
fi

echo "== Pack $name"
stage=$work/$name
mkdir -p "$stage/sbin" "$stage/lib/pkgconfig" "$stage/include" \
         "$stage/share/man/man8" "$stage/share/doc/freeunit"
cp "$unitd" "$stage/sbin/"
cp build/lib/libunit.a "$stage/lib/"
# unit.pc lists the libraries that unitd links, among them OpenSSL and
# PCRE2 from $deps, which the archive does not have.  libunit needs none
# of them, so drop them, and the build directory from confargs.
sed -e "s#-L$deps/lib##g" -e "s#-I$deps/include##g" \
    -e 's/ -lssl//; s/ -lcrypto//; s/ -lpcre2-8//' \
    -e 's/[[:space:]]*$//' \
    build/share/pkgconfig/unit.pc > "$stage/lib/pkgconfig/unit.pc"
cp src/nxt_unit.h src/nxt_unit_field.h src/nxt_unit_request.h \
   src/nxt_unit_response.h src/nxt_unit_sptr.h src/nxt_unit_typedefs.h \
   src/nxt_unit_websocket.h src/nxt_websocket_header.h \
   build/include/nxt_auto_config.h build/include/nxt_version.h \
   "$stage/include/"
cp build/share/man/man8/unitd.8 "$stage/share/man/man8/"
cp LICENSE NOTICE CHANGES "$stage/share/doc/freeunit/"
cp "$work/openssl-$OPENSSL_VERSION/LICENSE.txt" \
   "$stage/share/doc/freeunit/LICENSE.openssl"
cp "$work/pcre2-$PCRE2_VERSION/LICENCE.md" \
   "$stage/share/doc/freeunit/LICENCE.pcre2.md"
sed -e "s/@VERSION@/$version/g" -e "s/@NAME@/$name/g" \
    -e "s/@OPENSSL@/$OPENSSL_VERSION/g" -e "s/@PCRE2@/$PCRE2_VERSION/g" \
    "$here/README.macos" > "$stage/README.macos"

# An "external" application links libunit with the flags of unit.pc and
# nothing else from the build.
cat > "$work/consumer.c" <<'C'
#include <nxt_unit.h>

int
main(void)
{
    nxt_unit_init_t  init = { 0 };

    return nxt_unit_init(&init) == NULL ? 0 : 1;
}
C
libs=$(sed -n 's/^Libs: //p' "$stage/lib/pkgconfig/unit.pc" \
       | sed "s#\${libdir}#$stage/lib#")
# shellcheck disable=SC2086 # split the flags of unit.pc into words
cc -I"$stage/include" -o "$work/consumer" "$work/consumer.c" $libs || {
    echo "$0: a program does not link with the flags of unit.pc: $libs" >&2
    exit 1
}

# The archive is installed with sudo into /usr/local.  It holds files only,
# owned by root with plain modes, so that the extraction neither gives the
# files to the account that had the runner's uid nor changes the modes of
# the directories that /usr/local already has.
chmod -R u=rwX,go=rX "$stage"
chmod 755 "$stage/sbin/unitd"
if tar --version 2>/dev/null | grep -q bsdtar; then
    owner="--uid 0 --gid 0 --uname root --gname wheel"
else
    owner="--owner=0 --group=0 --numeric-owner"
fi
# Every file gets the time of SOURCE_DATE_EPOCH, the names go in sorted,
# in the plain ustar format, and gzip stores no name or time.
if stamp=$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y%m%d%H%M.%S 2>/dev/null); then
    :
else
    stamp=$(date -u -r "$SOURCE_DATE_EPOCH" +%Y%m%d%H%M.%S)
fi
find "$stage" -exec env TZ=UTC0 touch -t "$stamp" {} +
(cd "$work" && find "$name" -type f | LC_ALL=C sort > files)
# shellcheck disable=SC2086 # $owner holds several options
(cd "$work" && COPYFILE_DISABLE=1 tar -cf "$name.tar" --format=ustar $owner -T files)
gzip -n -9 < "$work/$name.tar" > "$out/$name.tar.gz"
gzip -t "$out/$name.tar.gz"
sum=$(sha256 "$out/$name.tar.gz")
[ -n "$sum" ] || { echo "$0: no sha256 for the archive" >&2; exit 1; }
echo "$sum  $name.tar.gz" > "$out/$name.tar.gz.sha256"

echo "== Done"
cat "$out/$name.tar.gz.sha256"
