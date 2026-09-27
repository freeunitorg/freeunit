#!/bin/sh
#
# Copyright (C) NGINX, Inc.
#
# Run queue_bench for every queue, under `perf stat` when hardware counters
# work, otherwise with its own clock_gettime() timings.
#
#   tools/perf/bench-queues.sh [build-dir] [-n ops]

set -eu

BENCH="${1:-build}/queue_bench"
[ $# -gt 0 ] && shift

if [ ! -x "$BENCH" ]; then
    echo "error: $BENCH not found; ./configure --tests && make tests" >&2
    exit 1
fi

echo "date:    $(date -u +%FT%TZ)"
echo "host:    $(uname -a)"
echo "cpus:    $(nproc)"
echo "loadavg: $(cat /proc/loadavg 2>/dev/null || echo n/a)"
echo

# perf may be installed yet fail, or run without PMU access.
PERF=
probe=$(perf stat -e instructions -- /bin/true 2>&1) || probe="<not supported>"

case "$probe" in
    *"<not supported>"*|*"not counted"*)
        echo "note: no usable perf hardware counters; using queue_bench timings"
        ;;
    *)
        PERF="perf stat -e instructions,cycles,cache-misses,branch-misses --"
        ;;
esac

echo

run() {
    echo "--- $* ---"
    $PERF "$BENCH" "$@" 2>&1
    echo
}

run nncq "$@"
run app_nncq "$@"
run port_queue "$@"
run app_queue "$@"
run port_queue_mt --producers 1 --consumers 1 "$@"
run port_queue_mt --producers 2 --consumers 2 "$@"
