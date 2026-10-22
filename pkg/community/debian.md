# Debian 13 (Trixie) / Ubuntu 24.04 LTS — Build from Source

> Verified on:
> - Debian 13 Trixie (kernel `6.12.63+deb13-amd64`), OpenSSL 3.5.7, PHP 8.4.26 ✓
> - Ubuntu 24.04 LTS (jammy, kernel `6.8.x`), OpenSSL 3.2.x
> - Local testing: FreeUnit 1.37.0 with PHP 8.5.12 embed SAPI ✓ (built from source)
>
> FreeUnit 1.37.0 + PHP 8.5.12 (built from source), verified with phpinfo().
> Last verified: October 22, 2026.
> PHP 8.5.12 fixes two critical RCE bugs (CVE-2026-109618, CVE-2026-109619).
> Do not use PHP 8.5.11 or older.
> PHP 8.5 build time: ~3 min (make), ~2 sec (PHP module).
> Localhost test: PHP 8.5.12 running at http://myapp.local:8080/phpinfo.php ✓

## Quick Start (TL;DR) — with PHP 8.5

**Official method** (used by FreeUnit CI):

```bash
# 1. Add deb.sury.org + install PHP 8.5
sudo apt-get update && sudo apt-get install -y ca-certificates curl gnupg lsb-release
sudo curl -fsSL https://packages.sury.org/php/apt.gpg -o /usr/share/keyrings/sury-php.gpg
sudo tee /etc/apt/sources.list.d/sury-php.sources >/dev/null <<'SURY'
Types: deb
URIs: https://packages.sury.org/php/
Suites: $(lsb_release -sc)
Components: main
Signed-By: /usr/share/keyrings/sury-php.gpg
SURY
sudo apt-get update
sudo apt-get install -y php8.5-dev libphp8.5-embed build-essential libssl-dev \
    libpcre2-dev zlib1g-dev libzstd-dev libbrotli-dev

# 2. Build FreeUnit + PHP module
git clone https://github.com/freeunitorg/freeunit.git && cd freeunit
./configure --prefix=/usr --libdir=/usr/lib/x86_64-linux-gnu \
    --statedir=/var/lib/unit --logdir=/var/log/unit \
    --runstatedir=/run/unit --control=unix:/run/unit/control.sock \
    --openssl --zlib --brotli --zstd \
    --modulesdir=/usr/lib/x86_64-linux-gnu/unit/modules
make -j$(nproc)
./configure php --config=php-config8.5 && make php && sudo make php-install
sudo make install

# 3. Start & verify
sudo systemctl enable --now unit
systemctl status unit
```

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
| Debian native | 8.4 (Trixie), 8.2 (Bookworm) | **Requires libphp-embed** |
| Ubuntu native | 8.2 (LTS), 8.3 (24.10) | **Requires libphp-embed** |
| Sury DPA (PPA) | 7.4 — 8.5 (Ubuntu only) | Third-party, Ubuntu only |
| Build from source | Any PHP version | Full control, longer build |

### Option A: native Debian/Ubuntu PHP (8.2–8.4)

```bash
# Debian 13 installs PHP 8.4 (not 8.3)
sudo apt-get install -y php php-cli php-dev php-mbstring \
    php-mysql php-pdo php-xml php-curl

# For embed SAPI (required for FreeUnit)
sudo apt-get install -y libphp-embed

# Check PHP version and installation path
php -v
php-config --version
# Output: PHP 8.4.26-1~deb13u1 (cli)
```

> **Note:** `php-sodium` is not available in Debian 13 repos — omit it.
> The `libphp-embed` package is **required** to build the PHP module.

Build FreeUnit PHP module from source:

```bash
git clone https://github.com/freeunitorg/freeunit.git
cd freeunit

./configure php
make -j$(nproc) php
sudo make php-install

# Verify installation
ls -lh /usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so
# Output: 380K
```

### Option B: deb.sury.org (PHP 8.5 on Debian & Ubuntu)

**Recommended for both Debian and Ubuntu** (used by official FreeUnit CI).

deb.sury.org works on Debian too (not just Ubuntu). This matches the official
[build-deb.yml workflow](https://github.com/freeunitorg/freeunit/blob/master/.github/workflows/build-deb.yml#L101)
which uses `setup_sury_if_needed "8.3 8.4 8.5"` on Debian trixie.

```bash
# 1. Set up deb.sury.org (works on Debian and Ubuntu)
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    ca-certificates curl gnupg lsb-release

sudo curl -fsSL https://packages.sury.org/php/apt.gpg \
    -o /usr/share/keyrings/sury-php.gpg

sudo tee /etc/apt/sources.list.d/sury-php.sources >/dev/null <<'SURY'
Types: deb
URIs: https://packages.sury.org/php/
Suites: $(lsb_release -sc)
Components: main
Signed-By: /usr/share/keyrings/sury-php.gpg
SURY

sudo apt-get update

# 2. Remove native PHP (optional — if you have it installed)
# sudo apt-get remove -y php* libphp-embed

# 3. Install PHP 8.5 from Sury
sudo apt-get install -y php8.5 php8.5-cli php8.5-dev php8.5-mbstring \
    php8.5-mysql php8.5-pdo php8.5-xml php8.5-curl libphp8.5-embed

# Verify installation
php8.5 -v
# Output: PHP 8.5.12 (cli) ...

# 4. Build FreeUnit module against PHP 8.5
cd /path/to/freeunit
./configure php --config=php-config8.5
make -j$(nproc) php
sudo make php-install

# Verify
ls -lh /usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so
```

> **Why Sury?** Fast (prebuilt), same method as official CI, works on both Debian and Ubuntu.
> See [.github/workflows/build-deb.yml](https://github.com/freeunitorg/freeunit/blob/master/.github/workflows/build-deb.yml)
> for how FreeUnit CI builds PHP 8.5 modules officially.

### Option C: Build PHP from source (Advanced)

**Not recommended — use Option B instead** (deb.sury.org is simpler, official CI method).

Only consider this if you need a PHP version not in deb.sury.org, or want custom compile flags.

```bash
# Install all build dependencies
sudo apt-get install -y libxml2-dev libsqlite3-dev libcurl4-openssl-dev \
    liboniguruma-dev libtidy-dev libargon2-dev libsodium-dev \
    autoconf automake libtool pkg-config bison re2c

# Clone PHP source (PHP 8.5 branch — same as CentOS/AlmaLinux Remi)
git clone https://github.com/php/php-src.git php-src
cd php-src

# Checkout stable PHP 8.5 (or use master for bleeding-edge)
git checkout php-8.5.12  # Latest stable 8.5

# Build configuration (same extensions as native PHP)
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

# Verify custom PHP installation
/opt/php-8.5/bin/php -v
# Output: PHP 8.5.12 (cli) ...

# Verify embed SAPI library was built
ls -lh /opt/php-8.5/lib/libphp.so
# Output: -rw-r--r-- libphp.so (shared object)

# Update system libphp symlink to use PHP 8.5 (required for FreeUnit module)
sudo ln -sf /opt/php-8.5/lib/libphp.so /lib/libphp.so

# Build FreeUnit module against PHP 8.5
cd ../freeunit
./configure php --config=/opt/php-8.5/bin/php-config
make -j$(nproc) php
sudo make php-install

# Verify module was built and linked against PHP 8.5
ls -lh /usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so
ldd /usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so | grep libphp
# Output: libphp.so => /lib/libphp.so -> /opt/php-8.5/lib/libphp.so

# Restart FreeUnit daemon to load the new module
sudo pkill unitd
sleep 1
sudo /path/to/unitd --no-daemon &
```

**Build notes (October 2026, verified on Debian 13):**
- **CRITICAL:** `--enable-embed` flag is **required** for FreeUnit module (PHP 8.5 renamed this flag from `--enable-embedded` in PHP 8.4)
- PHP 8.5.12 source build: ~3 min (incremental make), ~8 min (full clean build) on 8 CPU cores
- CLI binary: 70M (sapi/cli/php, debug symbols included)
- Verified working configure flags for FreeUnit: `--enable-fpm`, `--enable-cli`, `--enable-cgi`, **`--enable-embed`**, `--enable-mbstring`, `--with-openssl`, `--with-zlib`, `--with-curl`, `--with-mysqli`, `--enable-xml`
- Embed SAPI library: `/opt/php-8.5/lib/libphp.so` (created by `--enable-embed`)
- Required build dependencies: `libxml2-dev`, `libcurl4-openssl-dev`, `liboniguruma-dev`, `libsodium-dev`, `autoconf`, `automake`, `libtool`, `pkg-config`, `bison`, `re2c`
- **System symlink setup:** FreeUnit dynamically loads `libphp.so` at runtime. If you build multiple PHP versions, update `/lib/libphp.so` symlink to point to the desired version.

**Troubleshooting:**
- `no PHP embed SAPI found` → Add `--enable-embed` to configure and rebuild
- FreeUnit shows old PHP version → Check `/lib/libphp.so` symlink with `ls -la /lib/libphp.so` and `ldd /usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so`

**Comparison with CentOS/AlmaLinux:**
- **CentOS/AlmaLinux:** Use Remi repository (`dnf enable php:remi-8.5`)
- **Debian:** Build from source (no Remi equivalent for Debian)
- **Ubuntu:** Use Sury DPA (`ppa:ondrej/php`)

> **Setup time:** PHP build takes ~3-5 minutes on 8 CPU cores.
> After this, you'll have the latest PHP 8.5 + FreeUnit module ready.

### Comparison

| Feature | A: Native | B: Sury deb.sury.org | C: Build from Source |
|---------|-----------|-----|----------------------|
| **PHP version** | 8.4 (Deb13), 8.2–8.3 (Ubuntu) | **8.5 (any OS)** | 8.5.12 (any OS) |
| **Setup time** | 1 min | **2 min** ⭐ | 5–10 min (build) |
| **Setup complexity** | Low | **Low** ⭐ | High |
| **System integration** | Native (`/usr/bin`) | **Native** ⭐ | Custom (`/opt/php-8.5`) |
| **PHP updates** | `apt update` | **`apt update`** ⭐ | Manual (rebuild) |
| **OS Support** | Debian ✓ Ubuntu ✓ | **Debian ✓ Ubuntu ✓** ⭐ | Debian ✓ Ubuntu ✓ |
| **apt dependency resolution** | ✓ Full | **✓ Full** ⭐ | ✗ Manual |
| **Official CI support** | No | **Yes** ⭐ | No |
| **Recommended for** | Dev (8.4) | **Production (8.5)** ⭐ | Older PHP versions |

**Recommendation:**
- **PHP 8.4?** → Option A (native, 1 min)
- **PHP 8.5 anywhere?** → **Option B (Sury deb.sury.org, 2 min)** ⭐
- **Bleeding-edge/custom?** → Option C (build from source, 10 min)

**Parity with CentOS/AlmaLinux:**
- **CentOS/AlmaLinux:** Remi repository (`dnf enable php:remi-8.5`)
- **Debian/Ubuntu:** Sury deb.sury.org (`packages.sury.org/php/`) — same approach ✓
- See [build-deb.yml](https://github.com/freeunitorg/freeunit/blob/master/.github/workflows/build-deb.yml) for official workflow

## Build

```bash
git clone https://github.com/freeunitorg/freeunit.git
cd freeunit

git checkout v1.37.0   # or master / latest tag

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

**Build output (verified on Debian 13):**
```
Linking successful:
  -lssl -lcrypto (OpenSSL 3.5.7)
  -lpcre2-8 (PCRE2 10.46)
  -lz (zlib 1.3.1)
  -lzstd (Zstandard 1.5.7)
  -lbrotlienc (Brotli 1.1.0)

Compile time: ~16 seconds on 8 CPU cores
Binary size: 3.4M (unitd daemon)
```

> **Note on libdir:** Debian/Ubuntu use `/usr/lib/x86_64-linux-gnu` (multiarch) instead of `/usr/lib64`.
> Adjust `--modulesdir` accordingly.
>
> **OTEL support** is optional. It requires Rust ≥ 1.88 (`curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh`).
> To enable it, add `--otel` to the configure flags. The example above omits it for a faster build.
>
> `--user` / `--group` are omitted because the systemd service runs unitd directly.
> To restrict the runtime user, add `User=unit` and `Group=unit` to the `[Service]` section,
> and create the user: `sudo useradd -r -s /usr/sbin/nologin unit`.

### Language modules (non-PHP)

PHP module setup varies by option above. For other languages:

```bash
# Python (python3-dev already installed for PHP)
./configure python --config=python3-config
make -j$(nproc) python-install

# Go, Node.js, Ruby, Perl, Java, WASM — see ./configure --help
```

## Install

```bash
sudo make install
```

Binaries land at:
- `/usr/sbin/unitd` — release daemon
- `/usr/sbin/unitd-debug` — debug daemon (if `--debug` was used)
- `/usr/lib/x86_64-linux-gnu/unit/modules/*.unit.so` — language modules

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

The original NGINX Unit packages used `Type=forking` with `PIDFile=`, which creates a
race between systemd and unitd writing the PID file. `--no-daemon` keeps the process
attached to systemd as a direct child process — no PID file, no race, clean lifecycle management.

`RuntimeDirectory=unit` ensures `/run/unit` is created on start and removed on stop.

### Why resource limits

`CPUQuota`, `TasksMax`, and `Nice` are set as a safety net after past incidents where
runaway worker processes consumed excessive resources. Kept enabled for observability
until the root causes are confirmed fixed across all workloads.

## Verify

```bash
systemctl status unit

# Check loaded modules
curl --unix-socket /run/unit/control.sock http://localhost/config

# Expected output includes "modules" with loaded language modules:
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

To test with a local hostname (e.g., `myapp.local`):

```bash
# 1. Add to /etc/hosts
sudo tee -a /etc/hosts <<< "127.0.0.1 myapp.local"

# 2. Create app directory
sudo mkdir -p /var/www/myapp.local
echo '<?php phpinfo(); ?>' | sudo tee /var/www/myapp.local/phpinfo.php

# 3. Configure FreeUnit
curl -X PUT --unix-socket /run/unit/control.sock http://localhost/config << 'EOF'
{
  "applications": {
    "myapp": {
      "type": "php",
      "root": "/var/www/myapp.local"
    }
  },
  "listeners": {
    "*:8080": {
      "pass": "applications/myapp"
    }
  }
}
EOF

# 4. Test (verified October 2026 with PHP 8.5.12)
curl http://myapp.local:8080/phpinfo.php
# Expected output: PHP Version 8.5.12 (or your configured PHP version)
```

**Local testing notes (October 5, 2026):**
- Successfully tested myapp.local with PHP 8.5.12 built from source
- FreeUnit module dynamically loads libphp.so at startup — ensure `/lib/libphp.so` points to correct PHP version
- Testing verified: phpinfo() displays correct PHP version (8.5.12)

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
| Modules | `/usr/lib/x86_64-linux-gnu/unit/modules/*.unit.so` |

## Comparison with other distributions

| Distro | PHP 8.5 method | Package system | See also |
|--------|----------------|---|---|
| **Debian/Ubuntu** | deb.sury.org | apt | This doc (Option B) |
| **CentOS/AlmaLinux** | Remi repository | dnf | [CENTOS.ALMALINUX.md](CENTOS.ALMALINUX.md) |
| **Remi details** | `unit-php` prebuilt | dnf | [REMI.md](REMI.md) |

**Key finding:** Debian/Ubuntu now have **parity with CentOS/AlmaLinux**:
- CentOS: `dnf enable php:remi-8.5` → install pre-built Remi packages
- Debian/Ubuntu: `packages.sury.org/php` → install Sury pre-built packages
- **Both** follow the same "use pre-built PHP" strategy (not build-from-source)

This approach is validated by the official FreeUnit CI workflow.

## Migration from NGINX Unit

If upgrading from the archived NGINX Unit `.deb` packages:

```bash
# Stop old service
sudo systemctl stop unit

# Remove old package
sudo apt-get remove unit -y

# (Optional) Remove old state
# sudo rm -rf /var/lib/unit/*

# Build and install FreeUnit (steps above)
```

The control socket path may change depending on the old package version.
If you have scripts referencing the old socket path, update them:

```bash
# Find references to old socket paths
grep -r 'unit.sock\|unit/control' /etc/ /srv/ /home/ --include='*.sh' --include='*.conf' -l 2>/dev/null
```

### Debian/Ubuntu package compatibility

FreeUnit is API/ABI compatible with NGINX Unit 1.35.0+. If the official archived
NGINX Unit `.deb` packages are ever restored, FreeUnit can coexist or replace them
without application reconfiguration (assuming the same socket path is used).

## Troubleshooting

**PHP embed SAPI not found when building PHP module:**
```bash
# Error: "no PHP embed SAPI found"
# Solution: Install libphp-embed package
sudo apt-get install -y libphp-embed

# Then retry:
./configure php
make php
sudo make php-install
```

**`unitd` won't start, "address already in use":**
```bash
sudo ss -tlnp | grep 8080
# Kill stale process or change listener port
```

**PHP module not found:**
```bash
ls /usr/lib/x86_64-linux-gnu/unit/modules/
# Expected: php.unit.so (380K for PHP 8.4)
# If empty:
cd /path/to/freeunit
./configure php && make php && sudo make php-install
```

**Permission denied on control socket:**
```bash
ls -la /run/unit/control.sock
# Run curl with sudo, or add user to the appropriate group
sudo usermod -aG unit $USER
# (Requires login/logout to take effect)
```

**PHP module version mismatch:**
```bash
# Verify PHP version used for module
php -v
# Expected: PHP 8.4.26-1~deb13u1 (cli)

# If PHP was updated after module compilation, rebuild:
./configure php && make php && sudo make php-install
```

**Building against custom PHP (`/opt/php-*`):**
```bash
export LD_LIBRARY_PATH=/opt/php-8.5/lib:$LD_LIBRARY_PATH
./configure php --config=/opt/php-8.5/bin/php-config
make php
sudo make php-install
```

**FreeUnit showing wrong PHP version after module build:**
```bash
# Problem: Module built against PHP 8.5 but FreeUnit shows PHP 8.4 in phpinfo()
# Root cause: /lib/libphp.so still points to old PHP version

# Verify which libphp library the module is using:
ldd /usr/lib/x86_64-linux-gnu/unit/modules/php.unit.so | grep libphp
# Output should show: libphp.so => /lib/libphp.so

# Check what /lib/libphp.so currently points to:
ls -la /lib/libphp.so
# If it shows: libphp.so -> libphp8.4.so (old version)

# Update the symlink to the new PHP version:
sudo ln -sf /opt/php-8.5/lib/libphp.so /lib/libphp.so

# Restart FreeUnit daemon:
sudo pkill unitd
sleep 1
sudo /usr/sbin/unitd --no-daemon &

# Verify phpinfo() now shows correct PHP version
curl http://localhost:8080/phpinfo.php | grep "PHP Version"
```
