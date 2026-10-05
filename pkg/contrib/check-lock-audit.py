#!/usr/bin/env python3
"""Gate a `cargo audit --json` report on the pinned Wasmtime source lock.

The archived Cargo.lock covers the whole Wasmtime workspace: the CLI, the
fuzzing targets and wasi-http are all in it, while Unit links one C API
library built from a subset of the features.  A finding against a crate that
the library does not link is not a finding against Unit, so auditing the lock
as a whole reports far more than it can act on.

The archive cannot be repaired either.  It ships `.cargo/config.toml` with
`replace-with = "vendored-sources"` over a `vendor/` directory, so
`cargo update --precise` inside it is a no-op: the vendor set holds the pinned
versions and nothing else.

So this script splits the report in two.  A finding against a crate that the C
API links fails the job unless the allowlist accepts that exact version with a
reason.  A finding outside the linked graph is reported and does not fail.

The linked graph is computed, never declared: `cargo tree` resolves it offline
against the vendored archive, with the feature set that pkg/contrib builds.  A
Wasmtime bump that starts linking an affected crate therefore turns the job red
on its own, which a hand-written "not linked" note would not.

Exit codes: 0 clean, 1 a linked finding is not allowed, 2 configuration or
tooling failure.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAKEFILE = REPO / "pkg/contrib/src/wasmtime/Makefile"
ALLOWLIST = REPO / "pkg/contrib/wasmtime-lock-audit.allow.json"
FEATURES_CMAKE = Path("crates/c-api/cmake/features.cmake")
CMAKELISTS = Path("crates/c-api/CMakeLists.txt")

PACKAGE_RE = re.compile(r"^(?P<name>[\w.+-]+) v(?P<version>[^\s]+)")
FEATURE_RE = re.compile(
    r"^\s*feature\((?P<name>[\w-]+)\s+(?P<default>ON|OFF)\)", re.MULTILINE
)
CMAKE_FLAG_RE = re.compile(r"-D(?P<name>WASMTIME_[A-Z0-9_]+)=(?P<value>ON|OFF)")


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def rel(path: Path) -> Path:
    """Shorten a path for a message, unless a test moved it out of the tree."""
    try:
        return path.relative_to(REPO)
    except ValueError:
        return path


def cmake_name(feature: str) -> str:
    return f"wasmtime_feature_{feature}".upper().replace("-", "_")


def read_cmake_flags() -> dict:
    """Read the -DWASMTIME_* flags that pkg/contrib passes to cmake."""
    if not MAKEFILE.is_file():
        fail(f"{MAKEFILE} is missing")
    return {
        match.group("name"): match.group("value") == "ON"
        for match in CMAKE_FLAG_RE.finditer(MAKEFILE.read_text())
    }


def read_features(tree: Path) -> list:
    """Resolve the Cargo features of the C API library as cmake would."""
    source = tree / FEATURES_CMAKE
    if not source.is_file():
        fail(f"{source} is missing; is {tree} an unpacked Wasmtime archive?")

    # The feature list below is only the whole story while cmake still starts
    # from nothing and passes exactly what features.cmake collected.  If a
    # release changes either, the resolved graph shifts and findings move
    # quietly in or out of it.
    text = source.read_text()
    if "--no-default-features" not in text:
        fail(f"{FEATURES_CMAKE} no longer starts from --no-default-features")

    cmakelists = tree / CMAKELISTS
    if not cmakelists.is_file():
        fail(f"{cmakelists} is missing")
    if "${WASMTIME_FEATURES}" not in cmakelists.read_text():
        fail(f"{CMAKELISTS} no longer passes ${{WASMTIME_FEATURES}} to cargo")

    flags = read_cmake_flags()
    disable_all = flags.get("WASMTIME_DISABLE_ALL_FEATURES", False)

    features = []
    declared = set()
    for match in FEATURE_RE.finditer(text):
        name = match.group("name")
        declared.add(cmake_name(name))
        default = match.group("default") == "ON" and not disable_all
        if flags.get(cmake_name(name), default):
            features.append(name)

    if not features:
        fail(f"no features parsed from {source}")

    unknown = set(flags) - declared - {"WASMTIME_DISABLE_ALL_FEATURES"}
    if unknown:
        fail(
            f"{rel(MAKEFILE)} sets {', '.join(sorted(unknown))}, "
            f"which {FEATURES_CMAKE} does not declare"
        )

    return features


def linked_packages(tree: Path) -> set:
    """Return the (name, version) pairs the C API library links."""
    command = [
        "cargo",
        "tree",
        "--offline",
        "--locked",
        "--package=wasmtime-c-api",
        "--edges=normal,build",
        # Without this cargo resolves the host triple alone, and a crate
        # linked only on another architecture would read as not linked.
        "--target=all",
        "--prefix=none",
        "--no-default-features",
    ]
    command += [f"--features={feature}" for feature in read_features(tree)]

    try:
        result = subprocess.run(
            command, cwd=tree, capture_output=True, text=True, check=True
        )
    except OSError as error:
        fail(f"cannot run cargo tree: {error}")
    except subprocess.CalledProcessError as error:
        fail(f"cargo tree failed:\n{error.stderr.strip()}")

    packages = set()
    for line in result.stdout.splitlines():
        match = PACKAGE_RE.match(line)
        if match:
            packages.add((match.group("name"), match.group("version")))

    if not packages:
        fail("cargo tree reported no packages")

    return packages


def read_findings(report: Path) -> list:
    """Flatten a cargo audit report into (kind, id, package, version) tuples.

    A yanked crate carries no advisory, so the identifier is None there and
    the allowlist has to key on the kind and the exact version instead.
    """
    try:
        data = json.loads(report.read_text())
    except OSError as error:
        fail(f"cannot read {report}: {error}")
    except json.JSONDecodeError as error:
        fail(f"{report} is not valid JSON: {error}")

    if not isinstance(data, dict) or "vulnerabilities" not in data:
        fail(f"{report} is not a cargo audit report")

    def package(entry: dict) -> tuple:
        try:
            return entry["package"]["name"], entry["package"]["version"]
        except (KeyError, TypeError):
            fail(f"{report} has an entry without a package name and version")

    findings = []
    for entry in data["vulnerabilities"].get("list", []):
        advisory = entry.get("advisory") or {}
        findings.append(("vulnerability", advisory.get("id"), *package(entry)))

    # Warnings are grouped by kind, and a kind this script has never seen is
    # still a warning: iterate whatever the report carries.
    for kind, entries in sorted(data.get("warnings", {}).items()):
        for entry in entries:
            advisory = entry.get("advisory") or {}
            findings.append((kind, advisory.get("id"), *package(entry)))

    return findings


def read_allowlist(path: Path) -> dict:
    try:
        data = json.loads(path.read_text())
    except OSError as error:
        fail(f"cannot read {path}: {error}")
    except json.JSONDecodeError as error:
        fail(f"{path} is not valid JSON: {error}")

    allowed = {}
    for entry in data.get("allow", []):
        missing = {"kind", "package", "version", "reason"} - set(entry)
        if missing:
            fail(f"{path} has an entry without {', '.join(sorted(missing))}")
        key = (entry["kind"], entry.get("id"), entry["package"], entry["version"])
        if key in allowed:
            fail(f"{path} lists {entry['package']} {entry['version']} twice")
        allowed[key] = entry["reason"]

    return allowed


def describe(finding: tuple) -> str:
    kind, identifier, package, version = finding
    label = identifier or kind
    return f"{package} {version}: {label}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--report", required=True, type=Path, help="cargo audit --json output"
    )
    parser.add_argument(
        "--tree", type=Path, help="the unpacked Wasmtime source archive"
    )
    parser.add_argument(
        "--linked", type=Path, help="a cargo tree listing, instead of --tree"
    )
    parser.add_argument("--allow", type=Path, default=ALLOWLIST)
    args = parser.parse_args()

    if (args.tree is None) == (args.linked is None):
        fail("pass either --tree or --linked")

    if args.linked:
        packages = {
            (match.group("name"), match.group("version"))
            for line in args.linked.read_text().splitlines()
            if (match := PACKAGE_RE.match(line))
        }
    else:
        packages = linked_packages(args.tree)

    findings = read_findings(args.report)
    allowed = read_allowlist(args.allow)

    unlinked = []
    accepted = []
    denied = []
    for finding in findings:
        _, _, package, version = finding
        if (package, version) not in packages:
            unlinked.append(finding)
        elif finding in allowed:
            accepted.append(finding)
        else:
            denied.append(finding)

    print(f"{len(packages)} packages linked by the C API library")

    for finding in unlinked:
        print(f"not linked: {describe(finding)}")

    for finding in accepted:
        print(f"accepted:   {describe(finding)}")
        print(f"            {allowed[finding]}")

    # An advisory id is None for a yank, so sort on a string in its place.
    stale = sorted(set(allowed) - set(accepted), key=lambda f: (f[0], f[1] or "", *f[2:]))
    for finding in stale:
        print(f"stale:      {describe(finding)} is no longer reported as linked")

    for finding in denied:
        print(f"error:      {describe(finding)} is linked and not allowed")

    if denied:
        print(
            f"\n{len(denied)} linked finding(s) are not in "
            f"{args.allow.name}.  Bump the pin, or add an entry with the "
            f"reason the build is not affected.",
            file=sys.stderr,
        )
        return 1

    print(f"\n{len(findings)} finding(s), none linked and unaccounted for")
    return 0


if __name__ == "__main__":
    sys.exit(main())
