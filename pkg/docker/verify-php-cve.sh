#!/bin/sh
#
# Check a FreeUnit PHP image for the PHP and Brotli CVEs of 2026.
#
# Usage: verify-php-cve.sh IMAGE [UNIT_VERSION [nts|zts]]
#
# The script runs the checks in the image and prints one line for each.  It
# exits 1 when a check fails.  It needs docker and no network.
#
# The checks:
#
#   php version      The version is at least the fixed version of the branch.
#                    The fixed versions are in pkg/community/PHP.CVE.md.
#   thread safety    The PHP build is the expected one: non-thread-safe
#                    (NTS) for a "php-<version>" image, thread-safe (ZTS)
#                    for a "php-<version>-zts" image.  The official php
#                    images are ZTS only from PHP 8.6, so a base image
#                    that changes its build is seen here, not by the users.
#   php module       The FreeUnit PHP module finds every library that it
#                    needs, and one of them is libphp.  The module works
#                    only with the libphp that it was built with.
#   upload_progress  The setting session.upload_progress.enabled is Off.
#   libbrotli1       The version is at least 1.1.0-2+deb13u1.
#   unitd            The daemon reports the expected version.  The check is
#                    skipped when the second argument is missing.
#
# The first argument is the image.  The second argument is the expected
# version of the daemon; an empty one skips the check.  The third argument
# is the expected PHP build, "nts" (the default) or "zts".

set -eu

usage() {
	echo "usage: $0 IMAGE [UNIT_VERSION [nts|zts]]" >&2
	exit 2
}

if [ $# -lt 1 ] || [ $# -gt 3 ]; then
	usage
fi

image=$1
want_unit=${2:-}
case "${3:-nts}" in
nts) want_ts=NTS ;;
zts) want_ts=ZTS ;;
*)   usage ;;
esac

# The expected daemon version matches as a whole token, so 1.37.1 does not
# accept 1.37.10.  The dots of the version are escaped for the pattern.
want_unit_esc=$(printf '%s\n' "$want_unit" | sed 's/[.[\*^$\\]/\\&/g')

# The first PHP version of each branch that carries the fix of the critical
# RCEs.
php_min_8_2=8.2.35
php_min_8_3=8.3.36
php_min_8_4=8.4.27
php_min_8_5=8.5.12
php_min_8_6=8.6.0

# The first libbrotli1 version that carries the fix of the overflow.
brotli_min=1.1.0-2+deb13u1

fail=0

report() {
	# report STATUS NAME DETAIL
	printf '%-4s %-15s %s\n' "$1" "$2" "$3"
}

pass_check() {
	report PASS "$1" "$2"
}

fail_check() {
	report FAIL "$1" "$2"
	fail=1
}

# The version of a Debian package is at least the wanted one.
version_ge() {
	dpkg --compare-versions "$1" ge "$2"
}

# The image answers with six lines.
if ! out=$(docker run --rm -i --entrypoint sh "$image" -s <<'IN_IMAGE'
php -r 'echo PHP_VERSION, "\n";' 2>/dev/null || echo unknown
php -r 'echo (ini_get("session.upload_progress.enabled") ? "On" : "Off"), "\n";' 2>/dev/null || echo unknown
b=$(dpkg-query -W -f='${Version}' libbrotli1 2>/dev/null) && printf '%s\n' "$b" || echo absent
unitd --version 2>&1 | head -n 1
php -r 'echo PHP_ZTS ? "ZTS" : "NTS", "\n";' 2>/dev/null || echo unknown
m=$(ls /usr/lib/unit/modules/php*.unit.so 2>/dev/null | head -n 1)
if [ -z "$m" ]; then
	echo absent
else
	case "$(ldd "$m" 2>&1)" in
	*'not found'*) echo unresolved ;;
	*libphp*)      echo linked ;;
	*)             echo nolibphp ;;
	esac
fi
IN_IMAGE
); then
	out=
fi

php_version=$(printf '%s\n' "$out" | sed -n '1p')
upload_progress=$(printf '%s\n' "$out" | sed -n '2p')
brotli=$(printf '%s\n' "$out" | sed -n '3p')
unitd=$(printf '%s\n' "$out" | sed -n '4p')
thread_safety=$(printf '%s\n' "$out" | sed -n '5p')
php_module=$(printf '%s\n' "$out" | sed -n '6p')

case "$php_version" in
8.2.*) php_min=$php_min_8_2 ;;
8.3.*) php_min=$php_min_8_3 ;;
8.4.*) php_min=$php_min_8_4 ;;
8.5.*) php_min=$php_min_8_5 ;;
8.6.*) php_min=$php_min_8_6 ;;
*) php_min= ;;
esac

if [ -z "$php_min" ]; then
	fail_check "php version" "$php_version is not a known branch"
elif version_ge "$php_version" "$php_min"; then
	pass_check "php version" "$php_version is at least $php_min"
else
	fail_check "php version" "$php_version is below $php_min"
fi

if [ "$thread_safety" = "$want_ts" ]; then
	pass_check "thread safety" "PHP is $thread_safety"
else
	fail_check "thread safety" "PHP is ${thread_safety:-unknown}, the image must be $want_ts"
fi

case "$php_module" in
linked)     pass_check "php module" "the module finds libphp and its other libraries" ;;
absent)     fail_check "php module" "no php*.unit.so in /usr/lib/unit/modules" ;;
unresolved) fail_check "php module" "the module needs a library that is not found" ;;
*)          fail_check "php module" "the module does not link libphp (${php_module:-unknown})" ;;
esac

if [ "$upload_progress" = "Off" ]; then
	pass_check "upload_progress" "the setting is Off"
else
	fail_check "upload_progress" "the setting is ${upload_progress:-unknown}"
fi

if [ -z "$brotli" ] || [ "$brotli" = "absent" ]; then
	fail_check "libbrotli1" "the package is absent"
elif version_ge "$brotli" "$brotli_min"; then
	pass_check "libbrotli1" "$brotli is at least $brotli_min"
else
	fail_check "libbrotli1" "$brotli is below $brotli_min"
fi

if [ -z "$want_unit" ]; then
	report SKIP "unitd" "$unitd"
elif printf '%s\n' "$unitd" \
	| grep -qE "(^|[^0-9.])${want_unit_esc}([^0-9.]|$)"; then
	pass_check "unitd" "$unitd"
else
	fail_check "unitd" "$unitd does not name $want_unit"
fi

if [ "$fail" -ne 0 ]; then
	echo "verify-php-cve: FAILED for $image" >&2
	exit 1
fi

echo "verify-php-cve: every check passed for $image"
