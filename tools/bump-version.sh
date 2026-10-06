#!/bin/sh
#
# Set the release version in every file that carries it.
#
#   tools/bump-version.sh X.Y.Z
#
# The sites are the rows of .github/scripts/version-sites; this script
# keeps no list of its own.  It edits the rows of kind line, vernum,
# cargo-package and cargo-lock, and runs "make dockerfiles" for the
# dockerfiles row.  Running it twice with the same version changes nothing.
#
# It does not write text that needs a person: the rows of kind
# changelog-md, security-md and changes (the CHANGES date, the two
# docs/changes.xml stanzas, the unitctl CHANGELOG section and the
# SECURITY.md table).  It ends with .github/scripts/check-version.sh,
# which names what is still missing.  Edit those files, run the check
# again, and make the release commit by hand.  This script does not
# commit.
#
# Run it in a clean tree.  If it stops with an error, the rows before the
# error are already edited; "git checkout -- ." restores them.

set -eu

ver=${1:-}

if ! printf '%s\n' "$ver" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "usage: $0 X.Y.Z" >&2
    exit 2
fi

cd "$(dirname "$0")/.."

sites=.github/scripts/version-sites
tab=$(printf '\t')
num=$(printf '%s\n' "$ver" | awk -F. '{ print $1 * 10000 + $2 * 100 + $3 }')
tmp=

# INT and TERM exit; a trap that does not exit would let rewrite() go on
# and truncate FILE.  The EXIT trap removes the temporary file.
trap '[ -z "$tmp" ] || rm -f "$tmp"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# rewrite FILE COMMAND...: replace FILE with the output of COMMAND FILE,
# keeping the file mode.  The output goes to a temporary file next to
# FILE, which the trap removes when COMMAND fails.
rewrite() {
    f=$1
    shift
    tmp=$(mktemp "$f.XXXXXX")
    "$@" "$f" > "$tmp"
    cat "$tmp" > "$f"
    rm -f "$tmp"
    tmp=
}

while IFS=$tab read -r path kind a1 a2; do
    case $path in
    ''|'#'*)
        continue ;;
    esac

    case $kind in
    line)
        rewrite "$path" sed "s#^\($a1\).*\($a2\)\$#\1$ver\2#" ;;
    vernum)
        rewrite "$path" sed "s#^\($a1\).*\$#\1$num#" ;;
    cargo-package)
        # Only the version line of the [package] table.
        rewrite "$path" awk -v v="$ver" '/^\[/ { p = ($0 == "[package]") }
            p && /^version *=/ { $0 = "version = \"" v "\""; p = 0 }
            { print }' ;;
    cargo-lock)
        # The version line that follows 'name = "<crate>"'.
        rewrite "$path" awk -v n="name = \"$a1\"" -v v="$ver" '
            $0 == n { f = 1; print; next }
            f && /^version = / { $0 = "version = \"" v "\""; f = 0 }
            { print }' ;;
    dockerfiles)
        # No environment but PATH: the Makefile takes VERSION, MODULES and
        # the other "?=" variables from it.
        env -i PATH="$PATH" make -s -B -C "$path" dockerfiles \
            < /dev/null > /dev/null ;;
    *)
        # Written by a person; check-version.sh reports them.
        ;;
    esac
done < "$sites"

echo "bump-version: files set to $ver; checking the rest:"
sh .github/scripts/check-version.sh
