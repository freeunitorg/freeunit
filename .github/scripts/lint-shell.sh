#!/bin/sh
#
# Lint the tracked shell scripts.  A file is selected by its first line:
#   #!/bin/sh, #!/usr/bin/env sh    -> shellcheck -s sh, dash -n
#   #!/bin/bash, #!/usr/bin/env bash -> shellcheck -s bash, bash -n
# configure and the generated *.in templates are not checked.
# A #!/bin/sh file must not run a bare "set -o pipefail".  The guarded
# form "if (set -o pipefail) 2>/dev/null" is allowed; a comment may sit
# between the guard and the set.
#
# Usage: sh .github/scripts/lint-shell.sh [shellcheck command]
# The default shellcheck command is "shellcheck".

set -eu

SHELLCHECK=${1:-shellcheck}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

: > "$tmp/sh"
: > "$tmp/bash"

# Symbolic links and submodules are skipped: they are not regular files.
# quotePath=off keeps a non-ASCII name readable by test -f.
git -c core.quotePath=off ls-files | while IFS= read -r f; do
    case "$f" in
    configure|*.in) continue ;;
    esac
    if ! test -f "$f" || test -L "$f"; then
        continue
    fi
    IFS= read -r first < "$f" || :
    case "$first" in
    "#!/bin/sh"|"#!/bin/sh "*|"#!/usr/bin/env sh"|"#!/usr/bin/env sh "*)
        printf '%s\n' "$f" >> "$tmp/sh" ;;
    "#!/bin/bash"|"#!/bin/bash "*|"#!/usr/bin/env bash"|"#!/usr/bin/env bash "*)
        printf '%s\n' "$f" >> "$tmp/bash" ;;
    esac
    first=
done

rc=0

while IFS= read -r f; do
    dash -n "$f" || { echo "::error file=$f::dash -n failed"; rc=1; }
    # A "set ... pipefail" line is allowed only right after the guard.
    awk -v f="$f" '
        /^[ \t]*set[ \t].*pipefail/ && prev !~ /^[ \t]*if \(set -o pipefail\) 2>\/dev\/null; then$/ {
            printf "::error file=%s,line=%d::bare set -o pipefail in a #!/bin/sh file\n", f, FNR
            bad = 1
        }
        /^[ \t]*[^ \t#]/ { prev = $0 }
        END { exit bad }' "$f" || rc=1
done < "$tmp/sh"

while IFS= read -r f; do
    bash -n "$f" || { echo "::error file=$f::bash -n failed"; rc=1; }
done < "$tmp/bash"

# The file names go through "$@", so a name with a space stays one word.
if test -s "$tmp/sh"; then
    set --
    while IFS= read -r f; do set -- "$@" "$f"; done < "$tmp/sh"
    $SHELLCHECK -s sh -S warning -f gcc "$@" || rc=1
fi
if test -s "$tmp/bash"; then
    set --
    while IFS= read -r f; do set -- "$@" "$f"; done < "$tmp/bash"
    $SHELLCHECK -s bash -S warning -f gcc "$@" || rc=1
fi

echo "Checked $(wc -l < "$tmp/sh") sh and $(wc -l < "$tmp/bash") bash files."
exit "$rc"
