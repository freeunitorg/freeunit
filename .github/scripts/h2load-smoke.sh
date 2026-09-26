#!/bin/sh
#
# HTTP/2 load smoke test: start the unitd of this tree with an HTTP/2 TLS
# listener that serves a static file, run h2load against it, and fail unless
# every request got a 2xx response and unit.log has no alert and no sanitizer
# report.
#
# Needs a build configured with --openssl --h2, and openssl, curl and h2load
# (Debian/Ubuntu: nghttp2-client) on PATH.  Run it as root, like the test
# suite: unitd starts its processes as "nobody".
#
# Usage: h2load-smoke.sh [requests] [clients] [streams]
#   defaults: 2000 requests, 8 clients, 16 streams per client

set -eu

requests=${1:-2000}
clients=${2:-8}
streams=${3:-16}

port=${H2LOAD_SMOKE_PORT:-8443}
unitd=${UNITD:-build/sbin/unitd}

dir=$(mktemp -d "${TMPDIR:-/tmp}/h2load-smoke.XXXXXX")
chmod 755 "$dir"

sock="$dir/control.sock"
pid_file="$dir/unit.pid"
log="$dir/unit.log"

stop() {
    if [ -f "$pid_file" ]; then
        kill -QUIT "$(cat "$pid_file")" 2>/dev/null || true

        i=0
        while [ -f "$pid_file" ] && [ $i -lt 100 ]; do
            sleep 0.1
            i=$((i + 1))
        done
    fi
}

trap stop EXIT

mkdir -p "$dir/state" "$dir/share"
head -c 4096 /dev/urandom | od -An -tx1 > "$dir/share/index.html"
chmod -R a+rX "$dir/share"

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
    -keyout "$dir/key.pem" -out "$dir/cert.pem" 2>/dev/null
cat "$dir/cert.pem" "$dir/key.pem" > "$dir/bundle.pem"

"$unitd" --control "unix:$sock" --pid "$pid_file" --log "$log" \
    --statedir "$dir/state" --tmpdir "$dir"

i=0
until [ -S "$sock" ]; do
    i=$((i + 1))

    if [ $i -gt 100 ]; then
        echo "unitd did not start:" >&2
        cat "$log" >&2
        exit 1
    fi

    sleep 0.1
done

curl -fsS --unix-socket "$sock" -X PUT --data-binary "@$dir/bundle.pem" \
    http://localhost/certificates/default

curl -fsS --unix-socket "$sock" -X PUT --data-binary @- \
    http://localhost/config <<EOF
{
    "listeners": {
        "127.0.0.1:$port": {
            "pass": "routes",
            "tls": {"certificate": "default", "http2": true}
        }
    },
    "routes": [{"action": {"share": "$dir/share\$uri"}}]
}
EOF

h2load -n "$requests" -c "$clients" -m "$streams" \
    "https://127.0.0.1:$port/index.html" | tee "$dir/h2load.out"

stop
trap - EXIT

status=0

if ! grep -Eq "^requests: $requests total, $requests started, $requests done, $requests succeeded, 0 failed, 0 errored" \
        "$dir/h2load.out"
then
    echo "h2load: not every request succeeded" >&2
    status=1
fi

if ! grep -Eq "^status codes: $requests 2xx, 0 3xx, 0 4xx, 0 5xx" \
        "$dir/h2load.out"
then
    echo "h2load: not every response was 2xx" >&2
    status=1
fi

if grep -Eq '\[alert\]|Sanitizer|runtime error:' "$log"; then
    echo "unit.log has an alert or a sanitizer report:" >&2
    grep -E '\[alert\]|Sanitizer|runtime error:' "$log" >&2
    status=1
fi

if [ $status -ne 0 ]; then
    cat "$log" >&2
fi

exit $status
