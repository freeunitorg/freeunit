#!/bin/sh
# entrypoint.sh — builds unit-eol-check from source then execs it

set -eu

cd /repo/pkg/eol

cargo build --release
rm -f /usr/local/bin/unit-eol-check
mv target/release/unit-eol-check /usr/local/bin/
rm -rf target

exec /usr/local/bin/unit-eol-check --json /repo/pkg/eol.json "$@"