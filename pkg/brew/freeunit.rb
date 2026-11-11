# Homebrew formula for unitd and unitctl, from the macOS archives and the
# unitctl binaries of the release.  pkg/brew/update.sh sets the version, the
# URLs and the checksums after a release.
class Freeunit < Formula
  desc "Universal web app server, a community fork of NGINX Unit"
  homepage "https://freeunit.org"
  # The version is explicit: "arm64" in the URL would be read as 64.
  version "1.37.2"
  license "Apache-2.0"

  # The archives are built by .github/workflows/release-macos.yml: unitd has
  # OpenSSL and PCRE2 linked in and needs only the libraries of macOS.
  depends_on :macos

  on_macos do
    on_arm do
      url "https://github.com/freeunitorg/freeunit/releases/download/1.37.2/freeunit-1.37.2-macos-arm64.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"

      resource "unitctl" do
        url "https://github.com/freeunitorg/freeunit/releases/download/unitctl/1.37.2/unitctl-1.37.2-aarch64-apple-darwin"
        sha256 "e20320870795493025441166dce1c4211d11f30118b3d37d1855d06d043c443f"
      end
    end

    on_intel do
      url "https://github.com/freeunitorg/freeunit/releases/download/1.37.2/freeunit-1.37.2-macos-x86_64.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"

      resource "unitctl" do
        url "https://github.com/freeunitorg/freeunit/releases/download/unitctl/1.37.2/unitctl-1.37.2-x86_64-apple-darwin"
        sha256 "603f83bfdfe42a6c0f03c49b48c2b0dfb731052f48b4e45099b8d311d050ac6f"
      end
    end
  end

  # The unitctl release has no man page; take it from the same tag.
  resource "unitctl-man" do
    url "https://raw.githubusercontent.com/freeunitorg/freeunit/unitctl/1.37.2/tools/unitctl/man/unitctl.1"
    sha256 "459bdeac5dfd72ee47cd26e62c02dc72d50a8c1ef873f983bb9d2212cfd389d7"
  end

  def install
    # The archive's unitd has the default paths under /usr/local.  Keep the
    # binary in libexec and put a script in sbin that gives the paths of this
    # prefix first; an option given by the caller comes later and wins.  The
    # state, log and run directories are outside the keg, so that the
    # configuration and the control socket survive an upgrade.  unitctl finds
    # the socket of a running unitd from its command line.
    libexec.install "sbin/unitd"
    (sbin/"unitd").write <<~SH
      #!/bin/sh
      exec "#{opt_libexec}/unitd" \\
        --control "unix:#{var}/run/unit/control.unit.sock" \\
        --statedir "#{var}/lib/unit" \\
        --pid "#{var}/run/unit/unit.pid" \\
        --log "#{var}/log/unit/unit.log" \\
        --modulesdir "#{HOMEBREW_PREFIX}/lib/unit/modules" \\
        "$@"
    SH
    (sbin/"unitd").chmod 0555

    man8.install "share/man/man8/unitd.8"
    doc.install Dir["share/doc/freeunit/*"]

    # libunit, for applications of type "external" (Go, Node.js).
    lib.install "lib/libunit.a"
    include.install Dir["include/*"]
    inreplace "lib/pkgconfig/unit.pc", "/usr/local", opt_prefix
    (lib/"pkgconfig").install "lib/pkgconfig/unit.pc"

    resource("unitctl").stage do
      binary = Dir["unitctl-*"].first
      chmod 0555, binary
      bin.install binary => "unitctl"
    end
    resource("unitctl-man").stage { man1.install "unitctl.1" }
  end

  post_install_steps do
    mkdir_p "lib/unit", base: :var
    mkdir_p "log/unit", base: :var
    mkdir_p "run/unit", base: :var
    mkdir_p "lib/unit/modules", base: :homebrew_prefix
  end

  service do
    run [opt_sbin/"unitd", "--no-daemon"]
    keep_alive true
    working_dir var
    log_path var/"log/unit/unit.log"
    error_log_path var/"log/unit/unit.log"
  end

  test do
    assert_match version.to_s, shell_output("#{sbin}/unitd --version 2>&1")
    assert_match version.to_s, shell_output("#{bin}/unitctl --version")

    port = free_port
    state = testpath/"state"
    state.mkpath
    control = testpath/"control.sock"

    pid = spawn sbin/"unitd", "--no-daemon",
                "--control", "unix:#{control}",
                "--statedir", state.to_s,
                "--pid", (testpath/"unit.pid").to_s,
                "--log", (testpath/"unit.log").to_s,
                "--tmpdir", testpath.to_s
    begin
      60.times do
        break if control.exist?

        sleep 0.5
      end
      assert_path_exists control

      config = <<~JSON
        {
          "listeners": { "127.0.0.1:#{port}": { "pass": "routes" } },
          "routes": [ { "action": { "return": 200 } } ]
        }
      JSON

      assert_match "Reconfiguration done",
                   shell_output("curl -s -X PUT --data-binary '#{config}' " \
                                "--unix-socket #{control} http://localhost/config")
      assert_match %r{^HTTP/1\.1 200}, shell_output("curl -sI http://127.0.0.1:#{port}/")
    ensure
      Process.kill("TERM", pid)
      Process.wait(pid)
    end
  end
end
