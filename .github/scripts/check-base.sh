#!/bin/sh
# Select an ancestor baseline for commit checks. Print an empty line when
# the head is a root commit and there is no preceding version to compare.
# Usage: sh .github/scripts/check-base.sh [--tree] [event-base [head]]
#
# --tree is for a check that compares the trees of two commits, such as
# the layout check. Such a check does not need an ancestor, so a present
# event base is kept even when it is not an ancestor of the head.

set -eu

tree=0
if [ "${1:-}" = --tree ]; then
    tree=1
    shift
fi

if [ "$#" -gt 2 ]; then
    printf '%s\n' 'check-base.sh: expected an event base and optional head' >&2
    exit 2
fi

base=${1:-}
head=${2:-HEAD}
head=$(git rev-parse --verify "${head}^{commit}")

# A new branch has no base. A replaced branch can retain its old base in
# the clone through another ref, but that does not make it an ancestor.
if [ -n "$base" ] \
   && [ "$base" != 0000000000000000000000000000000000000000 ] \
   && git cat-file -e "${base}^{commit}" 2>/dev/null
then
    if [ "$tree" -eq 1 ]; then
        git rev-parse --verify "${base}^{commit}"
        exit 0
    fi

    rc=0
    git merge-base --is-ancestor "$base" "$head" || rc=$?
    case "$rc" in
    0) git rev-parse --verify "${base}^{commit}"; exit 0 ;;
    1) ;; # A non-ancestor uses the same fallback as an unavailable base.
    *) exit "$rc" ;;
    esac
fi

# rev-list also fails for an unreadable parent object; do not treat that
# error as a root commit or silently select another baseline.
parents=$(git rev-list --parents -n 1 "$head")
# Only full object IDs from git are split here.
# shellcheck disable=SC2086
set -- $parents
if [ "$#" -gt 1 ]; then
    printf "::notice::Base '%s' is not an available ancestor; using the head's first parent as the baseline.\n" "$base" >&2
    printf '%s\n' "$2"
else
    printf "::notice::Base '%s' is not an available ancestor and the head has no parent.\n" "$base" >&2
    printf '\n'
fi
