# PHP CVE Map: FreeUnit Docker Images

**Checked:** 2026-10-29
**Scope:** the PHP images of this repository. The repository builds `php-8.3`, `php-8.4`,
and `php-8.5`. The list of versions comes from `pkg/eol.json`.

This document maps the PHP and Brotli CVEs of 2026 to these images.
It also records which mitigations the repository implements and which ones it lacks.

## The CVEs

### PHP runtime

| CVE | Severity | Component | Vulnerable versions | Fixed in |
|-----|----------|-----------|---------------------|----------|
| CVE-2026-109618 | critical — RCE via OOM corruption | php-src | < 8.2.35, < 8.3.36, < 8.4.27, < 8.5.12 | 8.2.35, 8.3.36, 8.4.27, 8.5.12 |
| CVE-2026-109619 | critical — RCE via stale global state | php-src | < 8.2.35, < 8.3.36, < 8.4.27, < 8.5.12 | 8.2.35, 8.3.36, 8.4.27, 8.5.12 |
| CVE-2026-93682 | medium — out-of-bounds read in the HTTP stream wrapper on a redirect with an empty `Location` | ext-standard | < 8.2.34, < 8.3.35, < 8.4.26, < 8.5.11 | 8.2.34, 8.3.35, 8.4.26, 8.5.11 |
| CVE-2026-91765 | high — unbounded recursion in the SOAP server cleanup | ext-soap | < 8.2.34, < 8.3.35, < 8.4.26, < 8.5.11 | 8.2.34, 8.3.35, 8.4.26, 8.5.11 |
| CVE-2026-91766 | medium — cross-origin credential leak on HTTP stream wrapper redirects | ext-standard | < 8.2.34, < 8.3.35, < 8.4.26, < 8.5.11 | 8.2.34, 8.3.35, 8.4.26, 8.5.11 |
| CVE-2026-91768 | medium — IPv6 ACL bypass in `listen.allowed_clients` of FastCGI | PHP-FPM | < 8.2.34, < 8.3.35, < 8.4.26, < 8.5.11 | 8.2.34, 8.3.35, 8.4.26, 8.5.11 |

Two separate batches of fixes exist. The batch of 8.5.11 closes four CVEs.
The batch of 8.5.12 closes two critical RCEs.
PHP 8.5.12 contains both batches, so 8.5.12 is the minimum safe version on the 8.5 branch.

The images of this repository need these PHP versions:

| Image | First safe version |
|-------|---------------------------|
| `php-8.3` | 8.3.36 |
| `php-8.4` | 8.4.27 |
| `php-8.5` | 8.5.12 |

### Brotli library

| CVE | Severity | Component | Vulnerable versions | Fixed in |
|-----|----------|-----------|---------------------|----------|
| CVE-2026-111497 | buffer overflow in the Brotli decoder (`c/dec/decode.c`) | libbrotli | 1.0.3 — 1.2.0 | 1.0.10, 1.1.1, 1.2.1 |

Debian ships the fix as a separate binary upload. The version in `trixie` main is
`1.1.0-2+b7`, and that version is still vulnerable.
Only `trixie-security` carries the fixed `1.1.0-2+deb13u1`.

### CVE-2026-115560

The mitigation for this CVE is an ini file setting: `session.upload_progress.enabled=0`.

The record does not exist in any public database. The identifier is absent from the
CVE.org registry, from the NVD, from the Debian security tracker, and from the
php-src advisory list.
Treat the identifier as unverified until the record is published.

The value `On` in `php -i` marks a vulnerable state.

## State of `pkg/docker/` in this repository

| Mitigation | Status | Evidence |
|------------|--------|----------|
| PHP runtime at 8.3.36 or newer | Partial | The base tag `php:8.3-cli-trixie` floats. It resolves to the newest 8.3.x at build time. The build asserts no version. |
| PHP runtime at 8.4.27 or newer | Partial | The base tag `php:8.4-cli-trixie` floats. It resolves to the newest 8.4.x at build time. The build asserts no version. |
| PHP runtime at 8.5.12 or newer | Partial | The base tag `php:8.5-cli-trixie` floats. It resolves to the newest 8.5.x at build time. The build asserts no version. |
| `session.upload_progress.enabled=0` | Done | The PHP images write `/usr/local/etc/php/conf.d/zz-freeunit-security.ini` at build time. The `RUN_php` value in `pkg/docker/Makefile` creates it. The `zz-` prefix makes it load last in `conf.d`. |
| `libbrotli1` at 1.1.0-2+deb13u1 or newer | Partial | The Dockerfile installs `libbrotli-dev` after `apt-get upgrade`. The version comes from the apt sources of the base image, not from a pin. A build that runs before `trixie-security` publishes the fix keeps the vulnerable library. |
| `pkg/docker/verify-php-cve.sh` | Done | The file checks the PHP version, the ini setting, the `libbrotli1` version and the daemon version of an image. |
| The release workflow runs the check | Done | The step `Verify PHP image (amd64)` or `Verify PHP image (arm64)` of `.github/workflows/release-docker.yml` runs the script for every PHP image after the build. The expected daemon version comes from the release tag. A failing check stops the job, so the merge job creates no final tag and no floating tag. The tag of the per-architecture image stays in the registries. |
| `unitd` version in the published image | 1.37.1 | The workflow reads the version from the tag and passes it as `UNIT_VERSION`. The `version` file of this release names `1.37.1`. The workflow refuses to publish when the `version` file at that tag names a different version. |

The ini file covers every PHP application in the image, also the ones a user adds
later through the control API.

An application config can also carry the setting under `options.admin`, and Unit
passes it to PHP as an ini value.

### The scope of the 1.37.1 release

The release 1.37.1 adds the ini fragment to the PHP images. It also adds the
check to the release workflow. It does not pin the PHP version or the Brotli
version. The ini fragment lowers the risk of CVE-2026-115560. It does not
close the other CVEs of this list. Only a patched PHP version closes them.

### One gap that needs a decision

1. **The version assertions in the build.** A floating base tag resolves to the newest patch of its branch at build time.
   The build asserts no PHP version and no Brotli version.
   The release workflow checks the image after the build, so a release never publishes an image that fails the check.
   A build outside the release workflow is not checked.

## How to check an image

The target `make -C pkg/docker verify IMAGE=<tag>` runs the checks of this
section and reports the result of each one. It exits 1 when a check fails.
The target expects the daemon version of the `version` file of the checkout.
Pass `VERSION=<version>` to check another version.
The release workflow runs the same target for the PHP images after the build.
It passes the version of the release tag.

```sh
make -C pkg/docker verify IMAGE=ghcr.io/freeunitorg/freeunit:latest-php-8.5
```

The script `pkg/docker/verify-php-cve.sh` does the same work. It takes the
image and the expected version of the daemon.

```sh
pkg/docker/verify-php-cve.sh ghcr.io/freeunitorg/freeunit:latest-php-8.5 1.37.1
```

The script checks four values: the PHP version, the ini setting, the
`libbrotli1` version and the daemon version.

The commands below show the same values. The images use the entry point
`docker-entrypoint.sh` at `/usr/local/bin/docker-entrypoint.sh`. The daemon is at `/usr/sbin/unitd`.
The module is at `/usr/lib/unit/modules/php.unit.so`.
The control socket is at `/var/run/control.unit.sock`.

Use `--entrypoint sh` for single commands. The path to the daemon and the package
manager depend on the image.

```sh
I=ghcr.io/freeunitorg/freeunit:latest-php-8.5

docker run --rm --entrypoint sh "$I" -c 'php -v | head -1'
docker run --rm --entrypoint sh "$I" -c "php -i | grep -E '^session\.upload_progress\.enabled'"
docker run --rm --entrypoint sh "$I" -c "v=\$(dpkg-query -W -f='\${Version}' libbrotli1 2>/dev/null); echo libbrotli1=\${v:-absent}"
docker run --rm --entrypoint sh "$I" -c 'ldd /usr/lib/unit/modules/php.unit.so | grep -i brotli || echo "php.unit.so: no brotli"'
docker run --rm --entrypoint sh "$I" -c 'ldd /usr/bin/curl | grep -i brotli || echo "curl: no brotli"'
docker run --rm --entrypoint sh "$I" -c 'unitd --version 2>&1 | head -1'
```

### How to read the results

| Command | Pass | Fail |
|---------|------|------|
| `php -v` | The expected PHP version | Another version |
| `upload_progress.enabled` | `Off` (`=> Off => Off`) | `On`. The ini file is missing from the image. |
| `libbrotli1` | A version at 1.1.0-2+deb13u1 or newer | `absent`, or an older version. The value `absent` needs a separate check on builds from source. |
| `ldd ... brotli` | The lines `libbrotlidec` and `libbrotlicommon` mean that the package version matters | `no brotli` means that the package version does not matter for that binary. |
| `unitd --version` | `unit version: 1.37.1` | An error or another version |

The library `libbrotli1` loads into `curl`, `php`, and `php.unit.so`.
The check used `ldd`.

The result of `round()` is not a gate. Unpatched PHP gives `float(0)` as well.

## What is still missing for acceptance

The checks above show the state of an image. They do not prove that a vulnerability
is closed.

| Requirement | Purpose | Status |
|-------------|---------|--------|
| A regression test for an OOM in a multipart request | The worker PID changes, and the next request returns 200. This is the gate for CVE-2026-109618 and CVE-2026-109619. | Open |
| A run on an unpatched image | The check must fail where the patch is absent. | Done — the script fails on an image without the ini fragment. |
| `php -m` before and after the patch | The set of extensions must not change. | Open |
| A smoke test of the start | The image starts through the entry point, `unitd --no-daemon` comes up, and the port answers. | Done |
| A worker stop on a failed `php_request_startup()` | The PHP module completes the request with an error. It does not stop the worker. | Open |

### The smoke test

The image starts through the entry point, the daemon comes up, and a PHP application
answers a request. Configure the application with one PUT of the whole config.
If the config is still empty, a PUT to `/config/applications/hello` fails with
"Value doesn't exist". The listener key needs the wildcard `*`.

```sh
I=freeunit:1.37.1-php-8.5-hardened
N=fu-smoke

docker rm -f "$N" 2>/dev/null
docker run -d --name "$N" "$I" >/dev/null
sleep 10

docker exec "$N" sh -c 'mkdir -p /var/www/app \
    && echo "<?php echo PHP_VERSION,\"\\n\";" > /var/www/app/index.php'

docker exec -i "$N" curl -s -o /dev/null -X PUT --data-binary @- \
    --unix-socket /var/run/control.unit.sock http://localhost/config <<'EOF'
{"listeners":{"*:8081":{"pass":"applications/hello"}},
 "applications":{"hello":{"type":"php","root":"/var/www/app","index":"index.php"}}}
EOF

sleep 3
docker exec "$N" curl -s http://localhost:8081/
# Expected: 8.5.12

docker rm -f "$N"
```

## Open tasks

| Task | Description | Status |
|------|-------------|--------|
| The `upload_progress` ini file | Add `session.upload_progress.enabled=0` to the PHP images. | Done. `RUN_php` in `pkg/docker/Makefile` writes the ini fragment. |
| The check script | Add `pkg/docker/verify-php-cve.sh` and run it in `release-docker.yml` after the build. | Done. The script and the step in the release workflow are in place. |
| The version assertions | Fail the release when the PHP version or the `libbrotli1` version is below the fixed version. | Partial. The release workflow checks the built image and stops the release. The build itself asserts nothing. |
| The tag of a failed image | Delete the tag of the per-architecture image when the check fails. The cleanup job does not run, because the merge job does not succeed. | Open |
| The worker stop on a failed `php_request_startup()` | Stop the worker in `src/nxt_php_sapi.c` when the request startup fails. | Open |
| The published images | The published images keep their old state until a new release tag appears. | Waiting for a decision |

## Sources

- [php-src advisories](https://github.com/php/php-src/security/advisories)
- [PHP changelog](https://www.php.net/ChangeLog-8.php)
- [Debian security tracker](https://security-tracker.debian.org/tracker/)
- [CVE-2026-109618](https://security-tracker.debian.org/tracker/CVE-2026-109618)
- [CVE-2026-111497](https://security-tracker.debian.org/tracker/CVE-2026-111497)
