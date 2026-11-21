# 🩺 macOS local build and test report — FreeUnit on Intel Mac

Date: 2026-11-15 · Branch: `macos` (from `master` @ 7765a6ae) · Reporter: Qwen Code

## 1. 🏥 Related milestone tasks (gh, milestone 11 = 1.37.3)

The macOS and Homebrew work in milestone **1.37.3** lives in two open pull
requests by andypost. The author calls them "the macOS plan", parts 1 and 2.

| PR | Title | Status |
|----|-------|--------|
| [#972](https://github.com/freeunitorg/freeunit/pull/972) | pkg: make the repository a Homebrew tap for unitd and unitctl (`pkg/brew/freeunit.rb`, `pkg/brew/unitctl.rb`, `update.sh`, `.github/workflows/homebrew.yml`) | OPEN, no labels |
| [#973](https://github.com/freeunitorg/freeunit/pull/973) | pkg: attach a unitd archive for macOS to each release (`pkg/macos/build-tarball.sh`, `.github/workflows/release-macos.yml`, OpenSSL 3.5.9 + PCRE2 10.49 static, arm64 + x86_64) | OPEN, no labels |

#975 is in the same milestone (`ci: keep apt working…`). It is not related to
macOS.

Related history:

- [#595](https://github.com/freeunitorg/freeunit/issues/595) is closed (fixed
  in 1.37.1). libunit rejected every shm segment on macOS and Apple Silicon.
  The workflow `.github/workflows/build-test-macos.yml` now guards the
  16 KiB-page path.
- [#640](https://github.com/freeunitorg/freeunit/issues/640) is open and has
  no milestone. It covers the refused WHOAMI call on the BSDs and macOS.

CI today runs `build-test-macos.yml` on `macos-15` (arm64) only. The workflow
runs the C test programs and one unitd smoke request. There is **no CI leg for
Intel macOS** and no pytest on macOS. This local run covers that gap.

## 2. 🩻 Machine and toolchain (old but native)

| Component | Version |
|---|---|
| OS | macOS 13.7.8 Ventura (22H730), **x86_64** (Intel) |
| Compiler | Apple clang 14.0.0 (clang-1400.0.29.202) |
| Make | GNU Make 3.81 (system. The CI brew `gmake` is not available.) |
| Xcode CLT | /Library/Developer/CommandLineTools |
| OpenSSL | 3.6.2 (MacPorts, `/opt/local/libexec/openssl3`) |
| PCRE2 | 10.x (MacPorts, `/opt/local`) |
| Rust | **1.99.1** via rustup (installed for `--otel`) |
| PHP | **8.5.12**, built from the official source with `--enable-embed`, prefix `~/freeunit-tmp/php85` |
| Docker | colima v0.10.1 VM, linux/amd64, engine 29.2.1. Run 4 used 2 CPUs and 6 GiB (see §6) |
| Homebrew | **not installed**. macOS 13 is past the supported window of brew. MacPorts is the package manager here. |

The MacPorts base was stale. It was built for darwin 21, but the system is
darwin 22. As a result, the `port` command refused to run.

During this session, the base was updated to 2.12.7 from the package
`MacPorts-2.12.7-13-Ventura.pkg`. Then `sudo port migrate` ran to completion.
Outcome: the libraries under `/opt/local` stayed valid during the migration.
They powered the native build through `--cc-opt` and `--ld-opt`.

The `docker` port was installed fresh: CLI **29.9.3** plus
`docker-buildx-plugin` 0.37.1 in `/opt/local/bin`. These replace the temporary
static docker-27.5.1 CLI in `~/.local/bin` from the port-tree rebuild.

The `colima`, `lima`, and `qemu` ports were **not** reinstalled. The running
colima VM became orphaned as a result. Its binaries in
`/opt/local/bin/{colima,limactl,qemu-system-x86_64}` were deactivated mid-run.
The processes continued to run (see §6).

The host has 8 GiB of RAM and 4 cores. This is tight for a 5 GiB VM plus the
build.

## 3. 💉 Native build

The configure command uses the same flags as `run-local-full.sh`
(`--otel --openssl --debug`). It adds `--tests` for the C suites. It also
adds paths to the MacPorts libraries:

```console
$ ./configure --tests --openssl --debug --otel \
      --cc-opt='-fno-common -I/opt/local/include' \
      --ld-opt='-L/opt/local/lib -Wl,-rpath,/opt/local/lib'
```

Summary: TLS **YES** (OpenSSL), Regex **YES** (PCRE2), otel **YES** (Rust
1.99.1 built the otel static library), debug **YES**. zlib, zstd, brotli,
njs, and USDT are not enabled. `run-local-full.sh` does not require them.

Build results:

- `make -j4` — **OK**. `build/sbin/unitd` linked with the kqueue engine and
  `nxt_macosx_sendfile.c` (native macOS paths).
- `make build/lib/libunit.a` — **OK**.
- `make tests` — **OK** after the `-fno-common` fix below. All test binaries
  linked (`build/tests`, `ncq_test`, `vbcq_test`, `unit_app_test`,
  `unit_websocket_chat`, `unit_websocket_echo`, `unit_close_test`,
  `unit_port_recv_test`, `unit_msg_test`).
- `./build/sbin/unitd --version` → `unit version: 1.37.2`.
- `otool -L build/sbin/unitd` → only `libssl.3` and `libcrypto.3`
  (`/opt/local/libexec/openssl3`), `libpcre2-8.0.dylib` (`/opt/local`), and
  `/usr/lib/libSystem.B.dylib`. The binary is rpath-linked. No `DYLD_*`
  variables are necessary (SIP-safe).

### 🩹 Gotcha found: tentative definitions vs ld64 (old macOS only)

With Apple clang 14, tentative definitions such as `nxt_pid`, `nxt_ppid`,
`nxt_euid`, `nxt_egid` (`src/nxt_process.c`) compile to **COMMON** symbols.
ld64 does not extract a static-archive member when an undefined reference
matches a COMMON definition. As a result, every test binary failed to link:

```
Undefined symbols for architecture x86_64:
  "_nxt_egid", referenced from: _nxt_lib_start in libnxt-tests.a(nxt_lib.test.o)
  "_nxt_euid", ...
  "_nxt_pid",  ... (also from nxt_mp.test.o, nxt_random.test.o)
  "_nxt_ppid", ...
```

`--cc-opt='-fno-common'` moves these symbols into `.bss`. Linking then
succeeds.

CI on `macos-15` does not hit this problem. Its clang uses `-fno-common` by
default. A note in `build-test-macos.yml` or the docs will help users on
Ventura or older Xcode. No source change is necessary.

## 4. 🧪 Native C test results (build-test-macos.yml equivalents)

| Suite | Result |
|---|---|
| `./build/tests` (full C suite: rbtree, mp, utf8, parse, tstr, conf, http_parse, regex, openssl server init, cpu limit, process nofile, and more) | **PASSED** (exit 0) |
| `./build/unit_msg_test` (libunit malformed port message; real shm object) | **all passed** |
| `./build/unit_close_test` (libunit close-provenance, concurrent stress) | **all passed** |
| `./build/unit_port_recv_test` (fd-ownership, deferred graceful quit) | **all passed** |
| `./build/ncq_test`, `./build/vbcq_test` (queue tests) | **PASSED** (exit 0) |

Benign noise: the log shows `setrlimit(RLIMIT_NOFILE) 64 → 512: Operation not
permitted`. The sandbox blocked the increase of the soft limit. The test
itself passed. The OpenSSL certificate test also printed duplicate-`localhost`
warnings.

## 5. ⚕️ unitd smoke tests (both CI smoke steps, run natively)

1. **Serve a request from `unit_app_test`.** unitd started with a private
   control socket. The configuration was applied over the control API
   ("Reconfiguration done."). `GET /` answered **"Hello world!"** on
   127.0.0.1:8391. unitd stayed alive. The log has no "failed to start" line.
   **PASSED**.
2. **Detached request** (`unit_app_test -d`, the FINISH retry path of
   `nxt_unit_run()`). The first, detached request was answered. The worker
   survived the detached finish past the 0.8 s retry window. The second
   request was answered. The log carried the injection verdict
   (`unit_app_test: N injected failures, all spent on FINISH`).
   **PASSED**.

### 🧬 Extra: a PHP 8.5.12 application (beyond the CI steps)

CI on macOS runs no language module. This step went further. The goal was to
show FreeUnit from `master` with PHP 8.5.12 on this Mac.

MacPorts has the port `php85 @8.5.12`. But the port has no embed SAPI. Its
SAPI subports are `apache2handler`, `cgi`, and `fpm` only. The unit module
links against `libphp`. So a local build with `--enable-embed` was
necessary.

PHP 8.5.12 came from the official source:

1. The tarball `php-8.5.12.tar.gz` was downloaded from php.net. The
   published SHA-256 sum matched.
2. configure ran with `--prefix=$HOME/freeunit-tmp/php85 --disable-all
   --enable-embed --without-pear`.
3. `make -j4` and `make install` passed. The prefix holds `bin/php` and
   `lib/libphp.dylib`.
4. The unit tree then configured and built the module:

```console
$ ./configure php --config=$HOME/freeunit-tmp/php85/bin/php-config \
      --lib-path=$HOME/freeunit-tmp/php85/lib --module=php85
$ make php85
```

   The probes reported: PHP version **8.5.12**, PHP embed SAPI **found**,
   Zend Thread Safety **not found** (NTS), `zend_signal_startup()` found.
   The artifact is `build/lib/unit/modules/php85.unit.so`. It links
   `@rpath/libphp.dylib`. The rpath points at the PHP prefix.

5. unitd started with a private control socket and
   `--modulesdir build/lib/unit/modules`. The log printed:

```
module: php 8.5.12 ".../build/lib/unit/modules/php85.unit.so"
```

6. The control API accepted a php application with target `main` and script
   `index.php`. `GET /` on 127.0.0.1:8392 answered **200 OK**:

```
X-Powered-By: PHP/8.5.12
Server: Unit/1.37.2

engine        = FreeUnit (unit) cli-server SAPI
PHP_VERSION   = 8.5.12
PHP_OS        = Darwin
PHP_INT_SIZE  = 8
ZTS           = no (NTS)
extensions    = Core, Reflection, SPL, Zend OPcache, date, hash, json,
                lexbor, pcre, random, standard, unit, uri
```

Result: **PASSED 🩺 — FreeUnit (`master` @ 7765a6ae) serves PHP 8.5.12
natively on Intel macOS.** The unitd of this step still runs. Its control
socket and log are in `~/freeunit-tmp/php-unit/`.

## 6. 🧫 clang-ast via `test/run-local-full.sh` (Docker)

`test/run-local-full.sh` is Linux and Docker by design. The ngx-ast LLVM
plugin is a Linux `.so`. So the script ran here under colima (a linux/amd64
VM on this Mac).

Attempt log:

1. `docker login ghcr.io` succeeded. But the private image
   `ghcr.io/freeunitorg/freeunit-clang-ast:trixie` is not pullable with this
   account. The script fell back to its documented local-build path
   (debian:testing, apt clang/llvm-dev/rustc, clone of
   `freeunitorg/clang-ast`, make plugin). The image `freeunit-test-full:local`
   (~2.97 GB) built OK. It includes `ngx-ast.so`.
2. Run 1 — the script exited with code 127. `port migrate` had just
   deactivated the docker CLI in `/opt/local/bin`. The workaround was a
   static CLI in `~/.local/bin` against the socket of the still-running VM
   (`DOCKER_HOST=unix://$HOME/.colima/default/docker.sock`).
3. Run 2 — the container saw an empty `/unit`. colima mounts only `$HOME`
   into the VM, not `/tmp`. So the copy from `mktemp -d /tmp/...` was
   invisible to Docker. The fix is local, in `test/run-local-full.sh`:
   `mktemp -d "${TMPDIR:-/tmp}/..."` (uncommitted on this branch). Then the
   script ran again with `TMPDIR="$HOME/freeunit-tmp"`.
4. Run 3 — the run that got the furthest. rsync copied the tree, and the
   cached image loaded. `CC=clang ./configure --otel --openssl --debug`
   passed all probes with the plugin loaded. `make` compiled the **entire C
   side**: every `build/src/*.o` and `AR build/lib/libnxt.a`. cargo compiled
   the full otel dependency tree (~110 crates: tokio, tonic 0.14.9, hyper,
   reqwest, axum). At 18:25 the VM **guest froze** at the tail of this step.
   The output stopped right after `Compiling opentelemetry-proto v0.33.1`.
   That is the final `libotel.a` rustc and link step, the step with peak
   memory use. `docker version` and `/_ping` over the socket no longer
   answered (curl timeout). Diagnosis: QMP `query-status` showed `running`
   (not paused). The serial log was clean apart from benign virtio DISCARD
   errors. The console had no kernel OOM lines. The most probable cause is
   guest-level memory thrash: the Rust link inside a 5 GiB VM on an 8 GiB
   host. (The run was cancelled from the host. The EXIT trap of the script
   removed its temporary rsync copy.)
5. Run 4 — the run that finished the job. The host reboot and the MacPorts
   update came first. The `colima` command was back after the update. The VM
   started again against the disk under `/Volumes/doc/docker/_lima`
   (`LIMA_HOME`), with a new size: 2 CPUs and 6 GiB of RAM. The command was
   `colima start --cpu 2 --memory 6`. The built image
   `freeunit-test-full:local` was still inside the VM. No rebuild was
   necessary. The script ran again with
   `TMPDIR="$HOME/freeunit-tmp" ./test/run-local-full.sh`. configure passed
   all probes with the plugin loaded (the container held OpenSSL 3.6.5 and
   PCRE2 10.49). The C side compiled completely. The plugin printed no
   diagnostic. cargo compiled 116 crates and printed ``Finished `release`
   profile [optimized] target(s) in 21m 12s``. The compile passed
   `opentelemetry-proto v0.33.1`. That was the step that froze the VM in run
   3. `LD build/sbin/unitd` passed. The script removed its temporary copy.
   Exit code was 0.

Result: **PASSED 🩺 — run 4 printed `=== clang-ast check PASSED ===`.** The
clang-ast plugin found no violation in the C code.

🩹 Lesson from runs 3 and 4: a 5 GiB guest with 3 CPUs froze at peak memory
use during the otel build. A 6 GiB guest with 2 CPUs did not. On an 8 GiB
host, give the VM more RAM and fewer CPUs for this check.

## 7. 💊 Not covered / deviations

- `--otel` on native macOS: Rust 1.99.1 built the otel library with no
  problems. The native run keeps the configure flags of `run-local-full.sh`,
  except the clang-ast plugin flags (Linux-only). The flags are
  `--otel --openssl --debug --tests`.
- The pytest suite was not run. The php module got a manual run (see §5).
  The other language modules (python, and the rest) were not built.
- `--hardening=strict`, njs, zlib, zstd, and brotli were not exercised.
- It was not possible to try the Homebrew formulas from PR #972 locally. This
  machine has no brew (macOS 13 is past the supported window of brew). The CI
  legs of the formula (`macos-15` and `macos-15-intel`) cover it.
