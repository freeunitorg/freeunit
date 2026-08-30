#!/bin/sh
#
# Capture the set of syscalls unitd issues while starting up, accepting a
# configuration and serving one request, and print it as a sorted, unique list
# of syscall names -- one per line.
#
# The list is the input to the CI drift gate (test/syscalls/check.sh); see
# test/syscalls/README.md for the whole story, including why the exercised
# configuration deliberately does not start a language application.
#
# Usage:
#   test/syscalls/capture.sh [-u UNITD] [-p PORT] [-o OUTFILE]
#
#   -u  path to the unitd binary          (default: build/sbin/unitd)
#   -p  TCP port for the test listener    (default: 8099)
#   -o  write the list here instead of stdout
#
# Requires: strace, curl.  Run it from the top of a built source tree.

set -eu

UNITD=build/sbin/unitd
PORT=8099
OUT=

while getopts 'u:p:o:' opt; do
    case "$opt" in
        u) UNITD=$OPTARG ;;
        p) PORT=$OPTARG ;;
        o) OUT=$OPTARG ;;
        *) echo "usage: $0 [-u UNITD] [-p PORT] [-o OUTFILE]" >&2; exit 2 ;;
    esac
done

die() { echo "capture.sh: $*" >&2; exit 1; }

command -v strace >/dev/null 2>&1 || die "strace not found"
command -v curl   >/dev/null 2>&1 || die "curl not found"
[ -x "$UNITD" ]                   || die "no unitd binary at $UNITD"

WORK=$(mktemp -d)
STRACE_PID=

# strace sets SIGINT/SIGQUIT/SIGTERM/SIGHUP to SIG_IGN in itself so that a
# terminal signal reaches the tracee instead, which means the daemon has to be
# signalled directly -- signalling strace is a no-op that hangs the script.
# unitd writes its main pid to --pid, so read it from there.
unit_pid() {
    [ -s "$WORK/unit.pid" ] && cat "$WORK/unit.pid"
}

# Every kill here is best-effort: by the time cleanup runs the daemon has
# usually exited already, and under `set -e` a failing kill would abort the
# EXIT trap before the rm and leak the temporary directory.
cleanup() {
    up=$(unit_pid || :)
    [ -n "$up" ] && { kill -QUIT "$up" 2>/dev/null || :; }
    i=0
    while [ "$i" -lt 50 ] \
        && [ -n "$STRACE_PID" ] && kill -0 "$STRACE_PID" 2>/dev/null; do
        i=$((i + 1))
        sleep 0.1
    done
    [ -n "$up" ] && { kill -KILL "$up" 2>/dev/null || :; }
    # SIGKILL on strace leaves its tracees behind by default -- hence
    # --kill-on-exit below, so the controller and router go with it and do not
    # sit on the test port.
    [ -n "$STRACE_PID" ] && { kill -KILL "$STRACE_PID" 2>/dev/null || :; }
    rm -rf "$WORK"
    return 0
}
# A trapped signal does not end a POSIX shell, so each signal handler exits
# explicitly; the EXIT trap is cleared first so the cleanup runs once.
trap cleanup EXIT
trap 'trap - EXIT; cleanup; exit 130' INT
trap 'trap - EXIT; cleanup; exit 143' TERM

mkdir -p "$WORK/state" "$WORK/tmp" "$WORK/modules" "$WORK/share"
echo 'syscall drift canary' > "$WORK/share/index.txt"

# Started as root, unitd drops the router to the configured unprivileged user
# (that is the point: setgid and setuid land in the trace).  mktemp -d makes
# the directory 0700, so the router would then get EACCES and answer 403.
chmod 711 "$WORK"
chmod -R a+rX "$WORK/share"

SOCK=$WORK/control.sock
TRACE=$WORK/trace.log

cat > "$WORK/config.json" <<CFG
{
    "listeners": {
        "127.0.0.1:$PORT": {
            "pass": "routes"
        }
    },
    "routes": [
        {
            "action": {
                "share": "$WORK/share\$uri"
            }
        }
    ]
}
CFG

# -f follows forks: unitd's main process, controller, router and the discovery
# child each get traced.  -qq drops the attach/detach chatter, which would
# otherwise have to be filtered out again below.  --kill-on-exit means a
# strace that is killed takes the whole Unit process family with it instead of
# detaching and leaving the router holding the test port.
strace --kill-on-exit -f -qq -e trace=all -o "$TRACE" \
    "$UNITD" --no-daemon \
        --control "unix:$SOCK" \
        --pid "$WORK/unit.pid" \
        --log "$WORK/unit.log" \
        --statedir "$WORK/state" \
        --tmpdir "$WORK/tmp" \
        --modulesdir "$WORK/modules" \
    >"$WORK/stdout.log" 2>&1 &
STRACE_PID=$!

# Wait for the control socket rather than sleeping a fixed amount: a slow
# runner must not silently produce a short trace.
i=0
while [ ! -S "$SOCK" ]; do
    i=$((i + 1))
    [ "$i" -lt 300 ] || {
        cat "$WORK/stdout.log" >&2 || :
        die "control socket did not appear within 30s"
    }
    kill -0 "$STRACE_PID" 2>/dev/null || {
        cat "$WORK/stdout.log" >&2 || :
        die "unitd exited before the control socket appeared"
    }
    sleep 0.1
done

curl -sS --fail --max-time 10 --unix-socket "$SOCK" \
    -X PUT --data-binary "@$WORK/config.json" \
    http://localhost/config >"$WORK/put.log" 2>&1 \
    || { cat "$WORK/put.log" >&2; die "configuration was rejected"; }

# Poll the listener: PUT returns once the configuration is accepted, which is
# not the same instant the router has the socket bound.
i=0
while :; do
    code=$(curl -sS --max-time 5 -o "$WORK/body.txt" -w '%{http_code}' \
        "http://127.0.0.1:$PORT/index.txt" 2>>"$WORK/get.log") && break
    i=$((i + 1))
    [ "$i" -lt 100 ] || { cat "$WORK/get.log" >&2; die "listener never answered"; }
    sleep 0.1
done

[ "$code" = "200" ] || { cat "$WORK/body.txt" >&2; die "expected HTTP 200, got $code"; }
grep -q 'syscall drift canary' "$WORK/body.txt" \
    || die "served body is not the canary file"

# Stop the daemon and let it run its teardown path *inside* the trace, so
# shutdown syscalls are part of the captured set.  Signal unitd, not strace
# (see unit_pid above), then wait for strace itself to reap and flush.
UPID=$(unit_pid) || die "unitd never wrote a pid file"
kill -QUIT "$UPID"
i=0
while kill -0 "$STRACE_PID" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -lt 200 ] || die "unitd did not exit within 20s of SIGQUIT"
    sleep 0.1
done
# strace exits with the tracee's status, so this is unitd's exit code from its
# SIGQUIT teardown.  Ignoring it would let a daemon that crashes on shutdown
# still produce a large, plausible capture -- and every syscall its teardown
# never reached would only be a warning.
src=0
wait "$STRACE_PID" || src=$?
STRACE_PID=
[ "$src" -eq 0 ] || {
    tail -20 "$WORK/unit.log" >&2 2>/dev/null || :
    die "unitd exited $src from its SIGQUIT teardown"
}

[ -s "$TRACE" ] || die "strace produced an empty trace"

# Two shapes carry a syscall name: a normal entry line, and the resumption of
# a call that was interrupted by another thread's output.
#   1234  epoll_pwait(4, ...) = 1
#   1234  <... epoll_pwait resumed>[...], 1024, ...) = 1
# Lines for signals (--- SIGCHLD ---) and exits (+++ exited +++) match neither.
SYSCALLS=$(sed -e 's/^[0-9][0-9]*[ \t][ \t]*//' "$TRACE" \
    | sed -n \
        -e 's/^\([a-zA-Z_][a-zA-Z0-9_]*\)(.*/\1/p' \
        -e 's/^<\.\.\. \([a-zA-Z_][a-zA-Z0-9_]*\) resumed.*/\1/p' \
    | LC_ALL=C sort -u)

# A missing or renamed tool piped into sort -u yields an empty list, and an
# empty list compares clean against any baseline.  Refuse to emit one.
COUNT=$(printf '%s\n' "$SYSCALLS" | grep -c '^[a-z]' || :)
[ "$COUNT" -ge 20 ] || die "only $COUNT syscalls extracted -- the trace did not parse"

for required in epoll_ctl socket bind listen read write close; do
    printf '%s\n' "$SYSCALLS" | grep -qx "$required" \
        || die "expected syscall '$required' is missing -- the trace did not parse"
done

if [ -n "$OUT" ]; then
    printf '%s\n' "$SYSCALLS" > "$OUT"
    echo "capture.sh: wrote $COUNT syscalls to $OUT" >&2
else
    printf '%s\n' "$SYSCALLS"
fi
