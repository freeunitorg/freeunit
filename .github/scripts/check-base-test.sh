#!/bin/sh
# Regression tests use only a disposable repository, never the caller's refs.
# Usage: sh .github/scripts/check-base-test.sh

set -eu

selector=$(cd "$(dirname "$0")" && pwd -P)/check-base.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/check-base-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir "$tmp/repo"
cd "$tmp/repo"
git init -q
git config user.name 'CI baseline test'
git config user.email 'ci-baseline@example.invalid'
# Tests must not inherit signing or hooks from the developer's Git config.
git config commit.gpgsign false
git config core.hooksPath /dev/null

check() {
    actual=$(sh "$selector" "$2" "$3" 2> "$tmp/notice")
    if [ "$actual" != "$4" ]; then
        printf 'FAIL: %s\n  want: %s\n  got:  %s\n' "$1" "$4" "$actual" >&2
        exit 1
    fi
    printf 'ok: %s\n' "$1"
}

check_tree() {
    actual=$(sh "$selector" --tree "$2" "$3" 2> "$tmp/notice")
    if [ "$actual" != "$4" ]; then
        printf 'FAIL: --tree %s\n  want: %s\n  got:  %s\n' "$1" "$4" "$actual" >&2
        exit 1
    fi
    printf 'ok: --tree %s\n' "$1"
}

printf 'root\n' > file
git add file
git commit -qm root
root=$(git rev-parse HEAD)
printf 'parent\n' >> file
git commit -qam parent
parent=$(git rev-parse HEAD)
printf 'head\n' >> file
git commit -qam head
head=$(git rev-parse HEAD)

git checkout -qb sibling "$root"
printf 'sibling\n' > sibling
git add sibling
git commit -qm sibling
sibling=$(git rev-parse HEAD)

check 'ancestor across several commits is preserved' "$root" "$head" "$root"
check 'direct ancestor is preserved' "$parent" "$head" "$parent"
check 'identical base and head is preserved' "$head" "$head" "$head"
check 'empty base uses first parent' '' "$head" "$parent"
check 'all-zero base uses first parent' 0000000000000000000000000000000000000000 "$head" "$parent"
check 'missing base uses first parent' ffffffffffffffffffffffffffffffffffffffff "$head" "$parent"
check 'present non-ancestor uses first parent' "$sibling" "$head" "$parent"
check 'root with no base has no preceding version' '' "$root" ''
check 'root with non-ancestor has no preceding version' "$sibling" "$root" ''

# A tree comparison keeps a present base, even the old tip of a replaced
# branch, and falls back only when there is no base to compare with.
check_tree 'ancestor is preserved' "$root" "$head" "$root"
check_tree 'present non-ancestor is preserved' "$sibling" "$head" "$sibling"
check_tree 'empty base uses first parent' '' "$head" "$parent"
check_tree 'all-zero base uses first parent' 0000000000000000000000000000000000000000 "$head" "$parent"
check_tree 'missing base uses first parent' ffffffffffffffffffffffffffffffffffffffff "$head" "$parent"
check_tree 'root with no base has no preceding version' '' "$root" ''

# A git error in the ancestor test must fail, not select a fallback.
real_git=$(command -v git)
mkdir "$tmp/bin"
cat > "$tmp/bin/git" <<EOF
#!/bin/sh
if [ "\$1" = merge-base ]; then
    exit 128
fi
exec "$real_git" "\$@"
EOF
chmod +x "$tmp/bin/git"
rc=0
out=$(PATH="$tmp/bin:$PATH" sh "$selector" "$parent" "$head" 2> "$tmp/notice") || rc=$?
if [ "$rc" -ne 128 ] || [ -n "$out" ]; then
    printf 'FAIL: merge-base error\n  want: status 128, no output\n  got:  status %s, output %s\n' "$rc" "$out" >&2
    exit 1
fi
printf 'ok: merge-base error fails\n'

# A pull-request merge keeps the base branch at its first parent.
git checkout -q -B main "$head"
git merge -qm 'test merge' "$sibling"
merge=$(git rev-parse HEAD)
check 'merge first parent is preserved' "$head" "$merge" "$head"
check 'merge fallback selects first, not second, parent' '' "$merge" "$head"

# Neither a bad head nor a real whitespace error may become success.
rc=0
sh "$selector" "$root" nonexistent > /dev/null 2>&1 || rc=$?
if [ "$rc" -eq 0 ]; then
    printf '%s\n' 'FAIL: invalid head succeeded' >&2
    exit 1
fi
printf 'ok: invalid head fails\n'
printf 'bad trailing space \n' > bad
git add bad
git commit -qm 'real whitespace error'
bad=$(git rev-parse HEAD)
base=$(sh "$selector" ffffffffffffffffffffffffffffffffffffffff "$bad" 2> "$tmp/notice")
rc=0
git log --check --format= "$base..$bad" > "$tmp/check.log" 2>&1 || rc=$?
if [ "$rc" -ne 2 ] || ! grep -q 'trailing whitespace' "$tmp/check.log"; then
    printf '%s\n' 'FAIL: genuine whitespace error did not fail' >&2
    exit 1
fi
printf 'ok: genuine whitespace error still fails\n'

# Reproduce an imported history that contains an error later removed. A
# present sibling base would make git log scan that obsolete commit too.
git checkout -q -B imported "$head"
printf '<<<<<<< HEAD\n' > obsolete
git add obsolete
git commit -qm 'historical conflict marker'
git rm -q obsolete
git commit -qm 'remove historical conflict marker'
imported=$(git rev-parse HEAD)
rc=0
git log --check --format= "$sibling..$imported" > "$tmp/history.log" 2>&1 || rc=$?
if [ "$rc" -ne 2 ] || ! grep -q 'leftover conflict marker' "$tmp/history.log"; then
    printf '%s\n' 'FAIL: historical-error control did not fail' >&2
    exit 1
fi
printf 'ok: the unfiltered imported history reproduces the old failure\n'
base=$(sh "$selector" "$sibling" "$imported" 2> "$tmp/notice")
git log --check --format= "$base..$imported" > "$tmp/head.log" 2>&1
printf 'ok: a replaced branch checks its clean head without obsolete errors\n'
