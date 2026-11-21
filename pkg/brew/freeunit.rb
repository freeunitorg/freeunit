# Homebrew formula for unitd, built from the release tarball.
# pkg/brew/update.sh sets the version and the checksum after a release.
class Freeunit < Formula
  desc "Universal web app server, a community fork of NGINX Unit"
  homepage "https://freeunit.org"
  url "https://github.com/freeunitorg/freeunit/releases/download/1.37.2/unit-1.37.2.tar.gz"
  sha256 "5e2c093b946fdc8f0db99d5197a5c45a7024e1b1e3e9d0e0c1823ab39b1e3c24"
  license "Apache-2.0"

  depends_on "make" => :build
  depends_on "pkgconf" => :build
  depends_on "brotli"
  depends_on "openssl@3"
  depends_on "pcre2"
  depends_on "zstd"

  uses_from_macos "zlib"

  def install
    openssl = Formula["openssl@3"]

    # The state, log and run directories are outside the keg, so that the
    # configuration and the control socket survive an upgrade.  Modules from
    # other formulae go in HOMEBREW_PREFIX/lib/unit/modules.
    system "./configure",
           "--prefix=#{prefix}",
           "--modulesdir=#{HOMEBREW_PREFIX}/lib/unit/modules",
           "--statedir=#{var}/lib/unit",
           # unitctl finds the socket, the pid file and the log of a
           # running unitd only from these options, not from the
           # directories below.  It takes the first option that starts
           # with "--log", so --log comes before --logdir.
           "--control=unix:#{var}/run/unit/control.unit.sock",
           "--pid=#{var}/run/unit/unit.pid",
           "--log=#{var}/log/unit/unit.log",
           "--logdir=#{var}/log/unit",
           "--runstatedir=#{var}/run/unit",
           "--openssl",
           "--zlib",
           "--zstd",
           "--brotli",
           "--cc-opt=-I#{openssl.opt_include}",
           "--ld-opt=-L#{openssl.opt_lib}"

    # The default make of macOS is GNU make 3.81; the build needs a newer one.
    system "gmake"
    system "gmake", "build/lib/libunit.a"

    # unitd-install would also create the state and log directories, which
    # are outside the keg; post_install_steps makes them.  libunit, for
    # applications of type "external" (Go, Node.js), installs with its
    # headers and unit.pc as the build lists them.
    sbin.install "build/sbin/unitd"
    system "gmake", "manpage-install", "libunit-install"
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
