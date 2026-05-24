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

Exit codes: `0` = clean or warnings only · `1` = errors found · `2` = file/network error

## Current EOL Status (2026-05-20)

All dates verified against endoflife.date API. No errors. Warnings below are proximity alerts only.

### Runtimes — warnings

| Entry | Note |
|-------|------|
| go 1.24 | Past EOL (2026-02), in 12-month grace until 2027-02 |
| node 20 | Past EOL (2026-04), in 12-month grace until 2027-04 |
| go 1.25, 1.26, node 26 | Upstream EOL not yet set (future) |

### OS — warnings

| Entry | Note |
|-------|------|
| alpine 3.20 | Past EOL (2026-04), grace until 2029-04 |
| debian 10, 11 | Past EOL, grace periods active |
| fedora 40, 41, 42 | Past EOL, grace periods active |
| debian 12 | EOL in ~1 month (2026-06) |
| alpine 3.21 | EOL in ~6 months (2026-11) |
| ubuntu 22.04 | EOL in ~11 months (2027-04) |

## Architecture

`run.sh` → builds `freeunit-eol-check:latest` (FROM `rust:slim-trixie` + `curl`) → mounts repo as `/repo` → `entrypoint.sh` compiles `pkg/eol/src/main.rs` → runs binary against `/repo/pkg/eol.json`.

Source: `src/main.rs` — zero external deps beyond `serde_json`. HTTP via `curl` subprocess.

See `PLAN.md` for full architecture and known issues.
