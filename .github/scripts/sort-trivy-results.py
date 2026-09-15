#!/usr/bin/env python3
"""Create a severity-sorted Markdown security audit report from Trivy JSON."""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path
from typing import Any

SEVERITY_ORDER = {
    "CRITICAL": 0,
    "HIGH": 1,
    "MEDIUM": 2,
    "LOW": 3,
    "UNKNOWN": 4,
}
DISPLAY_SEVERITIES = ("CRITICAL", "HIGH", "MEDIUM", "LOW", "UNKNOWN")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Read Trivy JSON output and write a Markdown report sorted by "
            "vulnerability severity, with CRITICAL findings first."
        )
    )
    parser.add_argument("trivy_json", type=Path, help="Path to Trivy JSON output")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("security-audit-report.md"),
        help="Markdown report path (default: security-audit-report.md)",
    )
    parser.add_argument(
        "--max-rows",
        type=int,
        default=200,
        help="Maximum vulnerability rows to include in the table (default: 200)",
    )
    parser.add_argument(
        "--fail-on-severity",
        choices=(*DISPLAY_SEVERITIES, "NONE"),
        default="CRITICAL",
        help=(
            "Exit with status 1 when vulnerabilities at or above this severity "
            "are found. Use NONE to disable. (default: CRITICAL)"
        ),
    )
    return parser.parse_args()


def normalized_severity(value: str | None) -> str:
    severity = (value or "UNKNOWN").upper()
    return severity if severity in SEVERITY_ORDER else "UNKNOWN"


def markdown_escape(value: Any) -> str:
    text = "" if value is None else str(value)
    return text.replace("|", "\\|").replace("\n", " ").replace("\r", " ")


def cvss_v3_score(vulnerability: dict[str, Any]) -> float:
    cvss = vulnerability.get("CVSS")
    if not isinstance(cvss, dict):
        return 0.0

    for source in ("nvd", "redhat", "ghsa"):
        source_data = cvss.get(source)
        if isinstance(source_data, dict):
            score = source_data.get("V3Score", 0)
            if score:
                return float(score)

    return 0.0


def vulnerability_sort_key(vulnerability: dict[str, Any]) -> tuple[Any, ...]:
    severity = normalized_severity(vulnerability.get("Severity"))

    return (
        SEVERITY_ORDER[severity],
        -cvss_v3_score(vulnerability),
        str(vulnerability.get("VulnerabilityID", "")),
        str(vulnerability.get("PkgName", "")),
        str(vulnerability.get("Target", "")),
    )


def collect_vulnerabilities(report: dict[str, Any]) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []

    if not isinstance(report, dict):
        return findings

    for result in report.get("Results") or []:
        target = result.get("Target", "")
        result_type = result.get("Type", "")

        for vulnerability in result.get("Vulnerabilities") or []:
            item = dict(vulnerability)
            item["Target"] = target
            item["ResultType"] = result_type
            item["Severity"] = normalized_severity(item.get("Severity"))
            findings.append(item)

    findings.sort(key=vulnerability_sort_key)
    return findings


def format_summary(counts: Counter[str]) -> str:
    return " | ".join(f"{severity}: {counts.get(severity, 0)}" for severity in DISPLAY_SEVERITIES)


def render_markdown(findings: list[dict[str, Any]], max_rows: int) -> str:
    counts = Counter(finding["Severity"] for finding in findings)
    lines = [
        "# Security Audit",
        "",
        "Vulnerability findings are sorted by severity, with critical findings first.",
        "",
        "## Summary",
        "",
        f"Total vulnerabilities: **{len(findings)}**",
        "",
        f"{format_summary(counts)}",
        "",
    ]

    if not findings:
        lines.extend(["## Vulnerabilities", "", "No vulnerabilities were reported by Trivy.", ""])
        return "\n".join(lines)

    displayed_findings = findings[:max_rows]
    lines.extend(
        [
            "## Vulnerabilities",
            "",
            "| Severity | Vulnerability | Package | Installed | Fixed | Target | Title |",
            "| --- | --- | --- | --- | --- | --- | --- |",
        ]
    )

    for finding in displayed_findings:
        lines.append(
            "| {severity} | {vuln_id} | {package} | {installed} | {fixed} | {target} | {title} |".format(
                severity=markdown_escape(finding.get("Severity")),
                vuln_id=markdown_escape(finding.get("VulnerabilityID")),
                package=markdown_escape(finding.get("PkgName")),
                installed=markdown_escape(finding.get("InstalledVersion")),
                fixed=markdown_escape(finding.get("FixedVersion") or "not fixed"),
                target=markdown_escape(finding.get("Target")),
                title=markdown_escape(finding.get("Title")),
            )
        )

    omitted = len(findings) - len(displayed_findings)
    if omitted > 0:
        lines.extend(["", f"_{omitted} additional vulnerabilities omitted from this table._"])

    lines.append("")
    return "\n".join(lines)


def should_fail(findings: list[dict[str, Any]], fail_on_severity: str) -> bool:
    if fail_on_severity == "NONE":
        return False

    threshold = SEVERITY_ORDER[fail_on_severity]
    return any(SEVERITY_ORDER[finding["Severity"]] <= threshold for finding in findings)


def main() -> int:
    args = parse_args()

    with args.trivy_json.open(encoding="utf-8") as fh:
        report = json.load(fh)

    findings = collect_vulnerabilities(report)
    markdown = render_markdown(findings, args.max_rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(markdown, encoding="utf-8")
    print(markdown)

    if should_fail(findings, args.fail_on_severity):
        print(
            f"Security audit failed: found vulnerabilities at or above {args.fail_on_severity} severity.",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
