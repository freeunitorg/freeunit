#!/bin/sh
#
# HTTP/2 conformance test: start the unitd of this tree with an HTTP/2 TLS
# listener that serves a static file, run h2spec against it, and fail when a
# test case fails that is not in the exclude list (h2spec-excludes.txt next to
# this script), or when an excluded case passes again.
#
# Needs a build configured with --openssl --h2, and openssl, curl, python3 and
# h2spec (https://github.com/summerwind/h2spec) on PATH.  Run it as root, like
# the test suite: unitd starts its processes as "nobody".
#
# Usage: h2spec.sh [h2spec arguments...]
#   extra arguments go to h2spec, for example "-S" for the strict cases or
#   "http2/6.5" to run one section.

set -eu

port=${H2SPEC_PORT:-8443}
unitd=${UNITD:-build/sbin/unitd}
h2spec=${H2SPEC:-h2spec}
excludes=${H2SPEC_EXCLUDES:-$(dirname "$0")/h2spec-excludes.txt}

dir=$(mktemp -d "${TMPDIR:-/tmp}/h2spec.XXXXXX")
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

# Request bodies go to --tmpdir, and the router writes them as "nobody".
mkdir -p "$dir/state" "$dir/share" "$dir/tmp"
chmod 1777 "$dir/tmp"
echo "h2spec" > "$dir/share/index.html"
chmod -R a+rX "$dir/share"

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
    -keyout "$dir/key.pem" -out "$dir/cert.pem" 2>/dev/null
cat "$dir/cert.pem" "$dir/key.pem" > "$dir/bundle.pem"

"$unitd" --control "unix:$sock" --pid "$pid_file" --log "$log" \
    --statedir "$dir/state" --tmpdir "$dir/tmp"

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

# h2spec sends POST requests with a body to "/" in some cases, so the route
# answers every request with the static file.
curl -fsS --unix-socket "$sock" -X PUT --data-binary @- \
    http://localhost/config <<EOF
{
    "listeners": {
        "127.0.0.1:$port": {
            "pass": "routes",
            "tls": {"certificate": "default", "http2": true}
        }
    },
    "routes": [{"action": {"share": "$dir/share/index.html"}}]
}
EOF

# h2spec exits with 1 when a case fails; the report decides.
"$h2spec" -t -k -h 127.0.0.1 -p "$port" -j "$dir/report.xml" "$@" \
    | tee "$dir/h2spec.out" || true

stop
trap - EXIT

status=0

if [ ! -s "$dir/report.xml" ]; then
    echo "h2spec wrote no report" >&2
    exit 1
fi

python3 - "$dir/report.xml" "$excludes" <<'EOF' || status=1
import re
import sys
import xml.etree.ElementTree as ET

report, excludes = sys.argv[1], sys.argv[2]

# One case per line: "<section id> <case number>  # reason", for example
# "http2/6.9.1 2  # reason".  The id is the h2spec package (http2, hpack,
# generic) and section; the number is the case number inside the section.
excluded = set()
with open(excludes) as f:
    for line in f:
        line = line.split('#', 1)[0].strip()
        if line:
            excluded.add(' '.join(line.split()))

failed, passed, skipped = set(), set(), 0

for suite in ET.parse(report).getroot().iter('testsuite'):
    # The package is like "http2/6.9.1"; the cases are numbered from 1
    # inside it, as h2spec prints them.
    for n, case in enumerate(suite.iter('testcase'), 1):
        key = '%s %d' % (suite.get('package'), n)
        if case.find('skipped') is not None:
            skipped += 1
        elif case.find('failure') is not None or case.find('error') is not None:
            failed.add((key, case.get('classname', '')))
        else:
            passed.add(key)

new = sorted(f for f in failed if f[0] not in excluded)
fixed = sorted(k for k in excluded if k in passed)

print('h2spec: %d passed, %d failed (%d excluded), %d skipped'
      % (len(passed), len(failed), len(failed) - len(new), skipped))

for key, name in new:
    print('h2spec: FAILED %s: %s' % (key, name), file=sys.stderr)

for key in fixed:
    print('h2spec: %s passes now, remove it from %s' % (key, excludes),
          file=sys.stderr)

sys.exit(1 if new or fixed else 0)
EOF

if grep -Eq '\[alert\]|Sanitizer|runtime error:' "$log"; then
    echo "unit.log has an alert or a sanitizer report:" >&2
    grep -E '\[alert\]|Sanitizer|runtime error:' "$log" >&2
    status=1
fi

if [ $status -ne 0 ]; then
    cat "$log" >&2
fi

exit $status
