# FreeUnit Support Policy

FreeUnit supports each language runtime for **1 year after its upstream EOL date**,
and each OS version for **3 years after its upstream EOL date**.
This gives users time to migrate without being forced onto newer versions immediately.

EOL dates are tracked at [endoflife.date](https://endoflife.date).

## Runtime Support Matrix

| Runtime | Version | Upstream EOL | FreeUnit Drops |
|---------|---------|--------------|----------------|
| Go | 1.24 | Feb 2026 | Feb 2027 |
| Go | 1.25 | Aug 2026 | Aug 2027 |
| Go | 1.26 | Feb 2027 | Feb 2028 |
| Java (JSC) | 17 (LTS) | Sep 2029 | Sep 2030 |
| Java (JSC) | 21 (LTS) | Sep 2031 | Sep 2032 |
| Node.js | 20 (LTS) | Apr 2026 | Apr 2027 |
| Node.js | 22 (LTS) | Apr 2027 | Apr 2028 |
| Node.js | 24 (LTS) | Apr 2028 | Apr 2029 |
| Perl | 5.38 | Jul 2026 | Jul 2027 |
| Perl | 5.40 | Jul 2028 | Jul 2029 |
| PHP | 8.3 | Dec 2027 | Dec 2028 |
| PHP | 8.4 | Dec 2028 | Dec 2029 |
| PHP | 8.5 | Dec 2029 | Dec 2030 |
| Python | 3.12 | Oct 2028 | Oct 2029 |
| Python | 3.13 | Oct 2029 | Oct 2030 |
| Python | 3.14 | Oct 2030 | Oct 2031 |
| Ruby | 3.3 | Mar 2027 | Mar 2028 |
| Ruby | 3.4 | Mar 2028 | Mar 2029 |
| WebAssembly | — | no EOL | — |

## OS Support Matrix

| OS | Version | Upstream EOL | FreeUnit Drops | Default Python |
|----|---------|--------------|----------------|----------------|
| Fedora | 40 (EOL) † | 2025-05 | 2028-05 | 3.11 |
| Fedora | 41 (EOL) † | 2025-12 | 2028-12 | 3.12 |
| Fedora | 42 | 2026-05 | 2029-05 | 3.13 |
| Fedora | 43 | 2026-12 | 2029-12 | 3.14 |
| CentOS Stream | 9 | 2027-05 | 2030-05 | 3.11 |
| CentOS Stream | 10 | 2035-05 | 2038-05 | 3.13 |
| Amazon Linux | 2 | 2026-06 | 2029-06 | 3.7 ‡ |
| Amazon Linux | 2023 | 2029-06 | 2032-06 | 3.11 |
| Ubuntu (LTS) | 22.04 | 2027-04 | 2030-04 | 3.10 |
| Ubuntu (LTS) | 24.04 | 2029-05 | 2032-05 | 3.12 |
| Ubuntu (LTS) | 26.04 | 2031-05 | 2034-05 | 3.13 |
| Debian | 10 (buster) (EOL) † | 2024-06 | 2027-06 | 3.7 ‡ |
| Debian | 11 (bullseye) (EOL) † | 2026-08 | 2029-08 | 3.9 |
| Debian | 12 (bookworm) | 2028-06 | 2031-06 | 3.11 |
| Debian | 13 (trixie) | 2030-06 | 2033-06 | 3.13 |
| RHEL | 8 | 2029-05 | 2032-05 | 3.8 ‡ |
| RHEL | 9 | 2032-05 | 2035-05 | 3.11 |
| RHEL | 10 | 2035-05 | 2038-05 | 3.13 |
| Alpine | 3.20 | 2026-11 | 2029-11 | 3.12 |
| Alpine | 3.21 | 2027-11 | 2030-11 | 3.13 |

† OS past upstream EOL; in FreeUnit 3-year grace period.
‡ Default Python shipped by this OS is itself past upstream EOL. FreeUnit does not backport fixes to that Python version.

## Rules

- **Adding a runtime version:** when new upstream release reaches stable, FreeUnit adds
  it within one release cycle.
- **Adding an OS version:** when new OS release is available, FreeUnit adds it within
  one release cycle.
- **Dropping a version:** announced at least one release before removal. Noted in
  `CHANGES`.
- **Security-only mode:** versions within 6 months of the FreeUnit drop date receive
  security fixes only — no new features backported.
- **LTS runtimes (Java 17/21):** follow the upstream LTS schedule strictly.
- **LTS OS (Ubuntu, RHEL, Debian):** 3-year extension applies to standard EOL, not
  extended security maintenance dates.

## Source of Truth

Machine-readable version data lives in [`pkg/eol.json`](pkg/eol.json).
The Docker CI matrix and RPM packaging are driven by this file.

## Reporting EOL Issues

If a runtime or OS version in the matrix has reached upstream EOL and is not yet listed here,
open an issue with the label `eol` at
[github.com/freeunitorg/freeunit/issues](https://github.com/freeunitorg/freeunit/issues).
