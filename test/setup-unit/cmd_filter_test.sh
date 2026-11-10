#!/bin/sh
#
# Check that setup-unit trusts only a "unit: main" owned by the superuser or
# the current user *where its argv is run* -- unit_cmd() feeds unit_restart()
# (eval) and unit_sock_find() (a command), both as root -- while the public
# "unit_ps" listing stays complete.  unitd's controller and router drop to the
# configured unprivileged user, so filtering them out of the listing would
# hide them from the documented "setup-unit ps -t c" / "-t r".
#
# ps(1) is stubbed: the listing shows a "unit: main" with a planted argv plus
# a controller and a router, and the per-pid owner query ("-o uid=") decides
# which of them unit_cmd() keeps.

set -eu

here=$(unset CDPATH; cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/setup-unit-cmd.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

SETUP_UNIT=${SETUP_UNIT:-$root/tools/setup-unit}
mkdir "$tmp/bin"

fail() {
    printf '%s\n' "setup-unit cmd filter test: $*" >&2
    exit 1
}

# $1 = uid the owner query reports for the "unit: main" process (pid 222).
# The controller (38) and router (39) always report a non-root, non-self uid.
write_ps_stub() {
    cat > "$tmp/bin/ps" <<EOF
#!/bin/sh
case "\$*" in
    *uid=*)
        pid=\${*##*-p }
        pid=\${pid%% *}
        case "\$pid" in
            222)   printf '%s\n' "$1" ;;
            38|39) printf '%s\n' 999 ;;
        esac
        ;;
    *)
        printf ' 222 ? S 0:00 unit: main v1 [touch $tmp/pwned]\n'
        printf '  38 ? S 0:00 unit: controller\n'
        printf '  39 ? S 0:00 unit: router\n'
        ;;
esac
exit 0
EOF
    chmod +x "$tmp/bin/ps"
}

run() {
    PATH="$tmp/bin:$PATH" "$SETUP_UNIT" "$@" </dev/null 2>/dev/null
}

# A "unit: main" owned by another user is a decoy: unit_cmd must print nothing
# for it, so its argv never reaches eval.
write_ps_stub 65534
out=$(run cmd || true)
if [ -n "$out" ]; then
    fail "trusted a foreign-owned 'unit: main' (unit_cmd output: $out)"
fi

# A "unit: main" we own is the real unitd: its argv is surfaced as before.
write_ps_stub "$(id -u)"
out=$(run cmd || true)
case $out in
    *"touch $tmp/pwned"*) ;;
    *) fail "did not surface an owned process's argv (output: $out)" ;;
esac

# The public listing is not filtered: the controller and the router run under
# the configured unprivileged user, and "setup-unit ps -t c" / "-t r" must
# still show them.
out=$(run ps -t c || true)
case $out in
    *"unit: controller"*) ;;
    *) fail "setup-unit ps -t c dropped the controller (output: $out)" ;;
esac
out=$(run ps -t r || true)
case $out in
    *"unit: router"*) ;;
    *) fail "setup-unit ps -t r dropped the router (output: $out)" ;;
esac

printf '%s\n' "setup-unit cmd filter test: OK"
