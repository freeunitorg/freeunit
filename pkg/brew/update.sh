#!/bin/sh
#
# Point the Homebrew formula at a published release.
#
#   pkg/brew/update.sh X.Y.Z
#
# Run it after the X.Y.Z release has the macOS archives and the unitctl/X.Y.Z
# release has its binaries.  It downloads each file, computes its sha256 and
# rewrites the version, the URLs and the checksums in freeunit.rb.  A missing
# file stops the script before it changes anything.

set -eu

if [ $# -ne 1 ] || ! printf '%s\n' "$1" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'
then
    echo "usage: $0 X.Y.Z" >&2
    exit 1
fi

ver=$1
dir=$(cd "$(dirname "$0")" && pwd)
repo=https://github.com/freeunitorg/freeunit
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | cut -d ' ' -f 1; }
else
    sha256() { shasum -a 256 "$1" | cut -d ' ' -f 1; }
fi

fetch() {
    curl -fsSL -o "$tmp/$2" "$1" || {
        echo "$0: cannot download $1" >&2
        exit 1
    }
    sha256 "$tmp/$2"
}

sum_arm_pkg=$(fetch "$repo/releases/download/$ver/freeunit-$ver-macos-arm64.tar.gz" pkg_arm)
sum_x86_pkg=$(fetch "$repo/releases/download/$ver/freeunit-$ver-macos-x86_64.tar.gz" pkg_x86)

ctl="$repo/releases/download/unitctl/$ver/unitctl-$ver"
sum_arm_mac=$(fetch "$ctl-aarch64-apple-darwin" arm_mac)
sum_x86_mac=$(fetch "$ctl-x86_64-apple-darwin" x86_mac)
man=$(fetch \
    "https://raw.githubusercontent.com/freeunitorg/freeunit/unitctl/$ver/tools/unitctl/man/unitctl.1" \
    man)

# freeunit.rb: the version in each URL and the sha256 line after it.
sed -e "s/^  version \"[^\"]*\"/  version \"$ver\"/" \
    -e "s#\(releases/download/\)[0-9.]*\(/freeunit-\)[0-9.]*-#\1$ver\2$ver-#" \
    -e "s#\(releases/download/unitctl/\)[0-9.]*/unitctl-[0-9.]*-#\1$ver/unitctl-$ver-#" \
    -e "/-macos-arm64.tar.gz\"/{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$sum_arm_pkg\"/;}" \
    -e "/-macos-x86_64.tar.gz\"/{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$sum_x86_pkg\"/;}" \
    -e "/-aarch64-apple-darwin\"/{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$sum_arm_mac\"/;}" \
    -e "/-x86_64-apple-darwin\"/{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$sum_x86_mac\"/;}" \
    -e "s#\(freeunit/unitctl/\)[0-9.]*\(/tools/unitctl/man/unitctl.1\"\)#\1$ver\2#" \
    -e "/tools\/unitctl\/man\/unitctl.1\"/{n;s/sha256 \"[0-9a-f]*\"/sha256 \"$man\"/;}" \
    "$dir/freeunit.rb" > "$tmp/freeunit.rb"

if cmp -s "$dir/freeunit.rb" "$tmp/freeunit.rb"; then
    echo "freeunit.rb: no change"
else
    cp "$tmp/freeunit.rb" "$dir/freeunit.rb"
    echo "freeunit.rb: updated to $ver"
fi
