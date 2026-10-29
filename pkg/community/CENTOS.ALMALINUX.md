# CentOS Stream 9 / AlmaLinux 9 — Build from Source

> Verified on:
> - CentOS Stream 9 (rolling), OpenSSL 3.5.x
> - AlmaLinux 9.8, kernel `5.14.0-687.61.1.el9_8`, OpenSSL 3.5.8
>
> FreeUnit 1.37.1, PHP 8.5.12 from Remi modular, `unit-php` 1.35.0.
> Last verified: October 2026.

## PHP security fixes

PHP 8.5.12, 8.4.27, 8.3.36, and 8.2.35 fix two critical RCE bugs
(CVE-2026-109618, CVE-2026-109619).
Do not use PHP 8.5.11, 8.4.26, 8.3.35, or 8.2.34.

| PHP branch | First safe release | Unsafe releases |
|------------|--------------------|-----------------|
| 8.5 | 8.5.12 | 8.5.11 and older |
| 8.4 | 8.4.27 | 8.4.26 and older |
| 8.3 | 8.3.36 | 8.3.35 and older |
| 8.2 | 8.2.35 | 8.2.34 and older |

Use the first safe release or a newer one.
The PHP project does not fix PHP 8.1 and older. Do not use it.

Remi publishes these releases for EL9.
The EL9 AppStream repository also ships the fixed 8.2.35 and 8.3.36 packages.

Sources: [PHP changelog](https://www.php.net/ChangeLog-8.php),
[Remi repository](https://rpms.remirepo.net/enterprise/9/).

## Prerequisites

```bash
# brotli-devel is in EPEL, not in the base EL9 repositories
dnf install -y epel-release

dnf install -y gcc make git openssl-devel pcre2-devel \
    zlib-devel libzstd-devel brotli-devel
# For OpenTelemetry support: Rust >= 1.95 (rustup.rs)
```

### PHP version options

| Source | PHP versions | Notes |
|--------|-------------|-------|
| EL9 AppStream (native) | 8.1, 8.2, 8.3 | You do not need Remi. Use 8.2 or 8.3. |
| Remi modular | 7.4 — 8.5 | PHP 8.4 and 8.5 require Remi. |

### Option A: native AppStream PHP (8.1–8.3)

```bash
# Enable the desired PHP stream
dnf module reset php -y
dnf module enable php:8.3 -y

dnf install -y php php-cli php-devel php-embedded php-mbstring \
    php-mysqlnd php-pdo php-xml

# Build the PHP module from source (no unit-php package in AppStream)
./configure php
make -j$(nproc)
sudo make php-install
```

AlmaLinux 9.8 AppStream ships PHP 8.1.32, 8.2.35, and 8.3.36.
Use 8.2 or 8.3.

> **Note:** EL9 AppStream has no `php-sodium` package. The sodium extension is absent
> from these builds. If your application needs the sodium extension, use the Remi repository.

### Option B: Remi PHP 8.4/8.5 + `unit-php` package

```bash
dnf install -y epel-release
dnf install -y https://rpms.remirepo.net/enterprise/remi-release-9.rpm
dnf module reset php -y
dnf module enable php:remi-8.5 -y

# unit-php lives in the non-modular remi repository. The module command does not
# activate that repository. Enable it explicitly so dnf can resolve unit-php.
dnf config-manager --enable remi

dnf install -y php php-cli php-devel php-embedded php-mbstring \
    php-mysqlnd php-pdo php-xml php-sodium unit-php
```

Remi's `unit-php` 1.35.0 is the last published version (checked October 2026).
Remi built it from the archived `nginx/unit` source.
The module is ABI-compatible with FreeUnit. It loads and works correctly.

See [REMI.md](REMI.md) for details.

### Option C: Remi PHP 8.4/8.5 + build from source

```bash
# EPEL may be needed for some PHP dependencies
dnf install -y epel-release

dnf install -y https://rpms.remirepo.net/enterprise/remi-release-9.rpm
dnf module reset php -y
dnf module enable php:remi-8.5 -y

dnf install -y php php-cli php-devel php-embedded php-mbstring \
    php-mysqlnd php-pdo php-xml php-sodium

./configure php
make -j$(nproc)
sudo make php-install
```

The resulting `php.unit.so` links against Remi's `libphp-8.5.so` but is
compiled from FreeUnit source. The FreeUnit version and the PHP version match exactly.

### Comparison

| | A: AppStream PHP | B: Remi `unit-php` | C: Remi + build |
|---|---|---|---|
| PHP version | 8.1–8.3 | 8.4 or 8.5 | 8.4 or 8.5 |
| External repository | None | Remi | Remi |
| PHP module | Build from source | `dnf install` | Build from source |
| FreeUnit version match | Exact | ABI-compatible | Exact |
| `dnf update` risk | PHP ABI change | None | PHP ABI change |

## Build

```bash
git clone https://github.com/freeunitorg/freeunit.git
cd freeunit

git checkout 1.37.1   # or master / the latest tag

./configure --prefix=/usr \
    --libdir=/usr/lib64 \
    --statedir=/var/lib/unit \
    --logdir=/var/log/unit \
    --runstatedir=/run/unit \
    --control=unix:/run/unit/control.sock \
    --openssl --otel --zlib --brotli --zstd \
    --modulesdir=/usr/lib64/unit/modules

make -j$(nproc)
```

> **Note:** The release tag of this version is `1.37.1`. Use `git checkout 1.37.1`.
> The release tags have no `v` prefix, except the tag `v1.35.1`.
> The `master` branch contains the unreleased changes.

> **Note:** `--otel` requires Rust >= 1.95
> (`curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh`).
> The official builds use Rust 1.95.0.
> If you do not need OpenTelemetry support, remove `--otel`.
>
> `--user` / `--group` are omitted because the systemd service runs unitd directly.
> To restrict the runtime user, add `User=unit` and `Group=unit` to the `[Service]`
> section, and create the user: `useradd -r -s /sbin/nologin unit`.

### Language modules (non-PHP)

PHP module setup varies by option above. For other languages:

```bash
# Python
./configure python --config=python3-config
make -j$(nproc) python3-install

# Go, Node.js, Ruby, Perl, Java, WASM — see ./configure --help
```

## Install

```bash
sudo make install
```

Binaries land at:
- `/usr/sbin/unitd` — release daemon
- `/usr/sbin/unitd-debug` — debug daemon (only with `--debug`)
- `/usr/lib64/unit/modules/*.unit.so` — language modules

## systemd Service

```bash
sudo tee /etc/systemd/system/unit.service << 'EOF'
[Unit]
Description=FreeUnit Application Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/sbin/unitd --no-daemon \
    --log /var/log/unit/unit.log \
    --statedir /var/lib/unit \
    --control unix:/run/unit/control.sock
ExecReload=/bin/kill -HUP $MAINPID
RuntimeDirectory=unit
RuntimeDirectoryMode=0755
TimeoutStartSec=30s
LimitNOFILE=65535
LimitCORE=infinity
Restart=on-failure
RestartSec=3s
CPUQuota=70%
TasksMax=512
Nice=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now unit
```

### Why `Type=simple` + `--no-daemon`

The original NGINX Unit packages used `Type=forking` with `PIDFile=`.
That setup creates a race between systemd and unitd writing the PID file.
`--no-daemon` keeps the process attached to systemd as a direct child process.
There is no PID file, and the lifecycle has no race condition.

`RuntimeDirectory=unit` ensures `/run/unit` is created on start and removed on stop.

### Why resource limits

`CPUQuota`, `TasksMax`, and `Nice` are a safety net after past incidents where
runaway worker processes consumed excessive resources.
Keep them enabled for observability until the root causes are confirmed fixed
across all workloads.

## Verify

```bash
systemctl status unit

# Check loaded modules
curl --unix-socket /run/unit/control.sock http://localhost/status

# Expected output includes "modules" with loaded language modules:
#   "modules": { "php": { "version": "8.x.y", "lib": "/usr/lib64/unit/modules/php.unit.so" } }
```

## Basic Application Setup

```bash
# Create a PHP application
sudo mkdir -p /var/www/app
echo '<?php echo "Hello from FreeUnit\n";' | sudo tee /var/www/app/index.php

# Configure via REST API
curl -X PUT --unix-socket /run/unit/control.sock http://localhost/config/applications/hello << 'EOF'
{
    "type": "php",
    "root": "/var/www/app",
    "index": "index.php"
}
EOF

curl -X PUT --unix-socket /run/unit/control.sock http://localhost/config/listeners/'*:8080' << 'EOF'
{
    "pass": "applications/hello"
}
EOF

# Test
curl http://localhost:8080/
```

## Logs

```bash
tail -f /var/log/unit/unit.log
```

### Access log

Unit supports per-config access logs via the `access_log` directive:

```bash
# Add access log to config
curl -X PUT --unix-socket /run/unit/control.sock \
    http://localhost/config/access_log << 'EOF'
{
    "path": "/var/log/unit/access.log",
    "format": "$remote_addr - - [$time_local] \"$request_line\" $status $body_bytes_sent \"$header_referer\" \"$header_user_agent\""
}
EOF
```

```bash
tail -f /var/log/unit/access.log
```

## Paths Summary

| Item | Path |
|------|------|
| Daemon | `/usr/sbin/unitd` |
| Control socket | `/run/unit/control.sock` |
| State | `/var/lib/unit/` |
| Log | `/var/log/unit/unit.log` |
| Modules | `/usr/lib64/unit/modules/*.unit.so` |

## Migration from NGINX Unit

If you upgrade from the archived NGINX Unit RPM packages:

```bash
# Stop old service
sudo systemctl stop unit

# Remove old repo
sudo rm /etc/yum.repos.d/unit.repo

# Build and install FreeUnit (steps above)
```

The control socket path changed from the old RPM default
(`/var/run/unit/control.unit.sock`) to `/run/unit/control.sock`.
Update any scripts that hardcode the old path:

```bash
# Find references to old socket path
grep -r 'control.unit.sock' /etc/ /srv/ /root/ --include='*.sh' --include='*.conf' --include='*.php' -l 2>/dev/null
```

### Remi `unit-php` compatibility

FreeUnit is API/ABI compatible with NGINX Unit 1.35.0 and newer.
The existing `unit-php` packages from Remi's repository work without reinstalling.

FreeUnit does not publish RPM packages yet. The official packages are planned.
The RPM in `pkg/rpm` declares `Provides: unit = %{version}`, so dnf resolves the
Remi module dependency.

Build that RPM with `make -C pkg rpm` on an EL9 host.

## Troubleshooting

**`unitd` won't start, "address already in use":**
```bash
sudo ss -tlnp | grep 8080
# Kill stale process or change listener port
```

**PHP module not found:**
```bash
ls /usr/lib64/unit/modules/
# If empty, rebuild: ./configure php && make php-install && sudo make install
```

**Permission denied on control socket:**
```bash
ls -la /run/unit/control.sock
# Run curl with sudo, or add user to the appropriate group
```

**`dnf install unit-php` fails to find the package:**
```bash
# The package lives in the non-modular remi repository
sudo dnf config-manager --enable remi
sudo dnf install unit-php
```

## See also

- [REMI.md](REMI.md) — Remi's RPM repository and the `unit-php` module
- [debian.md](debian.md) — Debian and Ubuntu build guide
- [PHP.CVE.md](PHP.CVE.md) — PHP and Brotli CVEs of 2026 and the Docker image state
- [FreeUnit documentation](https://docs.freeunit.org)
