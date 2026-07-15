# unit-eol-check

Validates `pkg/eol.json` against [endoflife.date](https://endoflife.date) API. Detects wrong dates, missed EOL flags, and new versions not yet in the matrix.

## Usage

```bash
./pkg/eol/run.sh                  # check all (runtimes + OS)
./pkg/eol/run.sh --runtimes       # runtime versions only
./pkg/eol/run.sh --os             # OS versions only
./pkg/eol/run.sh --new            # detect new versions missing from matrix
./pkg/eol/run.sh --ci             # CI mode: JSON output, exit 1 on errors
./pkg/eol/run.sh --fix            # print corrected runtime EOL dates
./pkg/eol/run.sh --quiet          # suppress [ OK ] lines
./pkg/eol/run.sh -n               # dry-run (print commands, no execute)
```

First run builds `freeunit-eol-check:latest` image (~30s). Subsequent runs use cached image (~5s compile + check).

Force rebuild: `docker rmi freeunit-eol-check:latest`

## Output

```
[ ERROR ] perl: perl 5.40 matrix EOL 2028-07 != actual 2027-06
[ WARN  ] go: go 1.24 upstream EOL passed (2026-02), add (EOL) flag
[ INFO  ] go: go 1.25 upstream EOL not yet set
[ OK    ] all dates match
```

Exit codes: `0` = clean or warnings only · `1` = errors found (drift, expiry, or a local/data error such as an invalid `eol.json`) · `2` = endoflife.date unreachable (all fetches failed) — a neutral network outage CI can treat as non-fatal

Errors include the **expiry gate**: any runtime or OS entry whose `supported_until` is already in the past (EOL + grace elapsed) is a hard error and must be dropped from `pkg/eol.json`.

## Current EOL Status (2026-07-13)

All dates verified against endoflife.date API. No errors. Warnings below are proximity/past-EOL alerts only — every entry is still inside its grace window.

### Runtimes — warnings

None. All shipped runtimes are within (or ahead of) upstream EOL; `node 20` is already flagged `(EOL)` in the matrix.

### OS — warnings

| Entry | Note |
|-------|------|
| debian 11 | Past EOL (2024-08), grace until 2027-08 |
| fedora 40, 41, 42 | Past EOL (2025-05 / 2025-12 / 2026-05), grace active |
| amazonlinux 2 | Past EOL (2026-06), grace until 2029-06 |
| alpine 3.20 | Past EOL (2026-04), grace until 2029-04 |
| debian 12 | EOL now (2026-07), grace until 2029-07 |
| fedora 43, 44 | EOL in ~5 / ~11 months |
| alpine 3.21, 3.22 | EOL in ~4 / ~10 months |
| centos_stream 9 | EOL in ~10 months (2027-05) |
| ubuntu 22.04 | EOL in ~9 months (2027-04) |

## Architecture

`run.sh` → builds `freeunit-eol-check:latest` (FROM `rust:slim-trixie` + `curl`) → mounts repo as `/repo` → `entrypoint.sh` compiles `pkg/eol/src/main.rs` → runs binary against `/repo/pkg/eol.json`.

Source: `src/main.rs` — zero external deps beyond `serde_json`. HTTP via `curl` subprocess.

See `PLAN.md` for full architecture and known issues.
