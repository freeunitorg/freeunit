#!/bin/sh
#
# Lint the tracked shell scripts.  A file is selected by the interpreter
# on its #! line, after "env" and its options if present:
#   sh, dash, ash -> shellcheck -s sh, dash -n
#   bash          -> shellcheck -s bash, bash -n
# configure and the generated *.in templates are not checked.
# A #!/bin/sh file must not run a bare "set -o pipefail".  The guarded
# form "if (set -o pipefail) 2>/dev/null" is allowed; a comment may sit
# between the guard and the set.
#
# Usage: sh .github/scripts/lint-shell.sh [shellcheck command]
# Run it from the repository root.  The default shellcheck command is
# "shellcheck".

set -eu

SHELLCHECK=${1:-shellcheck}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/lint-shell.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# The file list is relative to the current directory, and the workflow
# mounts $PWD, so a run from elsewhere would check only part of the tree.
if [ "$(pwd -P)" != "$(git rev-parse --show-toplevel)" ]; then
    echo "lint-shell.sh: run it from the repository root" >&2
    exit 2
fi

# Print sh or bash for a #! line, or nothing.
interpreter() {
    case "$1" in
    "#!"*) ;;
    *) return 0 ;;
    esac
    set -f
    # shellcheck disable=SC2086 # split the #! line into words
    set -- ${1#??}
    set +f
    [ $# -gt 0 ] || return 0
    w=${1##*/}
    shift
    if [ "$w" = env ]; then
        # -u and -C take the next word.  -S and --split-string give the
        # rest of the line as the command line; its first word may be
        # attached to the option.
        while [ $# -gt 0 ]; do
            case "$1" in
            -u|--unset|-C|--chdir)
                [ $# -gt 1 ] || return 0
                shift 2 ;;
            --split-string=*)
                v=${1#*=}; shift; set -- "$v" "$@" ;;
            -S?*)
                v=${1#-S}; shift; set -- "$v" "$@" ;;
            -*|*=*) shift ;;
            *) break ;;
            esac
        done
        [ $# -gt 0 ] || return 0
        w=${1##*/}
    fi
    case "$w" in
    sh|dash|ash) echo sh ;;
    bash) echo bash ;;
    esac
}

: > "$tmp/sh"
: > "$tmp/bash"

# Symbolic links and submodules are skipped: they are not regular files.
# quotePath=off keeps a non-ASCII name readable by test -f.  The list goes
# to a file first, so a git failure stops the script with exit status 2.
git -c core.quotePath=off ls-files > "$tmp/files" || exit 2
while IFS= read -r f; do
    case "$f" in
    configure|*.in) continue ;;
    esac
    if ! test -f "$f" || test -L "$f"; then
        continue
    fi
    IFS= read -r first < "$f" || :
    case "$(interpreter "$first")" in
    sh) printf '%s\n' "$f" >> "$tmp/sh" ;;
    bash) printf '%s\n' "$f" >> "$tmp/bash" ;;
    esac
    first=
done < "$tmp/files"

if ! test -s "$tmp/sh" && ! test -s "$tmp/bash"; then
    echo "lint-shell.sh: no shell scripts found" >&2
    exit 2
fi

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
