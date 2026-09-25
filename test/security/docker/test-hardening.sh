#!/bin/sh
#
# What the FreeUnit image can and cannot do under each common Docker
# hardening posture.  Every cell starts the same image with the same trivial
# Python application and asserts the documented outcome -- a served 200, or a
# refusal at a named point.
#
# This is documentation that runs.  It is not a claim that any of these
# postures is required, and it deliberately does not claim that Docker's
# default seccomp profile blocks capget or mount: measured against docker
# 29.7.2 it blocks neither (capget is allowed by name; mount is gated on
# CAP_SYS_ADMIN and AppArmor).  Every cell below therefore runs with the
# default seccomp profile, unchanged; the profile FreeUnit does ship
# (pkg/docker/seccomp-no-af-alg.json) has its own test next door in
# test/security/seccomp/.
#
# Usage:
#   ./test/security/docker/test-hardening.sh [IMAGE]
#
# IMAGE defaults to the published python-slim flavour.  Requires docker and
# curl on the host.

set -eu

IMAGE="${1:-ghcr.io/freeunitorg/freeunit:latest-python-3.13-slim}"
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/app"
PORT="${UNIT_HARDENING_PORT:-8099}"
# Pinned rather than left to the daemon's ambient default, so a multi-arch tag
# cannot silently resolve to something other than what CI runs.  Override it
# on an arm64 host, where the arm64 manifest is the artifact worth testing:
#   UNIT_HARDENING_PLATFORM=linux/arm64 ./test-hardening.sh
PLATFORM="${UNIT_HARDENING_PLATFORM:-linux/amd64}"

PASS=0
FAIL=0
NAME=
RESULT=

die() { echo "test-hardening.sh: $*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker not found"
command -v curl   >/dev/null 2>&1 || die "curl not found"

LOGDIR=$(mktemp -d)

cleanup() {
    [ -n "$NAME" ] && { docker rm -f "$NAME" >/dev/null 2>&1 || :; }
    rm -rf "$LOGDIR"
    return 0
}

# A trapped signal does not end a POSIX shell: without an explicit exit the
# script would resume, with LOGDIR already deleted underneath it, and report
# an ordinary test error instead of stopping.  The EXIT trap is cleared first
# so cleanup runs once.
trap cleanup EXIT
trap 'trap - EXIT; cleanup; exit 130' INT
trap 'trap - EXIT; cleanup; exit 143' TERM

# Start the image with the given docker flags, wait for the application, and
# set RESULT to "200", "no-200" or "no-start".  It sets a variable rather than
# echoing, because a $(command substitution) would run it in a subshell: NAME
# would never reach the parent, and the EXIT/INT/TERM trap could not remove a
# container left behind by a Ctrl-C -- leaking both the container and the
# published port.  Never fails the script itself: a cell that is *expected*
# to be refused has to be able to report the refusal.
attempt() {
    NAME="unit-hardening-$$"
    docker rm -f "$NAME" >/dev/null 2>&1 || :

    # No --rm: a container that exits on its own would delete itself along
    # with the logs, and the logs are the whole point of a refused cell.
    # shellcheck disable=SC2086  # $1 is a deliberate list of docker flags
    docker run -d --name "$NAME" --platform "$PLATFORM" \
        -v "$APP:/www:ro" \
        -v "$APP/config.json:/docker-entrypoint.d/config.json:ro" \
        -p "127.0.0.1:$PORT:8080" \
        $1 "$IMAGE" >/dev/null 2>"$LOGDIR/start.err" || {
            cp "$LOGDIR/start.err" "$LOGDIR/container.log"
            # docker run can fail *after* creating the container -- a port
            # already allocated, for one -- leaving it in Created state.  The
            # trap cannot reach it once NAME is cleared, and the next run
            # picks a different pid-based name, so remove it here.
            docker rm -f "$NAME" >/dev/null 2>&1 || :
            NAME=
            RESULT=no-start
            return 0
        }

    # docker-entrypoint.sh runs a *temporary* daemon to apply the initial
    # configuration -- which activates this listener -- and only then execs
    # the real one.  Polling straight away can therefore get a 200 out of the
    # throwaway daemon and pass while the production startup is broken.  Wait
    # for the entrypoint to say it is done first.
    # Absolute deadlines, not iteration counts: with a per-request timeout an
    # iteration bound is a bound on attempts, not on time, and a listener that
    # accepts and never answers would spend a hundred five-second timeouts
    # here -- eight minutes in one cell, against a twenty-minute job.
    ready=no
    deadline=$(( $(date +%s) + 45 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if docker logs "$NAME" 2>&1 | grep -q 'ready for start up'; then
            ready=yes
            break
        fi
        docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null \
            | grep -q true || break
        sleep 0.2
    done

    # Never probe an unready container.  Exhausting the loop while it is still
    # running means the entrypoint stalled after applying the configuration --
    # with the temporary daemon still answering, so a probe would take a 200
    # from it and pass the very failure this wait exists to catch.
    if [ "$ready" = no ]; then
        docker logs "$NAME" > "$LOGDIR/container.log" 2>&1 || :
        running=$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null || echo false)
        docker rm -f "$NAME" >/dev/null 2>&1 || :
        NAME=
        if [ "$running" = true ]; then
            RESULT=no-ready
        else
            RESULT=no-200
        fi
        return 0
    fi

    deadline=$(( $(date +%s) + 30 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        # --max-time as well as the deadline: without it one wedged request
        # blocks past the deadline the loop can only check between attempts.
        code=$(curl -s --max-time 5 -o "$LOGDIR/body.txt" -w '%{http_code}' \
            "http://127.0.0.1:$PORT/" 2>/dev/null || echo 000)
        if [ "$code" = "200" ] && grep -q 'docker hardening canary' "$LOGDIR/body.txt"; then
            docker logs "$NAME" > "$LOGDIR/container.log" 2>&1 || :
            # The canary reports the credentials of the process that served
            # the request, so a cell can assert who answered rather than
            # trusting the flags in its own label.
            sed 's/^/response: /' "$LOGDIR/body.txt" >> "$LOGDIR/container.log"
            # ...and the router's own credentials, which the canary cannot
            # speak for: a router still running as root would serve a
            # correctly-dropped application just fine.  Read from /proc inside
            # the container rather than `docker top`, which reports host-side
            # uids -- under daemon user-namespace remapping those are the
            # remapped ones and would fail every cell.  The image has no ps.
            #
            # Match on the *start* of cmdline, not a substring of it: this
            # shell's own cmdline contains the pattern it is searching for,
            # and in a non-root cell it runs as 1000 -- so a substring match
            # would report the right answer no matter what the router did.
            docker exec "$NAME" sh -c '
                for d in /proc/[0-9]*; do
                    [ "$d" = "/proc/$$" ] && continue
                    c=$(tr "\0" " " < "$d/cmdline" 2>/dev/null) || continue
                    case "$c" in "unit: router"*) ;; *) continue ;; esac
                    awk "/^Uid:/ {u=\$2} /^Gid:/ {g=\$2}
                         END {print \"router uid=\" u \" gid=\" g}" \
                        "$d/status"
                done' 2>/dev/null \
                | sed 's/^/proc: /' >> "$LOGDIR/container.log" || :
            docker rm -f "$NAME" >/dev/null 2>&1 || :
            NAME=
            RESULT=200
            return 0
        fi
        # A container that has already exited will never answer.
        docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null \
            | grep -q true || break
        sleep 0.2
    done

    docker logs "$NAME" > "$LOGDIR/container.log" 2>&1 || :
    docker rm -f "$NAME" >/dev/null 2>&1 || :
    NAME=
    RESULT=no-200
}

# cell DESCRIPTION 200|refused FLAGS [LOG-SUBSTRING]
#
# LOG-SUBSTRING is how a cell is pinned to its cause.  "no-200" on its own is
# satisfied by a wholly broken image, so a cell documenting a refusal has to
# say which refusal; and a 200 says nothing about who served it, so every
# serving cell matches on the credentials the canary reports.  Both the
# container log and the response body are searched.
cell() {
    desc=$1
    expect=$2
    flags=$3
    want_log=${4:-}

    RESULT=
    attempt "$flags"
    got=$RESULT

    # "refused" is any of the ways a cell can fail to serve: the container
    # never started, never reached readiness, or never answered.  Which one a
    # broken posture produces is not stable -- --cap-drop=ALL leaves the
    # entrypoint's daemon alive but never ready -- and the cause is pinned by
    # the log assertion below, not by which shade of failure it was.
    case "$expect" in
        refused)
            case "$got" in
                no-start|no-ready|no-200) got=refused ;;
            esac
            ;;
    esac

    if [ "$got" != "$expect" ]; then
        echo "FAIL: $desc -> $got, expected $expect"
        [ -f "$LOGDIR/container.log" ] && sed 's/^/    | /' "$LOGDIR/container.log"
        FAIL=$((FAIL + 1))
        return 0
    fi

    # want_log holds one pattern per line and every one has to match.
    if [ -n "$want_log" ]; then
        oldifs=$IFS
        IFS='
'
        for pat in $want_log; do
            IFS=$oldifs
            if ! grep -q "$pat" "$LOGDIR/container.log"; then
                echo "FAIL: $desc -> $got as expected, but the container log" \
                     "does not match \"$pat\""
                [ -f "$LOGDIR/container.log" ] \
                    && sed 's/^/    | /' "$LOGDIR/container.log"
                FAIL=$((FAIL + 1))
                return 0
            fi
            IFS='
'
        done
        IFS=$oldifs
    fi

    echo "PASS: $desc -> $got (as documented)"
    PASS=$((PASS + 1))
}

echo "Image:    $IMAGE"
echo "Platform: $PLATFORM"
echo "Every cell runs with Docker's DEFAULT seccomp profile."
echo ""

# Started as root, the daemon drops the router and applications to the image's
# unprivileged "unit" user (uid 999).  The canary reports what it actually ran
# as, so these cells assert the drop happened rather than assuming it.
# Two patterns, both anchored -- an unanchored "gid=999" also matches gid=9990.
# The first is the application's own view of itself, including supplementary
# groups: dropping the primary gid while keeping group 0 is root-group access
# under another name.  The second is the router, which the application cannot
# speak for.
ROOT_ID="uid=999 gid=999 groups=999$
^proc: router uid=999 gid=999$"

# The shipped default: root in the container, full default capability set.
cell "root, default caps" 200 "" "$ROOT_ID"

# no-new-privileges does not stop Unit dropping privileges: setuid/setgid from
# root is not a privilege *gain*, so the bit never applies to it.
cell "root, --security-opt no-new-privileges" 200 \
    "--security-opt no-new-privileges" "$ROOT_ID"

# Root without capabilities cannot become the unprivileged 'unit' user, which
# is the first thing the daemon does:
#   [alert] setgid(999) failed (1: Operation not permitted)
# CAP_SETGID and CAP_SETUID are what --cap-drop=ALL takes away here.  Dropping
# capabilities is only useful once the process is already non-root, which is
# the cell below.  Documented as a refusal, not a bug.
cell "root, --cap-drop=ALL" refused "--cap-drop=ALL" \
    "setgid(999) failed"

# The non-root recipe.  1000:1000, not a bare 1000: uid 1000 has no passwd
# entry in this image, so `--user 1000` resolves the primary group to GID 0 --
#
#   $ docker run --rm --user 1000 --entrypoint sh IMAGE -c id
#   uid=1000 gid=0(root) groups=0(root)
#
# -- and the container keeps root-group access, which is not what anyone means
# by "runs unprivileged".
#
# Nothing to drop to means nothing needs CAP_SETUID -- but the state, run and
# tmp directories are baked into the image root-owned, so they have to be
# replaced by writable mounts.  A bare "--tmpfs /var/lib/unit" is not enough:
# a tmpfs is created root-owned 0755 and the daemon still gets EACCES, so the
# ownership has to be spelled out.  This is the part of the non-root recipe
# people get wrong.
#
# One variable for all three cells, so a flag list can never drift from the
# label describing it -- which is exactly what happened while writing this.
# Each cell also asserts "uid=1000 gid=1000" from the canary's own
# os.getuid()/os.getgid(), so the identity of the process that served the
# request is measured rather than assumed.
NONROOT="--user 1000:1000"
NONROOT="$NONROOT --tmpfs /var/lib/unit:uid=1000,gid=1000"
NONROOT="$NONROOT --tmpfs /var/run:uid=1000,gid=1000"
NONROOT="$NONROOT --tmpfs /var/tmp:mode=1777"
NONROOT_ID="uid=1000 gid=1000 groups=1000$
^proc: router uid=1000 gid=1000$"

cell "--user 1000:1000, tmpfs state/run/tmp" 200 \
    "$NONROOT" "$NONROOT_ID"

# The same, with every capability removed.  Nothing above needed one.
cell "--user 1000:1000, --cap-drop=ALL, tmpfs" 200 \
    "$NONROOT --cap-drop=ALL" "$NONROOT_ID"

# And with no-new-privileges on top: the posture to recommend.
cell "--user 1000:1000, --cap-drop=ALL, no-new-privileges, tmpfs" 200 \
    "$NONROOT --cap-drop=ALL --security-opt no-new-privileges" "$NONROOT_ID"

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
