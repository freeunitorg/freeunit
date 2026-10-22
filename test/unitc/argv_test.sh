#!/bin/sh
#
# Check the control-target argv that tools/unitc builds for each transport.
#
# Before PR 745 the whole control target was quoted, so curl received
# "--unix-socket /path _" as one argument.  These stubs log one argument
# per line and the test requires the option, the socket path and the
# "_/..." URL to be separate, while the http:// URL stays one argument.
#
# unitc takes the GET and DELETE paths only when stdin is a terminal, so
# those two runs go through script(1), which lends a pseudo-terminal.

set -eu

here=$(unset CDPATH; cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/unitc-argv.XXXXXX")
# unitc keeps its cache in /tmp/<own name>.<unitd pid>.env.  Run a copy
# with a name of its own, so the test never touches the cache of a real
# unitc, and two runs do not share one.
name=unitc-argv-test.${tmp##*.}
trap 'rm -rf "$tmp"; rm -f "/tmp/$name.111.env"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cp "${UNITC:-$root/tools/unitc}" "$tmp/$name"
chmod +x "$tmp/$name"
UNITC=$tmp/$name

mkdir "$tmp/bin"

cat > "$tmp/bin/curl" <<'EOF'
#!/bin/sh
{ printf '%s\n' "$@"; } >> "$STUBLOG"
for a in "$@"; do
    if [ "$a" = --version ]; then exit 0; fi
done
if [ ! -t 0 ]; then cat >/dev/null; fi
printf '%s' '{"ok":true}'
EOF

cat > "$tmp/bin/docker" <<'EOF'
#!/bin/sh
{ printf '%s\n' "$@"; } >> "$STUBLOG"
exit 0
EOF

cat > "$tmp/bin/ssh" <<'EOF'
#!/bin/sh
{ printf '%s\n' "$@"; } >> "$STUBLOG"
exit 0
EOF

cat > "$tmp/bin/ps" <<'EOF'
#!/bin/sh
case "$*" in
    ax) printf ' 111 ? S 0:00 unit: main\n' ;;
esac
exit 0
EOF

# script(1) runs this on the pseudo-terminal.  The unitc arguments come
# through TTY_ARGS, split on blanks, so run_tty takes no argument with one.
cat > "$tmp/bin/ttywrap" <<'EOF'
#!/bin/sh
# shellcheck disable=SC2086
exec "$UNITC" $TTY_ARGS
EOF

chmod +x "$tmp/bin/curl" "$tmp/bin/docker" "$tmp/bin/ssh" "$tmp/bin/ps" \
    "$tmp/bin/ttywrap"
printf '{"x":1}\n' > "$tmp/conf.json"
printf 'UNIT_CTRL="--unix-socket /tmp/unitc-argv.sock _"\nERROR_LOG=/dev/null\n' \
    > "/tmp/$name.111.env"

fail() {
    printf '%s\n' "unitc argv test: $*" >&2
    exit 1
}

assert_arg() {
    if ! grep -qxF -e "$1" "$2"; then
        fail "missing argument '$1' in $2"
    fi
}

assert_no_arg() {
    if grep -qxF -e "$1" "$2"; then
        fail "unexpected single argument '$1' in $2"
    fi
}

run() {
    log=$1
    shift
    : > "$log"
    STUBLOG=$log
    export STUBLOG
    if ! PATH="$tmp/bin:$PATH" "$UNITC" "$@" </dev/null >/dev/null 2>&1; then
        fail "unitc failed: $*"
    fi
}

# Same, with a pseudo-terminal on stdin and no configuration file.
run_tty() {
    log=$1
    shift
    : > "$log"
    STUBLOG=$log
    TTY_ARGS=$*
    export STUBLOG TTY_ARGS UNITC
    if ! PATH="$tmp/bin:$PATH" script -qec ttywrap /dev/null \
        </dev/null >/dev/null 2>&1; then
        fail "unitc failed under script(1): $*"
    fi
}

if ! command -v script >/dev/null 2>&1; then
    fail "script(1) is required for the GET and DELETE paths"
fi

# docker://: the option, the socket and the placeholder are three words.
run "$tmp/docker.args" docker://cont/var/run/control.unit.sock /config "$tmp/conf.json"
assert_arg --unix-socket "$tmp/docker.args"
assert_arg /var/run/control.unit.sock "$tmp/docker.args"
assert_arg _/config "$tmp/docker.args"
assert_no_arg "--unix-socket /var/run/control.unit.sock _/config" "$tmp/docker.args"

# ssh://: same.
run "$tmp/ssh.args" ssh://host/var/run/control.unit.sock /config "$tmp/conf.json"
assert_arg --unix-socket "$tmp/ssh.args"
assert_arg /var/run/control.unit.sock "$tmp/ssh.args"
assert_arg _/config "$tmp/ssh.args"
assert_no_arg "--unix-socket /var/run/control.unit.sock _/config" "$tmp/ssh.args"

# http://: one URL.
run "$tmp/http.args" http://127.0.0.1:1/config "$tmp/conf.json"
assert_arg http://127.0.0.1:1/config "$tmp/http.args"

# Local control socket: the cached UNIT_CTRL holds the option form.
run "$tmp/local.args" /config "$tmp/conf.json"
assert_arg --unix-socket "$tmp/local.args"
assert_arg /tmp/unitc-argv.sock "$tmp/local.args"
assert_arg _/config "$tmp/local.args"
assert_no_arg "--unix-socket /tmp/unitc-argv.sock _/config" "$tmp/local.args"

# GET: no file, a terminal on stdin, over docker://.
run_tty "$tmp/get.args" docker://cont/var/run/control.unit.sock /config
assert_arg --unix-socket "$tmp/get.args"
assert_arg /var/run/control.unit.sock "$tmp/get.args"
assert_arg _/config "$tmp/get.args"
assert_no_arg "--unix-socket /var/run/control.unit.sock _/config" "$tmp/get.args"

# DELETE: same, on the local control socket.
run_tty "$tmp/delete.args" DELETE /config
assert_arg DELETE "$tmp/delete.args"
assert_arg --unix-socket "$tmp/delete.args"
assert_arg /tmp/unitc-argv.sock "$tmp/delete.args"
assert_arg _/config "$tmp/delete.args"
assert_no_arg "--unix-socket /tmp/unitc-argv.sock _/config" "$tmp/delete.args"

printf '%s\n' "unitc argv test: OK"
