# shellcheck shell=sh
# Shared smoke-time assertions for the FreeUnit .deb packages. Sourced by both
# the isolated and combined smoke scripts in build-local.sh (bind-mounted at
# /smoke-asserts.sh, mirroring sury-setup.sh). Keeping the assertions in one
# place prevents the two local smoke paths from drifting apart. The CI smoke job
# (.github/workflows/build-deb.yml) sources this file too, so local and CI smoke
# run the identical assertions. Consumes the BRAND / RUNTIME / RUNDIR / VERSION
# env the smoke containers / CI job export, against an already-installed package
# set.
#
# Convention: a broken rebrand (wrong path, unsubstituted %%placeholder%%,
# version mismatch, missing runtime user) is fatal (exit 1); packaging details
# that vary by debhelper version (the SysV /etc/default wiring) are reported as
# WARN and do not fail the smoke.

# Core artifacts must land under the configured RUNTIME/BRAND names, never the
# upstream "unit*" ones.
assert_rebrand() {
    echo "=== rebrand assertions (BRAND=${BRAND} RUNTIME=${RUNTIME}) ==="
    test -x "/usr/sbin/${RUNTIME}d" \
        || { echo "FAIL: /usr/sbin/${RUNTIME}d not installed"; exit 1; }
    ls /usr/lib/"${RUNTIME}"/modules/*.so >/dev/null 2>&1 \
        || { echo "FAIL: no modules under /usr/lib/${RUNTIME}/modules"; exit 1; }
    dpkg -L "${BRAND}" | grep -q "systemd/system/${RUNTIME}\.service" \
        || { echo "FAIL: ${RUNTIME}.service not packaged"; exit 1; }
    if [ "${RUNTIME}" != unit ] && [ -e /usr/sbin/unitd ]; then
        echo "FAIL: stale /usr/sbin/unitd on a RUNTIME=${RUNTIME} build"; exit 1
    fi
    for p in "/etc/init.d/${RUNTIME}" "/etc/default/${RUNTIME}"; do
        dpkg -L "${BRAND}" | grep -qx "$p" \
            || echo "WARN: $p not installed (verify dh_installinit wiring in debian/rules.in)"
    done
    echo "rebrand assertions: core PASS"
}

# No installed packaging file may carry an unsubstituted %%placeholder%% or, on
# a rebranded build, a stray upstream "unit" path — both mean a missed
# substitution somewhere in pkg/deb. Scans the on-disk init/default/systemd/
# logrotate files plus the maintainer scripts dpkg stored under /var/lib/dpkg.
assert_no_residual_brand() {
    echo "=== residual-brand scan ==="
    files="$(dpkg -L "${BRAND}" 2>/dev/null \
        | grep -E '/(init\.d|default|systemd/system|logrotate\.d)/' || true)
/var/lib/dpkg/info/${BRAND}.postinst /var/lib/dpkg/info/${BRAND}.preinst
/var/lib/dpkg/info/${BRAND}.postrm /var/lib/dpkg/info/${BRAND}.prerm"
    # Unquoted on purpose: word-split the newline/space list (these paths never
    # contain whitespace). `set -f` disables pathname expansion so a future path
    # carrying a glob metachar (*, ?) can't silently skip a file and weaken the
    # gate. Runs in the main shell, so a fatal exit terminates the smoke script
    # rather than just a pipeline subshell.
    set -f
    for f in $files; do
        [ -f "$f" ] || continue
        if grep -qE '%%[A-Z_]+%%' "$f"; then
            echo "FAIL: unsubstituted placeholder in $f"
            grep -nE '%%[A-Z_]+%%' "$f"
            exit 1
        fi
        if [ "${RUNTIME}" != unit ] \
           && grep -qE '/usr/sbin/unitd|/usr/lib/unit/|/var/lib/unit\b|/var/log/unit\.log|control\.unit\.sock' "$f"; then
            echo "FAIL: residual upstream 'unit' path in $f"
            grep -nE '/usr/sbin/unitd|/usr/lib/unit/|/var/lib/unit\b|/var/log/unit\.log|control\.unit\.sock' "$f"
            exit 1
        fi
    done
    set +f
    echo "residual-brand scan: PASS"
}

# The installed daemon must report the version the package was stamped with —
# catches a build off a stale tree or a mislabelled .deb. --version writes
# "unit version: <NXT_VERSION>" to stderr and exits 0.
assert_daemon_version() {
    echo "=== daemon version assertion (expect ${VERSION}) ==="
    ver="$(/usr/sbin/"${RUNTIME}"d --version 2>&1 || true)"
    printf '%s\n' "$ver"
    printf '%s' "$ver" | grep -qF "${VERSION}" \
        || { echo "FAIL: ${RUNTIME}d does not report version ${VERSION}"; exit 1; }
    echo "daemon version assertion: PASS"
}

# postinst must have created the unprivileged RUNTIME user and group (the 1.22
# non-root worker model the migration banner warns about).
assert_runtime_user() {
    echo "=== runtime user/group assertion ==="
    getent group "${RUNTIME}" >/dev/null \
        || { echo "FAIL: group ${RUNTIME} not created by postinst"; exit 1; }
    getent passwd "${RUNTIME}" >/dev/null \
        || { echo "FAIL: user ${RUNTIME} not created by postinst"; exit 1; }
    echo "runtime user/group assertion: PASS"
}

run_smoke_asserts() {
    assert_rebrand
    assert_no_residual_brand
    assert_daemon_version
    assert_runtime_user
}

# True once the daemon $1 is no longer a live process: either reaped (kill -0
# fails) or left as a zombie (<defunct>, /proc state Z). The zombie case matters
# under a non-reaping container PID 1 -- GitHub Actions' job containers keep the
# container alive with a `sleep`/`tail` PID 1 that never wait()s, so a daemon
# that has fully exited lingers as <defunct> and kill -0 keeps succeeding even
# though it is gone. A plain `docker run` (the local runner) has the shell as a
# reaping PID 1, so there the process is reaped outright and the first branch
# fires. /proc/$pid/stat is parsed after the last ')' so a comm with spaces or
# parens cannot shift the state field.
shutdown_settled() {
    kill -0 "$1" 2>/dev/null || return 0
    _stat=$(cat "/proc/$1/stat" 2>/dev/null) || return 0
    _state=${_stat##*) }
    _state=${_state%% *}
    [ "$_state" = Z ]
}

# Called at the end of a smoke run, after the daemon has served requests: a
# SIGTERM to the main process must shut it down cleanly, which the router
# signals by removing its control socket. Uses the pidfile + kill -0 / /proc
# state (shell builtins + procfs) so it needs no procps in the smoke image.
# Paths follow the .deb configure flags via $RUNDIR (default /var/run, matching
# pkg/deb/Makefile's RUNDIR ?= /var/run): control.$RUNTIME.sock, $RUNTIME.pid.
assert_clean_shutdown() {
    echo "=== clean shutdown (SIGTERM) assertion ==="
    sock="${RUNDIR:-/var/run}/control.${RUNTIME}.sock"
    pidfile="${RUNDIR:-/var/run}/${RUNTIME}.pid"
    pid=""
    [ -f "$pidfile" ] && pid="$(cat "$pidfile" 2>/dev/null)"
    if [ -z "$pid" ]; then
        echo "WARN: no pidfile at $pidfile; cannot send SIGTERM precisely"
        return 0
    fi
    kill -TERM "$pid" 2>/dev/null || true
    # Wait on the process state, not the control socket: the router removes the
    # socket early in shutdown, so socket-absence races ahead of the daemon
    # actually exiting. shutdown_settled treats both reaped and <defunct> as
    # done -- see its comment for why a zombie counts as a clean shutdown here.
    for _ in $(seq 1 30); do
        shutdown_settled "$pid" && break
        sleep 0.5
    done
    if ! shutdown_settled "$pid"; then
        echo "FAIL: ${RUNTIME}d (pid $pid) still running after SIGTERM"; exit 1
    fi
    [ -S "$sock" ] \
        && { echo "FAIL: control socket $sock still present after SIGTERM"; exit 1; }
    echo "clean shutdown assertion: PASS"
}

# Asserts that an HTTP response from <url> carries a "Server: ... Unit" header,
# confirming the freeunit daemon (NXT_NAME "Unit") served the response rather
# than some other process. <url> must already be serving (no retry loop here).
# Usage: assert_server_header <url> [label]
assert_server_header() {
    _url="$1"
    _label="${2:-${_url}}"
    _hdr=$(curl -fsSI "$_url" 2>/dev/null || true)
    printf '%s' "$_hdr" | grep -qiE '^Server:.*Unit' \
        || { echo "FAIL ${_label}: response missing Server: Unit header"; printf '%s\n' "$_hdr"; exit 1; }
}

# Asserts that GET /config on the control socket echoes back a given listener
# string (e.g. "*:8084"), confirming the controller accepted and retained the
# applied config rather than silently dropping it.
# Usage: assert_listener_echoed <listener_pattern>  (grep -q pattern; shell-escaped as needed)
assert_listener_echoed() {
    _want="$1"
    _cfg=$(curl -fsS --unix-socket "${RUNDIR:-/var/run}/control.${RUNTIME}.sock" http://localhost/config)
    printf '%s' "$_cfg" | grep -q "$_want" \
        || { echo "FAIL: GET /config did not echo listener ${_want}"; printf '%s\n' "$_cfg"; exit 1; }
}
