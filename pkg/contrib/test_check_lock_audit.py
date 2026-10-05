"""Tests for check-lock-audit.py.

These run without a Unit binary, so they live next to the script instead of in
test/, where conftest.py starts a server for every session.  Run them with
`python3 -m pytest pkg/contrib/test_check_lock_audit.py`.
"""
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).with_name("check-lock-audit.py")

LINKED = """\
wasmtime-c-api v47.0.4 (/src/crates/c-api/artifact)
crossbeam-epoch v0.9.18
chacha20 v0.10.0
anyhow v1.0.100
wasmtime v47.0.4 (/src/crates/wasmtime)
"""

ALLOW = {
    "allow": [
        {
            "kind": "vulnerability",
            "id": "RUSTSEC-2026-0204",
            "package": "crossbeam-epoch",
            "version": "0.9.18",
            "reason": "not reached",
        },
        {
            "kind": "yanked",
            "id": None,
            "package": "chacha20",
            "version": "0.10.0",
            "reason": "a yank is not an advisory",
        },
    ]
}


def vulnerability(identifier, package, version):
    return {
        "advisory": {"id": identifier},
        "package": {"name": package, "version": version},
    }


def report(vulnerabilities=(), warnings=None):
    return {
        "vulnerabilities": {
            "count": len(vulnerabilities),
            "list": list(vulnerabilities),
        },
        "warnings": warnings or {},
    }


BASELINE = report(
    vulnerabilities=[vulnerability("RUSTSEC-2026-0204", "crossbeam-epoch", "0.9.18")],
    warnings={
        "yanked": [{"advisory": None, "package": {"name": "chacha20", "version": "0.10.0"}}],
        "unmaintained": [
            {
                "advisory": {"id": "RUSTSEC-2024-0436"},
                "package": {"name": "paste", "version": "1.0.7"},
            }
        ],
    },
)


@pytest.fixture
def run(tmp_path):
    def call(report_text, allow=ALLOW, linked_text=LINKED):
        linked = tmp_path / "linked.txt"
        linked.write_text(linked_text)
        report_file = tmp_path / "audit.json"
        if report_text is not None:
            report_file.write_text(report_text)
        allow_file = tmp_path / "allow.json"
        allow_file.write_text(json.dumps(allow))
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--report",
                str(report_file),
                "--linked",
                str(linked),
                "--allow",
                str(allow_file),
            ],
            capture_output=True,
            text=True,
        )

    return call


def test_allowlisted_findings_pass(run):
    result = run(json.dumps(BASELINE))
    assert result.returncode == 0, result.stdout + result.stderr
    assert "accepted:   crossbeam-epoch 0.9.18" in result.stdout
    assert "accepted:   chacha20 0.10.0: yanked" in result.stdout


def test_unlinked_finding_is_reported_but_passes(run):
    result = run(json.dumps(BASELINE))
    assert result.returncode == 0
    assert "not linked: paste 1.0.7: RUSTSEC-2024-0436" in result.stdout


def test_unknown_linked_vulnerability_fails(run):
    data = report(
        vulnerabilities=[
            vulnerability("RUSTSEC-2026-0204", "crossbeam-epoch", "0.9.18"),
            vulnerability("RUSTSEC-2099-0001", "anyhow", "1.0.100"),
        ]
    )
    result = run(json.dumps(data))
    assert result.returncode == 1
    assert "error:      anyhow 1.0.100: RUSTSEC-2099-0001 is linked" in result.stdout


def test_unknown_linked_yank_fails(run):
    data = report(
        warnings={
            "yanked": [
                {"advisory": None, "package": {"name": "anyhow", "version": "1.0.100"}}
            ]
        }
    )
    result = run(json.dumps(data))
    assert result.returncode == 1
    assert "error:      anyhow 1.0.100: yanked is linked" in result.stdout


def test_an_unseen_warning_kind_is_still_gated(run):
    data = report(
        warnings={
            "unsound": [
                {
                    "advisory": {"id": "RUSTSEC-2099-0002"},
                    "package": {"name": "anyhow", "version": "1.0.100"},
                }
            ]
        }
    )
    result = run(json.dumps(data))
    assert result.returncode == 1
    assert "RUSTSEC-2099-0002" in result.stdout


def test_the_allowlist_version_is_part_of_the_key(run):
    # An acceptance is written against one version; the next pin has to earn
    # its own entry rather than inherit this one.
    linked = LINKED + "crossbeam-epoch v0.9.20\n"
    data = report(
        vulnerabilities=[
            vulnerability("RUSTSEC-2026-0204", "crossbeam-epoch", "0.9.20")
        ]
    )
    result = run(json.dumps(data), linked_text=linked)
    assert result.returncode == 1
    assert "error:      crossbeam-epoch 0.9.20: RUSTSEC-2026-0204" in result.stdout


def test_a_stale_entry_warns_without_failing(run):
    data = report(
        vulnerabilities=[vulnerability("RUSTSEC-2026-0204", "crossbeam-epoch", "0.9.18")]
    )
    result = run(json.dumps(data))
    assert result.returncode == 0
    assert "stale:      chacha20 0.10.0: yanked is no longer reported" in result.stdout


def test_malformed_report_fails(run):
    result = run("{not json")
    assert result.returncode == 2
    assert "not valid JSON" in result.stderr


def test_an_empty_report_fails(run):
    result = run("{}")
    assert result.returncode == 2
    assert "not a cargo audit report" in result.stderr


def test_a_missing_report_fails(run):
    result = run(None)
    assert result.returncode == 2
    assert "cannot read" in result.stderr


def test_an_allowlist_entry_needs_a_reason(run):
    allow = {"allow": [{"kind": "yanked", "package": "chacha20", "version": "0.10.0"}]}
    result = run(json.dumps(BASELINE), allow)
    assert result.returncode == 2
    assert "without reason" in result.stderr


def module():
    spec = importlib.util.spec_from_file_location("check_lock_audit", SCRIPT)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


def archive(tmp_path, check, features_cmake):
    """Lay out the parts of an unpacked archive that read_features() reads."""
    cmake = tmp_path / check.FEATURES_CMAKE
    cmake.parent.mkdir(parents=True, exist_ok=True)
    cmake.write_text(features_cmake)
    cmakelists = tmp_path / check.CMAKELISTS
    cmakelists.write_text("${WASMTIME_CARGO_BINARY} build ${WASMTIME_FEATURES}\n")
    return tmp_path


def test_the_feature_set_follows_the_cmake_override(tmp_path, monkeypatch):
    check = module()
    makefile = tmp_path / "Makefile"
    makefile.write_text("-DWASMTIME_FEATURE_WASI_HTTP=OFF \\\n")
    monkeypatch.setattr(check, "MAKEFILE", makefile)
    tree = archive(
        tmp_path,
        check,
        'set(WASMTIME_FEATURES "--no-default-features")\n'
        "feature(wasi ON)\nfeature(wasi-http ON)\nfeature(all-arch OFF)\n",
    )
    assert check.read_features(tree) == ["wasi"]


@pytest.mark.parametrize(
    "features_cmake, cmakelists",
    [
        ('set(WASMTIME_FEATURES "")\nfeature(wasi ON)\n', None),
        (None, "${WASMTIME_CARGO_BINARY} build\n"),
    ],
)
def test_a_changed_cargo_invocation_fails(
    tmp_path, monkeypatch, features_cmake, cmakelists
):
    """The reconstruction is only valid while cmake drives cargo this way."""
    check = module()
    makefile = tmp_path / "Makefile"
    makefile.write_text("")
    monkeypatch.setattr(check, "MAKEFILE", makefile)
    tree = archive(
        tmp_path,
        check,
        features_cmake
        or 'set(WASMTIME_FEATURES "--no-default-features")\nfeature(wasi ON)\n',
    )
    if cmakelists:
        (tree / check.CMAKELISTS).write_text(cmakelists)
    with pytest.raises(SystemExit) as error:
        check.read_features(tree)
    assert error.value.code == 2


def test_an_undeclared_cmake_flag_fails(tmp_path, monkeypatch):
    check = module()
    makefile = tmp_path / "Makefile"
    makefile.write_text("-DWASMTIME_FEATURE_NO_SUCH_THING=OFF\n")
    monkeypatch.setattr(check, "MAKEFILE", makefile)
    tree = archive(
        tmp_path,
        check,
        'set(WASMTIME_FEATURES "--no-default-features")\nfeature(wasi ON)\n',
    )
    with pytest.raises(SystemExit) as error:
        check.read_features(tree)
    assert error.value.code == 2


def test_a_cargo_tree_listing_parses(tmp_path):
    """The shapes cargo tree --prefix=none actually prints."""
    check = module()
    listing = tmp_path / "tree.txt"
    listing.write_text(
        "wasmtime v47.0.4 (/src/crates/wasmtime)\n"
        "anyhow v1.0.100\n"
        "anyhow v1.0.100 (*)\n"
        "serde_derive v1.0.228 (proc-macro)\n"
        "wasi v0.11.0+wasi-snapshot-preview1\n"
        "\n"
    )
    found = {
        (match.group("name"), match.group("version"))
        for line in listing.read_text().splitlines()
        if (match := check.PACKAGE_RE.match(line))
    }
    assert found == {
        ("wasmtime", "47.0.4"),
        ("anyhow", "1.0.100"),
        ("serde_derive", "1.0.228"),
        ("wasi", "0.11.0+wasi-snapshot-preview1"),
    }


def test_a_duplicate_allowlist_entry_fails(run):
    entry = {
        "kind": "yanked",
        "id": None,
        "package": "chacha20",
        "version": "0.10.0",
        "reason": "first",
    }
    allow = {"allow": [entry, dict(entry, reason="second")]}
    result = run(json.dumps(BASELINE), allow)
    assert result.returncode == 2
    assert "twice" in result.stderr


def test_a_warning_without_a_package_fails(run):
    data = report(warnings={"yanked": [{"advisory": None}]})
    result = run(json.dumps(data))
    assert result.returncode == 2
    assert "without a package name and version" in result.stderr


def test_stale_entries_sort_with_and_without_an_advisory_id(run):
    # Sorting the stale list used to compare None against a string.
    allow = {
        "allow": [
            {
                "kind": "yanked",
                "id": None,
                "package": "aaa",
                "version": "1.0.0",
                "reason": "no advisory",
            },
            {
                "kind": "yanked",
                "id": "RUSTSEC-2099-0003",
                "package": "bbb",
                "version": "2.0.0",
                "reason": "with advisory",
            },
        ]
    }
    result = run(json.dumps(report()), allow)
    assert result.returncode == 0
    assert result.stdout.count("stale:") == 2
