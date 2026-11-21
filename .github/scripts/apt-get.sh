#!/bin/sh
#
# apt-get for the hosted Ubuntu runners, with a fallback mirror.
#
#   sudo sh "$GITHUB_WORKSPACE/.github/scripts/apt-get.sh" ARGS...
#
# The runners fetch packages from the mirrors in /etc/apt/apt-mirrors.txt,
# all of them under ubuntu.com.  When the runner's DNS cannot resolve
# ubuntu.com, as on 2026-11-11, apt-get waits on each of them and fails,
# and adding mirrors to the end of the list does not help: apt still
# waits on the dead ones first.  So when archive.ubuntu.com does not
# resolve, this script makes the fallback mirrors the only ones, warns,
# and runs apt-get with retries.  apt moves to the next mirror of the
# list when one fails.  On a non-amd64 host, or with another list, it
# only runs apt-get.

set -eu

fallback=${APT_FALLBACK_MIRRORS:-http://mirrors.edge.kernel.org/ubuntu/ http://mirror.leaseweb.net/ubuntu/}
list=/etc/apt/apt-mirrors.txt
first=${fallback%% *}

if [ -f "$list" ] && [ "$(dpkg --print-architecture)" = amd64 ] \
   && ! grep -qF "$first" "$list" \
   && ! getent hosts archive.ubuntu.com > /dev/null 2>&1
then
    echo "::warning::archive.ubuntu.com does not resolve; apt-get uses $fallback"
    : > "$list"
    n=1
    for m in $fallback; do
        printf '%s\tpriority:%s\n' "$m" "$n" >> "$list"
        n=$((n + 1))
    done
fi

if grep -qF "$first" "$list" 2>/dev/null; then
    exec apt-get -o Acquire::Retries=3 "$@"
fi

exec apt-get "$@"
