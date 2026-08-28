# FreeUnit Support Policy

FreeUnit supports each language runtime for **1 year after its upstream EOL date**,
and each OS version for **3 years after its upstream EOL date**.
This gives users time to migrate without being forced onto newer versions immediately.

EOL dates are tracked at [endoflife.date](https://endoflife.date).

## Runtime Support Matrix

| Runtime | Version | Upstream EOL | FreeUnit Drops |
|---------|---------|--------------|----------------|
| Go | 1.24 (EOL) † | Feb 2026 | Feb 2027 |
| Go | 1.25 | Aug 2026 | Aug 2027 |
| Go | 1.26 | Feb 2027 | Feb 2028 |
| Java (JSC) | 17 (LTS) | Oct 2027 | Oct 2028 |
| Java (JSC) | 21 (LTS) | Dec 2029 | Dec 2030 |
| Java (JSC) | 25 (LTS) | Sep 2031 | Sep 2032 |
| Java (JSC) | 26 | Sep 2026 | Sep 2027 |
| Node.js | 20 (LTS) (EOL) † | Apr 2026 | Apr 2027 |
| Node.js | 22 (LTS) | Apr 2027 | Apr 2028 |
| Node.js | 24 (LTS) | Apr 2028 | Apr 2029 |
| Node.js | 26 (LTS) | Apr 2029 | Apr 2030 |
| Perl | 5.38 | Jul 2026 | Jul 2027 |
| Perl | 5.40 | Jun 2027 | Jun 2028 |
| Perl | 5.42 | Jul 2028 | Jul 2029 |
| PHP | 8.3 | Dec 2027 | Dec 2028 |
| PHP | 8.4 | Dec 2028 | Dec 2029 |
| PHP | 8.5 | Dec 2029 | Dec 2030 |
| Python | 3.12 | Oct 2028 | Oct 2029 |
| Python | 3.13 | Oct 2029 | Oct 2030 |
| Python | 3.14 | Oct 2030 | Oct 2031 |
| Ruby | 3.3 | Mar 2027 | Mar 2028 |
| Ruby | 3.4 | Mar 2028 | Mar 2029 |
| Ruby | 4.0 | Mar 2029 | Mar 2030 |
| WebAssembly | — | no EOL | — |

## OS Support Matrix

| OS | Version | Upstream EOL | FreeUnit Drops | Default Python |
|----|---------|--------------|----------------|----------------|
| Fedora | 40 (EOL) † | 2025-05 | 2028-05 | 3.11 |
| Fedora | 41 (EOL) † | 2025-12 | 2028-12 | 3.12 |
| Fedora | 42 (EOL) † | 2026-05 | 2029-05 | 3.13 |
| Fedora | 43 | 2026-12 | 2029-12 | 3.14 |
| Fedora | 44 | 2027-06 | 2030-06 | 3.14 |
| CentOS Stream | 9 | 2027-05 | 2030-05 | 3.11 |
| CentOS Stream | 10 | 2030-05 | 2033-05 | 3.13 |
| Amazon Linux | 2 | 2026-06 | 2029-06 | 3.7 ‡ |
| Amazon Linux | 2023 | 2029-06 | 2032-06 | 3.11 |
| Ubuntu (LTS) | 22.04 | 2027-04 | 2030-04 | 3.10 |
| Ubuntu (LTS) | 24.04 | 2029-05 | 2032-05 | 3.12 |
| Ubuntu (LTS) | 26.04 | 2031-04 | 2034-04 | 3.13 |
| Debian | 11 (bullseye) (EOL) † | 2024-08 | 2027-08 | 3.9 |
| Debian | 12 (bookworm) | 2026-07 | 2029-07 | 3.11 |
| Debian | 13 (trixie) | 2028-08 | 2031-08 | 3.13 |
| RHEL | 8 | 2029-05 | 2032-05 | 3.8 ‡ |
| RHEL | 9 | 2032-05 | 2035-05 | 3.11 |
| RHEL | 10 | 2035-05 | 2038-05 | 3.13 |
| Alpine | 3.20 (EOL) † | 2026-04 | 2029-04 | 3.12 |
| Alpine | 3.21 | 2026-11 | 2029-11 | 3.13 |
| Alpine | 3.22 | 2027-05 | 2030-05 | 3.13 |
| Alpine | 3.23 | 2027-11 | 2030-11 | 3.14 |
| Alpine | 3.24 | 2028-06 | 2031-06 | 3.14 |

† Past upstream EOL; in FreeUnit grace period.
‡ Default Python shipped by this OS is itself past upstream EOL. FreeUnit does not backport fixes to that Python version.

## Dependency Support

Build-time and bundled libraries. These are **not** matrix variants — nothing in
Docker or CI derives an image from this table — so the 1-year grace rule does not
apply to them. Machine-readable copy: the `dependencies` key in `pkg/eol.json`.

† Bundled software that is already past upstream EOL. It carries no FreeUnit "drops"
date, because the grace policy applies to matrix variants, not to vendored jars; it
needs an upgrade or replacement decision instead.

| Dependency | Role | Floor / Version | Upstream EOL | Notes |
|------------|------|-----------------|--------------|-------|
| OpenSSL | TLS backend (`--openssl`) | 1.1.1 (floor) | Sep 2023 | Kept because RHEL 8, Amazon Linux 2 (`openssl11-devel`) and Debian 11 ship it. Floor declared by the `auto/ssltls` probe (PR #224). |
| OpenSSL | tested ceiling | 3.6.2 (CI build) | Nov 2026 | `build-test.yml` builds it into `/opt/openssl-3.6` (`OPENSSL_VERSION`). **The pin needs a bump before 2026-11-01** (3.5 is the LTS, EOL Apr 2030). |
| OpenSSL | tested ceiling | 4.0.2 (CI build) | May 2027 | The `openssl4` job builds it into `/opt/openssl-4.0` (`OPENSSL4_VERSION`) and compiles with `-DOPENSSL_NO_DEPRECATED`, so any use of an API 4.0 deprecates fails the build. |
| Apache Tomcat | Servlet/JSP/EL API + Jasper jars bundled by the Java module | 9.0.x | Mar 2027 ([no earlier than](https://tomcat.apache.org/whichversion.html)) | Bundled by `auto/modules/java`; independent of the JDK variant. |
| Eclipse Jetty | `jetty-util` / `jetty-server` / `jetty-http` jars bundled by the Java module | 9.4.58.v20250814 | **Aug 2025 (EOL)** † | Jetty 9.4 lost community support on 2025-08-14 and the bundled build is its last release. Needs an upgrade-or-replace decision — Jetty 10/11 are EOL too; 12.x is the supported line. |
| Eclipse ECJ (JDT batch compiler) | JSP compilation jar bundled by the Java module | 3.26.0 | none published | Tracks Eclipse releases; endoflife.date has no product for it. Pinned build is from Jun 2021; current is 3.42.0 (Jun 2025). |
| ClassGraph | classpath scanning jar bundled by the Java module | latest | — | No upstream EOL schedule; pinned and bumped as needed. |

## Rules

- **Adding a runtime version:** when new upstream release reaches stable, FreeUnit adds
  it within one release cycle.
- **Adding an OS version:** when new OS release is available, FreeUnit adds it within
  one release cycle.
- **Dropping a version:** announced at least one release before removal. Noted in
  `CHANGES`.
- **Security-only mode:** versions within 6 months of the FreeUnit drop date receive
  security fixes only — no new features backported.
- **LTS runtimes (Java 17/21/25):** follow the upstream LTS schedule strictly.
- **Dependencies:** the floors in the Dependency Support section are not part of the
  EOL + grace policy; they track what FreeUnit builds and bundles, not what it ships
  as a variant.
- **LTS OS (Ubuntu, RHEL, Debian):** 3-year extension applies to standard EOL, not
  extended security maintenance dates.

## Source of Truth

Machine-readable version data lives in [`pkg/eol.json`](pkg/eol.json).
The Docker CI matrix and RPM packaging are driven by this file.

## Reporting EOL Issues

If a runtime or OS version in the matrix has reached upstream EOL and is not yet listed here,
open an issue with the label `eol` at
[github.com/freeunitorg/freeunit/issues](https://github.com/freeunitorg/freeunit/issues).
