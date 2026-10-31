#!/bin/sh
# Time the build and a test subset on the current platform.
# Usage: measure-arch.sh <label>   (run as root, from the repository root)
set -u
label=$1
out=measure-$label.tsv
: > "$out"
nproc=$(nproc)

stage() {
    name=$1; shift
    t0=$(date +%s)
    "$@" > "log-$label-$name.txt" 2>&1
    rc=$?
    t1=$(date +%s)
    printf '%s\t%s\t%s\t%s\t%s\n' "$label" "$name" "$((t1 - t0))" "$rc" "$nproc" >> "$out"
    tail -n 5 "log-$label-$name.txt"
    return 0
}

if command -v apk > /dev/null; then
    stage deps sh -c 'apk add -q gcc make musl-dev openssl-dev pcre2-dev \
        linux-headers python3-dev py3-pytest py3-openssl curl procps'
else
    stage deps sh -c 'apt-get -qq update && apt-get -qq -y install build-essential \
        libssl-dev libpcre2-dev python3-dev python3-pytest python3-openssl curl'
fi

uname -m
stage configure ./configure --openssl --tests
stage make make -j"$nproc"
stage make-tests make -j"$nproc" tests
stage c-tests ./build/tests
stage configure-python ./configure python --config=python3-config
stage make-python make -j"$nproc"
stage pytest-ws python3 -m pytest -q -p no:cacheprovider \
    test/test_asgi_websockets.py test/test_python_shm_limit.py
stage pytest-ws-1cpu taskset -c 0 python3 -m pytest -q -p no:cacheprovider \
    test/test_asgi_websockets.py

cat "$out"
