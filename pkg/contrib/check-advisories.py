#!/usr/bin/env python3
"""Check tarball-pinned dependencies against OSV advisories.

`cargo audit` reads Cargo.lock files only, so a dependency pinned as a source
tarball under pkg/contrib is invisible to it.  This script reads the version
pin and asks OSV, which mirrors the RustSec advisory database and also carries
the GHSA/CVE aliases.

It also checks that the three places naming the pin agree:
  - pkg/contrib/src/wasmtime/version
  - pkg/contrib/src/wasmtime/SHA512SUMS
  - pkg/eol.json (dependencies.wasmtime_c_api)

Exit codes: 0 clean, 1 advisories found, 2 configuration or query failure.
"""
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VERSION_FILE = REPO / "pkg/contrib/src/wasmtime/version"
SUMS_FILE = REPO / "pkg/contrib/src/wasmtime/SHA512SUMS"
EOL_FILE = REPO / "pkg/eol.json"
OSV_URL = "https://api.osv.dev/v1/query"

# The contrib tarball is the wasmtime workspace; pinning it pins both crates
# that the C API links.
CRATES = ("wasmtime", "wasmtime-wasi")


def read_version() -> str:
    match = re.search(r"WASMTIME_VERSION\s*:=\s*(\S+)", VERSION_FILE.read_text())
    if not match:
        fail(f"no WASMTIME_VERSION in {VERSION_FILE.relative_to(REPO)}")
    return match.group(1)


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def check_sums(version: str) -> None:
    tarball = f"wasmtime-v{version}-src.tar.gz"
    pattern = re.compile(rf"\s{tarball}$", re.MULTILINE)
    if not pattern.search(SUMS_FILE.read_text()):
        fail(f"{SUMS_FILE.relative_to(REPO)} has no digest for {tarball}")


def check_eol_json(version: str) -> None:
    config = json.loads(EOL_FILE.read_text())
    pinned = config["dependencies"]["wasmtime_c_api"]["version"]
    if pinned != version:
        fail(
            f"{EOL_FILE.relative_to(REPO)} says wasmtime_c_api {pinned}, but the "
            f"pin is {version}"
        )


def query(crate: str, version: str) -> list:
    body = json.dumps(
        {"package": {"name": crate, "ecosystem": "crates.io"}, "version": version}
    ).encode()
    request = urllib.request.Request(
        OSV_URL, data=body, headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response).get("vulns", [])
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
        fail(f"OSV query for {crate} {version} failed: {error}")


def main() -> int:
    version = read_version()
    check_sums(version)
    check_eol_json(version)

    found = False
    seen = set()
    for crate in CRATES:
        for vuln in query(crate, version):
            aliases = vuln.get("aliases", [])
            # OSV carries the GHSA and RUSTSEC records of one advisory as two
            # entries that alias each other; report each advisory once.
            key = tuple(sorted({vuln["id"], *aliases}))
            if key in seen:
                continue
            seen.add(key)
            found = True
            alias_text = ", ".join(aliases)
            suffix = f" ({alias_text})" if alias_text else ""
            print(f"{crate} {version}: {vuln['id']}{suffix}")
            if vuln.get("summary"):
                print(f"  {vuln['summary']}")

    if found:
        return 1

    print(f"{', '.join(CRATES)} {version}: no advisories")
    return 0


if __name__ == "__main__":
    sys.exit(main())
