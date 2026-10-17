#!/bin/sh
#
# Test the file selection and the exit status of lint-shell.sh.  The real
# tree has only #! forms that a simple match finds, so this script makes
# a throwaway repository with the other forms.  The linter command is
# "echo", so the output shows the files that the linter would get.
#
# Usage: sh .github/scripts/lint-shell-test.sh

set -eu

lint=$(cd "$(dirname "$0")" && pwd -P)/lint-shell.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/lint-shell-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail=0

check() {
    if [ "$2" != "$3" ]; then
        printf 'FAIL: %s\n  want: %s\n  got:  %s\n' "$1" "$3" "$2"
        fail=1
    else
        printf 'ok: %s\n' "$1"
    fi
}

repo=$tmp/repo
mkdir "$repo" "$repo/sub"
cd "$repo"
git init -q

printf '#!/bin/sh\n:\n' > plain.sh
printf '#!/bin/dash\n:\n' > a.dash
printf '#!/bin/ash\n:\n' > b.ash
printf '#! /bin/sh -eu\n:\n' > c.space
printf '#!/usr/bin/env -S bash -e\n:\n' > d.envs
printf '#!/usr/bin/env -u HOME bash\n:\n' > e.envu
printf '#!/usr/bin/env --split-string=bash\n:\n' > f.envsplit
printf '#!/usr/bin/env -C /tmp sh\n:\n' > g.envc
printf '#!/usr/bin/env python3\nprint(1)\n' > h.py
printf 'no interpreter line\n' > i.txt
printf '#!/bin/sh\n:\n' > configure
printf '#!/bin/sh\n:\n' > j.in
ln -s plain.sh k.link
git add .

out=$(sh "$lint" echo)
sh_files=$(printf '%s\n' "$out" | sed -n 's/^-s sh -S warning -f gcc //p')
bash_files=$(printf '%s\n' "$out" | sed -n 's/^-s bash -S warning -f gcc //p')

check "sh selection" "$sh_files" "a.dash b.ash c.space g.envc plain.sh"
check "bash selection" "$bash_files" "d.envs e.envu f.envsplit"

rc=0
(cd sub && sh "$lint" echo) > /dev/null 2>&1 || rc=$?
check "exit status outside the root" "$rc" 2

rc=0
printf 'not an index\n' > .git/index
sh "$lint" echo > /dev/null 2>&1 || rc=$?
check "exit status when git ls-files fails" "$rc" 2

exit "$fail"
