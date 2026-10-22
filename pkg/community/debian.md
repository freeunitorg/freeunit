# Debian 13 (Trixie) / Ubuntu 24.04 LTS (Noble) — Build from Source

> Verified on:
> - Debian 13 Trixie (kernel `6.12.x`), OpenSSL 3.5.7, FreeUnit 1.37.0 with PHP 8.5.12 embed SAPI ✓
> - Ubuntu 24.04 LTS (Noble): same steps, with the Ubuntu notes below. Not yet tested by the author.
>
> Last verified on Debian 13: October 22, 2026.
> PHP 8.5.12 fixes two critical RCE bugs (CVE-2026-109618, CVE-2026-109619).
> Do not use PHP 8.5.11 or older.
>
> | PHP branch | First safe release | Unsafe releases |
> |------------|--------------------|-----------------|
> | 8.5 | 8.5.12 | 8.5.11 and older |
> | 8.4 | 8.4.27 | 8.4.26 and older |
> | 8.3 | 8.3.36 | 8.3.35 and older |
> | 8.2 | 8.2.35 | 8.2.34 and older |
>
> Use the first safe release or a newer one. The PHP project does not fix PHP 8.1 and older. Do not use them.
> PHP 8.2 reaches end of life on 31 December 2026.
>
> Distribution packages:
> - Debian 13 (Trixie): `php8.4` 8.4.27-1~deb13u1 or newer (DSA-6574-1). Version 8.4.26 is not safe.
> - Ubuntu 24.04 (Noble): see USN-8948-1 for the fixed `php8.3` version. Make sure that your package is at least that version.
>
> Sources: [PHP changelog](https://www.php.net/ChangeLog-8.php),
> [Debian security tracker](https://security-tracker.debian.org/tracker/CVE-2026-109618).

Debian 13 is the main target. Ubuntu differences are marked **Ubuntu**.

## Quick Start (TL;DR) — PHP 8.5 from packages

**Debian 13 (Trixie):**

```bash
# 1. Add deb.sury.org and install PHP 8.5
sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg lsb-release
sudo curl -fsSL https://packages.sury.org/php/apt.gpg -o /usr/share/keyrings/sury-php.gpg
CODENAME=$(lsb_release -sc)    # trixie
sudo tee /etc/apt/sources.list.d/sury-php.sources >/dev/null <<SURY
Types: deb
URIs: https://packages.sury.org/php/
Suites: ${CODENAME}
Components: main
Signed-By: /usr/share/keyrings/sury-php.gpg
SURY
sudo apt-get update
sudo apt-get install -y php8.5-dev libphp8.5-embed build-essential libssl-dev \
    libpcre2-dev zlib1g-dev libzstd-dev libbrotli-dev
```

**Ubuntu 24.04 (Noble):** use the Sury PPA instead of the lines above.

```bash
sudo apt-get update
sudo apt-get install -y software-properties-common
sudo add-apt-repository -y ppa:ondrej/php
sudo apt-get install -y php8.5-dev libphp8.5-embed build-essential libssl-dev \
    libpcre2-dev zlib1g-dev libzstd-dev libbrotli-dev
```

Then, on both systems:

```bash
# 2. Build FreeUnit + PHP module
git clone https://github.com/freeunitorg/freeunit.git && cd freeunit
git checkout 1.37.0
./configure --prefix=/usr --libdir=/usr/lib/x86_64-linux-gnu \
    --statedir=/var/lib/unit --logdir=/var/log/unit \
    --runstatedir=/run/unit --control=unix:/run/unit/control.sock \
    --openssl --zlib --brotli --zstd \
    --modulesdir=/usr/lib/x86_64-linux-gnu/unit/modules
make -j$(nproc)
sudo make install
./configure php --config=php-config8.5
make php && sudo make php-install

# 3. Create the systemd unit (see "systemd Service" below), then:
sudo systemctl daemon-reload
sudo systemctl enable --now unit
systemctl status unit
```

> `make install` does **not** install a systemd unit. You must create it yourself.

## Prerequisites

```bash
sudo apt-get update

# Build tools and essential libraries
sudo apt-get install -y build-essential gcc make git \
    libssl-dev libpcre2-dev zlib1g-dev libzstd-dev libbrotli-dev

# For OTEL support: Rust >= 1.88 (curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh)
```

### PHP version options

| Source | PHP versions | Notes |
|--------|-------------|-------|
| Debian native | 8.4 (Trixie) | Needs the embed package |
| Ubuntu native | 8.3 (24.04 Noble) | Needs the embed package |
| deb.sury.org | 7.4 — 8.5 | Debian only |
| Sury PPA (`ppa:ondrej/php`) | 7.4 — 8.5 | Ubuntu only |
| Build from source | Any PHP version | Full control, longer build |

### Option A: native distribution PHP

```bash
# Debian 13 installs PHP 8.4, Ubuntu 24.04 installs PHP 8.3
sudo apt-get install -y php php-cli php-dev php-mbstring \
    php-mysql php-xml php-curl

# Embed SAPI (required for FreeUnit)
sudo apt-get install -y libphp-embed

php -v
php-config --version
# Debian 13 output: PHP 8.4.x
```

> **Note:** `php-sodium` is not available in Debian 13 repos — omit it.
> The `libphp-embed` package is **required** to build the PHP module.

Build the FreeUnit PHP module (after the FreeUnit build from the "Build" section):

```bash
./configure php
make -j$(nproc) php
sudo make php-install

ls -lh /usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so
```

### Option B: Sury packages (PHP 8.5)

**Recommended.** The official [build-deb.yml workflow](https://github.com/freeunitorg/freeunit/blob/master/.github/workflows/build-deb.yml)
uses `setup_sury_if_needed "8.3 8.4 8.5"` on Debian trixie.

- **Debian 13:** `deb.sury.org` (`packages.sury.org/php`).
- **Ubuntu 24.04:** `ppa:ondrej/php`. This is a different source from `deb.sury.org`.

```bash
# 1. Set up the repository (see Quick Start for Debian and Ubuntu commands)

# 2. Install PHP 8.5
sudo apt-get install -y php8.5 php8.5-cli php8.5-dev php8.5-mbstring \
    php8.5-mysql php8.5-xml php8.5-curl libphp8.5-embed

php8.5 -v
# Output: PHP 8.5.12 (cli) ...

# 3. Build the FreeUnit module against PHP 8.5
cd /path/to/freeunit
./configure php --config=php-config8.5
make -j$(nproc) php
sudo make php-install

ls -lh /usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so
```

> **Why Sury?** Prebuilt, updated by `apt`, same method as the official CI.

### Option C: Build PHP from source (Advanced)

Use this only if you need a PHP version that Sury does not ship, or custom compile flags.
It works the same on Debian 13 and Ubuntu 24.04.

```bash
# Build dependencies
sudo apt-get install -y libxml2-dev libsqlite3-dev libcurl4-openssl-dev \
    liboniguruma-dev libtidy-dev libargon2-dev libsodium-dev \
    autoconf automake libtool pkg-config bison re2c

git clone https://github.com/php/php-src.git php-src
cd php-src
git checkout php-8.5.12

./buildconf --force
./configure --prefix=/opt/php-8.5 \
    --with-config-file-path=/etc/php/8.5 \
    --enable-fpm --enable-cli --enable-cgi --enable-embed \
    --enable-mbstring \
    --with-curl --with-openssl --with-zlib --with-sodium \
    --with-mysqli=mysqlnd --with-pdo-mysql=mysqlnd \
    --enable-xml

make clean && make -j$(nproc)
sudo make install

/opt/php-8.5/bin/php -v
# Output: PHP 8.5.12 (cli) ...

# The embed SAPI library must exist
ls -lh /opt/php-8.5/lib/libphp.so

# Point the system libphp symlink to PHP 8.5
sudo ln -sf /opt/php-8.5/lib/libphp.so /lib/libphp.so

# Build the FreeUnit module against PHP 8.5
cd ../freeunit
./configure php --config=/opt/php-8.5/bin/php-config
make -j$(nproc) php
sudo make php-install

# Check the link
ldd /usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so | grep libphp
# Output: libphp.so => /lib/libphp.so -> /opt/php-8.5/lib/libphp.so

# Restart FreeUnit to load the new module
sudo systemctl restart unit
```

**Build notes:**
- `--enable-embed` is **required**. It builds the embed SAPI (`libphp.so`) that FreeUnit loads.
- PHP 8.5.12 build: ~3 min (incremental), ~8 min (full clean build) on 8 CPU cores.
- FreeUnit loads `libphp.so` at runtime. If you build several PHP versions, point `/lib/libphp.so` to the one you want.

**Troubleshooting:**
- `no PHP embed SAPI found` → add `--enable-embed` to the PHP configure and rebuild.
- FreeUnit shows an old PHP version → check `ls -la /lib/libphp.so` and `ldd .../php.unit.so`.

### Comparison

| Feature | A: Native | B: Sury | C: Source |
|---------|-----------|---------|-----------|
| **PHP version** | 8.4 (Debian 13), 8.3 (Ubuntu 24.04) | **8.5** | 8.5.12 or any |
| **Setup time** | 1 min | **2 min** | 5–10 min |
| **Updates** | `apt upgrade` | **`apt upgrade`** | Manual rebuild |
| **System integration** | `/usr/bin` | **`/usr/bin`** | `/opt/php-8.5` |
| **Used by official CI** | No | **Yes** | No |

**Recommendation:**
- PHP 8.4 (Debian) or 8.3 (Ubuntu) is enough → Option A.
- PHP 8.5 → **Option B**.
- Custom flags or other version → Option C.

## Build

```bash
git clone https://github.com/freeunitorg/freeunit.git
cd freeunit

git checkout 1.37.0   # or master

./configure --prefix=/usr \
    --libdir=/usr/lib/x86_64-linux-gnu \
    --statedir=/var/lib/unit \
    --logdir=/var/log/unit \
    --runstatedir=/run/unit \
    --control=unix:/run/unit/control.sock \
    --openssl --zlib --brotli --zstd \
    --modulesdir=/usr/lib/x86_64-linux-gnu/unit/modules

make -j$(nproc)
```

Run the common `./configure` first. A language module `./configure php` needs it.

> **Note on libdir:** Debian and Ubuntu use `/usr/lib/x86_64-linux-gnu` (multiarch) instead of `/usr/lib64`.
>
> **OTEL support** is optional. It needs Rust ≥ 1.88. Add `--otel` to the configure flags to enable it.
>
> `--user` / `--group` are omitted because the systemd service runs unitd directly.
> To restrict the runtime user, add `User=unit` and `Group=unit` to the `[Service]` section,
> and create the user: `sudo useradd -r -s /usr/sbin/nologin unit`.

### PHP module

FreeUnit loads PHP through the module `php.unit.so`. The module links to the embed library `libphp.so`.
You must build the module against the PHP version that you want to run.

1. Install the PHP embed package, or build PHP with `--enable-embed` (see Options A, B, and C).
2. Run the common `./configure` from the "Build" section. Do this one time.
3. Configure the module. Use the `php-config` of your PHP version:

   ```bash
   # Native PHP (Debian 13: 8.4, Ubuntu 24.04: 8.3)
   ./configure php

   # Sury PHP 8.5
   ./configure php --config=php-config8.5

   # PHP 8.5 built from source
   ./configure php --config=/opt/php-8.5/bin/php-config
   ```

4. Build and install the module:

   ```bash
   make -j$(nproc) php
   sudo make php-install
   ```

5. Restart the service: `sudo systemctl restart unit`.
6. Make sure that the module is loaded. Run the command from the "Verify" section.

If you update PHP, build the module again. The module does not follow the PHP version by itself.
If you run `./configure php` a second time, the common `./configure` must run first.
Otherwise the command stops with `duplicate php module configured`.

### Language modules (non-PHP)

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

Files:
- `/usr/sbin/unitd` — daemon
- `/usr/lib/x86_64-linux-gnu/unit/modules/*.unit.so` — language modules

## systemd Service

`make install` does not create a unit. Create it before you enable the service:

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
Restart=on-failure
RestartSec=3s

[Install]
WantedBy=multi-user.target
EOF

sudo mkdir -p /var/log/unit /var/lib/unit
sudo systemctl daemon-reload
sudo systemctl enable --now unit
```

### Why `Type=simple` + `--no-daemon`

The packaged units in `pkg/deb` use `Type=forking` with `PIDFile=`. This unit uses
`--no-daemon` instead, so unitd stays a direct child of systemd. No PID file is needed.

`RuntimeDirectory=unit` creates `/run/unit` on start and removes it on stop.

### Optional resource limits

Add these to `[Service]` only if you need to cap the service. They can slow a busy server:

```ini
CPUQuota=70%
TasksMax=512
Nice=5
```

Always use `systemctl restart unit` to restart. Do not use `pkill unitd`; systemd will fight it.

## Verify

```bash
systemctl status unit

# Loaded modules are listed in /status (not /config)
curl --unix-socket /run/unit/control.sock http://localhost/status

# Expected output includes:
#   "modules": { "php": { "version": "8.x.y", "lib": "/usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so" } }
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

### Local hostname testing (dev/testing)

To test with a local hostname (for example `myapp.local`):

```bash
# 1. Add to /etc/hosts
sudo tee -a /etc/hosts <<< "127.0.0.1 myapp.local"

# 2. Create app directory
sudo mkdir -p /var/www/myapp.local
echo '<?php phpinfo(); ?>' | sudo tee /var/www/myapp.local/phpinfo.php

# 3. Configure FreeUnit
curl -X PUT --unix-socket /run/unit/control.sock http://localhost/config/applications/myapp << 'EOF'
{
    "type": "php",
    "root": "/var/www/myapp.local"
}
EOF

curl -X PUT --unix-socket /run/unit/control.sock http://localhost/config/listeners/'*:8080' << 'EOF'
{
    "pass": "applications/myapp"
}
EOF

# 4. Test
curl http://myapp.local:8080/phpinfo.php
# Expected output: PHP Version 8.5.12 (or your configured PHP version)
```

FreeUnit loads `libphp.so` at startup. Make sure `/lib/libphp.so` points to the PHP version you want.

## Logs

```bash
tail -f /var/log/unit/unit.log
```

### Access log

```bash
curl -X PUT --unix-socket /run/unit/control.sock \
    http://localhost/config/access_log << 'EOF'
{
    "path": "/var/log/unit/access.log",
    "format": "$remote_addr - - [$time_local] \"$request_line\" $status $body_bytes_sent \"$header_referer\" \"$header_user_agent\""
}
EOF

tail -f /var/log/unit/access.log
```

## Paths Summary

| Item | Path |
|------|------|
| Daemon | `/usr/sbin/unitd` |
| Control socket | `/run/unit/control.sock` |
| State | `/var/lib/unit/` |
| Log | `/var/log/unit/unit.log` |
| Modules | `/usr/lib/x86_64-linux-gnu/unit/modules/*.unit.so` |

## Comparison with other distributions

| Distro | PHP 8.5 method | Package system | See also |
|--------|----------------|---|---|
| **Debian 13** | deb.sury.org | apt | This doc (Option B) |
| **Ubuntu 24.04** | `ppa:ondrej/php` | apt | This doc (Option B) |
| **CentOS/AlmaLinux** | Remi repository (`dnf module enable php:remi-8.5`) | dnf | [CENTOS.ALMALINUX.md](CENTOS.ALMALINUX.md) |
| **Remi details** | `unit-php` prebuilt | dnf | [REMI.md](REMI.md) |

All of them use prebuilt PHP packages, which is also what the official CI does.

## Migration from NGINX Unit

If you upgrade from the archived NGINX Unit `.deb` packages:

```bash
sudo systemctl stop unit
sudo apt-get remove unit -y

# (Optional) Remove old state
# sudo rm -rf /var/lib/unit/*

# Then build and install FreeUnit (steps above)
```

The control socket path can differ between package versions.
If you have scripts that use the old path, find them:

```bash
grep -r 'unit.sock\|unit/control' /etc/ /srv/ /home/ --include='*.sh' --include='*.conf' -l 2>/dev/null
```

## Troubleshooting

**PHP embed SAPI not found when building the PHP module:**
```bash
# Error: "no PHP embed SAPI found"
sudo apt-get install -y libphp-embed    # or libphp8.5-embed for Sury

./configure php
make php
sudo make php-install
```

**`unitd` won't start, "address already in use":**
```bash
sudo ss -tlnp | grep 8080
# Stop the other process or change the listener port
```

**PHP module not found:**
```bash
ls /usr/lib/x86_64-linux-gnu/unit/modules/
# Expected: php.unit.so
# If empty:
cd /path/to/freeunit
./configure php && make php && sudo make php-install
```

**`Please run common ./configure before configuring module php`:**
```bash
# Run the common configure (see "Build") first, then the module configure.
```

**Permission denied on control socket:**
```bash
ls -la /run/unit/control.sock
# Run curl with sudo
```

**PHP module version mismatch:**
```bash
php -v
# If PHP was updated after the module was built, rebuild:
./configure php && make php && sudo make php-install
sudo systemctl restart unit
```

**Building against custom PHP (`/opt/php-*`):**
```bash
export LD_LIBRARY_PATH=/opt/php-8.5/lib:$LD_LIBRARY_PATH
./configure php --config=/opt/php-8.5/bin/php-config
make php
sudo make php-install
```

**FreeUnit shows the wrong PHP version after a module build:**
```bash
# Cause: /lib/libphp.so still points to an old PHP version
ldd /usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so | grep libphp
ls -la /lib/libphp.so

# Fix: update the symlink, then restart
sudo ln -sf /opt/php-8.5/lib/libphp.so /lib/libphp.so
sudo systemctl restart unit

curl http://localhost:8080/phpinfo.php | grep "PHP Version"
```
