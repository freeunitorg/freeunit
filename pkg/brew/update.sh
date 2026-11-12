#!/bin/sh
#
# Point the Homebrew formulae at a published release.
#
#   pkg/brew/update.sh X.Y.Z
#
# Run it after the X.Y.Z release has unit-X.Y.Z.tar.gz and the unitctl/X.Y.Z
# release has its binaries.  It downloads each file, computes its sha256 and
# rewrites the version, the URLs and the checksums in freeunit.rb and
# unitctl.rb.  A missing file stops the script before it changes anything.

set -eu

if [ $# -ne 1 ] || ! printf '%s\n' "$1" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'
then
    echo "usage: $0 X.Y.Z" >&2
    exit 1
fi

ver=$1
dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
repo=https://github.com/freeunitorg/freeunit
# POSIX has no mktemp; mkdir fails on a name that exists.
tmp=${TMPDIR:-/tmp}/freeunit-brew.$$
mkdir -m 700 "$tmp"
trap 'rm -rf "$tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | cut -d ' ' -f 1; }
else
    sha256() { shasum -a 256 "$1" | cut -d ' ' -f 1; }
fi

fetch() {
    curl -fsSL --retry 3 -o "$tmp/$2" "$1" || {
        echo "$0: cannot download $1" >&2
        exit 1
    }
    sum=$(sha256 "$tmp/$2")
    case $sum in
    *[!0-9a-f]* | '')
        echo "$0: no sha256 for $1" >&2
        exit 1
        ;;
    esac
    [ ${#sum} -eq 64 ] || { echo "$0: no sha256 for $1" >&2; exit 1; }
    echo "$sum"
}

tarball=$(fetch "$repo/releases/download/$ver/unit-$ver.tar.gz" tarball)

ctl="$repo/releases/download/unitctl/$ver/unitctl-$ver"
sum_arm_mac=$(fetch "$ctl-aarch64-apple-darwin" arm_mac)
sum_x86_mac=$(fetch "$ctl-x86_64-apple-darwin" x86_mac)
sum_arm_linux=$(fetch "$ctl-aarch64-unknown-linux-gnu" arm_linux)
sum_x86_linux=$(fetch "$ctl-x86_64-unknown-linux-gnu" x86_linux)

man=$(fetch \
    "https://raw.githubusercontent.com/freeunitorg/freeunit/unitctl/$ver/tools/unitctl/man/unitctl.1" \
    man)

# freeunit.rb: the url line and the sha256 line after it.
sed -e "s#^\(  url \"$repo/releases/download/\)[^/]*/unit-[^\"]*\"#\1$ver/unit-$ver.tar.gz\"#" \
    -e "\#^  url \"$repo/releases/download/#{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$tarball\"/;}" \
    "$dir/freeunit.rb" > "$tmp/freeunit.rb"

# unitctl.rb: the version in each URL, then each sha256 after the url of
# its target.
sed -e "s#/unitctl/[0-9.]*/unitctl-[0-9.]*-#/unitctl/$ver/unitctl-$ver-#" \
    -e "/-aarch64-apple-darwin\"/{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$sum_arm_mac\"/;}" \
    -e "/-x86_64-apple-darwin\"/{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$sum_x86_mac\"/;}" \
    -e "/-aarch64-unknown-linux-gnu\"/{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$sum_arm_linux\"/;}" \
    -e "/-x86_64-unknown-linux-gnu\"/{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$sum_x86_linux\"/;}" \
    -e "s#\(freeunit/unitctl/\)[0-9.]*\(/tools/unitctl/man/unitctl.1\"\)#\1$ver\2#" \
    -e "/tools\/unitctl\/man\/unitctl.1\"/{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$man\"/;}" \
    "$dir/unitctl.rb" > "$tmp/unitctl.rb"

# A formula laid out differently would leave a line unchanged with no
# error; check that each new value is in the result.
for want in "$ver/unit-$ver.tar.gz" "$tarball"; do
    grep -qF "$want" "$tmp/freeunit.rb" || {
        echo "$0: freeunit.rb: $want is not set" >&2
        exit 1
    }
done
for want in "unitctl/$ver/unitctl-$ver-" "$sum_arm_mac" "$sum_x86_mac" \
            "$sum_arm_linux" "$sum_x86_linux" "$man"; do
    grep -qF "$want" "$tmp/unitctl.rb" || {
        echo "$0: unitctl.rb: $want is not set" >&2
        exit 1
    }
done

for f in freeunit.rb unitctl.rb; do
    if cmp -s "$dir/$f" "$tmp/$f"; then
        echo "$f: no change"
    else
        cp "$tmp/$f" "$dir/$f"
        echo "$f: updated to $ver"
    fi
done
