# Remi's RPM Repository — FreeUnit Integration

> **Note:** Remi's repository is a community project maintained independently of FreeUnit.
> Packages are built by [Remi Collet](https://blog.remirepo.net/) and are not officially
> supported by the FreeUnit team. Use them with the same trust you extend to Remi's PHP stack.

## Overview

[Remi's RPM repository](https://rpms.remirepo.net/) provides the latest PHP stack for
Fedora and RHEL-based systems. It also ships `unit-php` language modules for each PHP
version it maintains.

**Division of responsibility:**

| Package | Source |
|---------|--------|
| `freeunit` (core server) | `packages.freeunit.org` |
| `unit-php` modules (per PHP version) | Remi's repository |

Remi's `unit-php` modules depend on `unit` (the server binary). FreeUnit packages satisfy
that dependency via `Provides: unit = %{version}`, so no module reinstallation is required
when migrating from the archived NGINX Unit packages.

## Supported Platforms

As of the last verified check (April 2026):

| Platform | Repo path | Latest unit-php version |
|----------|-----------|------------------------|
| Enterprise Linux 9 (RHEL/CentOS Stream 9) | `enterprise/9/phpXX/` | 1.35.0 |
| Fedora (current releases) | `fedora/NN/phpXX/` | 1.35.0 |

PHP versions with confirmed `unit-php` packages: **7.4, 8.0, 8.3, 8.4, 8.5**

> Remi stopped updating `unit-php` at version 1.35.0 (the last NGINX Unit release before
> the project was archived in October 2025). FreeUnit is API/ABI compatible starting from
> that version.

## Migration from NGINX Unit + Remi

If you previously ran `unit` from `packages.nginx.org` with `unit-php` from Remi:

1. Remove the NGINX Unit repository:

   ```bash
   rm /etc/yum.repos.d/unit.repo
   ```

2. Add the FreeUnit repository:

   ```bash
   # See https://docs.freeunit.org/installation/ for the current repo config
   ```

3. Upgrade the core package (Remi's `unit-php` modules stay as-is):

   ```bash
   dnf install freeunit
   systemctl restart unit
   ```

## Fresh Installation

Configure Remi's repository first:
[rpms.remirepo.net — configuration wizard](https://rpms.remirepo.net/wizard/)

Then install FreeUnit core + Remi PHP modules:

```bash
# Install FreeUnit core from packages.freeunit.org
dnf install freeunit

# Install PHP module from Remi — enable the matching per-PHP repo
dnf install --enablerepo=remi-php84 unit-php   # adjust phpXX to match your PHP version
```

Restart to load the new modules:

```bash
systemctl restart unit
```

## Runtime Paths (Remi-packaged)

| Item | Path |
|------|------|
| Control socket | `/run/unit/control.sock` |
| Log | `/var/log/unit/unit.log` |
| Non-privileged user | `nobody` |

## Old Documentation Reference

The original NGINX Unit docs described this integration at:
`https://unit.nginx.org/installation/#community-remisrpm`

That page is archived. Current FreeUnit installation docs:
[docs.freeunit.org/installation](https://docs.freeunit.org/installation/)

## Notes

Last verified: April 2026. Remi's `unit-php` packages were last published at version 1.35.0
and have not been updated since NGINX Unit was archived. If Remi does not resume updates,
native `freeunit-php` modules via `packages.freeunit.org` are the planned fallback.
Track progress at [github.com/freeunitorg/freeunit/issues](https://github.com/freeunitorg/freeunit/issues).
