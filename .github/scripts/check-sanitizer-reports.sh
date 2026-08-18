#!/bin/sh
#
# Fail a sanitizer CI leg on any ASan/UBSan report the test session produced.
#
# Why this scans the harness's unit.log rather than a sanitizer log_path
# directory: ASAN_OPTIONS/UBSAN_OPTIONS log_path does not work for unitd.
# The daemon relocates argv[]/environ[] in nxt_process_title()
# (src/nxt_process_title.c) and overwrites the original block, so by the time a
# forked router or application worker writes a report the options string the
# sanitizer runtime kept a pointer into is gone.  Measured on this tree: an
# ordinary binary run with the same UBSAN_OPTIONS honours log_path,
# halt_on_error and print_stacktrace; inside unitd none of the three take
# effect and the log directory stays empty on every run -- including runs that
# produced a genuine heap-use-after-free.  Reports do reach unitd's stderr,
# which test/conftest.py wires to <temp_dir>/unit.log, so that is what we read.
#
# The caller must pass --save-log to pytest: without it conftest.py removes the
# per-restart temp dir (and with it unit.log) after every test.
#
# Usage: check-sanitizer-reports.sh <log-root> [allowlist-file]
#   <log-root>       directory holding the harness's unit-test-* temp dirs
#                    (tempfile.mkdtemp(prefix='unit-test-'), i.e. $TMPDIR or /tmp)
#   [allowlist-file] optional; see .github/sanitizer-allowlist.txt

set -eu

log_root=${1:?usage: check-sanitizer-reports.sh <log-root> [allowlist-file]}
allowlist=${2:-}

# A sanitizer report line, anchored tightly enough that a request body echoed
# into the --debug log cannot forge one.
#   UBSan: "src/nxt_conf.c:664:26: runtime error: ..."
#   ASan:  "==123==ERROR: AddressSanitizer: heap-use-after-free ..."
ubsan_re='[^[:space:]]+:[0-9]+:[0-9]+: runtime error: '
asan_re='(ERROR|SUMMARY): [A-Za-z]*Sanitizer:'

logs=$(find "$log_root" -maxdepth 2 -name unit.log -type f 2>/dev/null | sort)

if [ -z "$logs" ]; then
    echo "::error::No unit.log found under ${log_root}. The sanitizer gate" \
         "cannot certify this leg: either the test session never started, or" \
         "pytest was invoked without --save-log and the harness removed the" \
         "temp dirs.  Refusing to report a clean run."
    exit 1
fi

echo "Scanning $(echo "$logs" | wc -l) unit.log file(s) under ${log_root}:"
echo "$logs" | sed 's/^/  /'
echo

findings=$(mktemp)
patterns=$(mktemp)
trap 'rm -f "$findings" "$patterns"' EXIT

# -a is load-bearing, not defensive.  unitd's --debug log carries raw payload
# bytes, tens of thousands of NULs in a typical run, so grep classifies
# unit.log as binary: it prints the matches it found before the first NUL and
# then silently stops.  Measured on a churn log: 3 of 8 report lines survived,
# and the two AddressSanitizer lines -- which come later in the file than the
# first NUL -- were among the 5 dropped.  Without -a this gate reproduces the
# exact bug it was written to remove.
for f in $logs; do
    grep -aEH "$ubsan_re|$asan_re" "$f" 2>/dev/null || true
done > "$findings"

if [ ! -s "$findings" ]; then
    echo "No sanitizer reports."
    exit 0
fi

# An ASan/LSan/TSan report is never allowlistable -- those are memory-safety
# bugs, not style debt.  Only UBSan lines can be waived, and only by an entry
# that names an open issue.
hard=$(grep -aE "$asan_re" "$findings" || true)

soft=$(grep -aEv "$asan_re" "$findings" || true)
known=''
unknown=$soft

if [ -n "$allowlist" ] && [ -f "$allowlist" ] && [ -n "$soft" ]; then
    # Patterns go through a file, not a heredoc: `grep -f -` would take its
    # pattern list from the same stdin the findings are piped in on, match
    # nothing, and report a clean leg -- the exact failure this gate exists to
    # remove.
    grep -Ev '^[[:space:]]*(#|$)' "$allowlist" > "$patterns" || true

    if [ -s "$patterns" ]; then
        known=$(printf '%s\n' "$soft" | grep -aE -f "$patterns" || true)
        unknown=$(printf '%s\n' "$soft" | grep -aE -v -f "$patterns" || true)
    fi
fi

if [ -n "$known" ]; then
    echo "Known UBSan violations waived by ${allowlist} (tracked, not fixed here):"
    printf '%s\n' "$known" | sed 's/.*unit\.log://' | sort | uniq -c | sort -rn |
        sed 's/^/  /'
    echo
fi

if [ -z "$hard" ] && [ -z "$unknown" ]; then
    echo "No unwaived sanitizer reports."
    exit 0
fi

if [ -n "$hard" ]; then
    echo "AddressSanitizer/LeakSanitizer report(s) -- never waivable:"
    printf '%s\n' "$hard" | sed 's/^/  /'
    echo
    echo "Stack frames from the same log(s):"
    for f in $logs; do
        grep -aE '^ *#[0-9]+ 0x' "$f" 2>/dev/null | head -40 | sed 's/^/  /'
    done
    echo
fi

if [ -n "$unknown" ]; then
    echo "UBSan violation(s) not in ${allowlist:-(no allowlist)}:"
    printf '%s\n' "$unknown" | sed 's/.*unit\.log://' | sort | uniq -c | sort -rn |
        sed 's/^/  /'
    echo
fi

echo "::error::Sanitizer report(s) in this leg; see the step output above and" \
     "the uploaded unit-logs artifact.  A new UBSan site must be fixed, or" \
     "waived in .github/sanitizer-allowlist.txt with an issue reference."
exit 1
