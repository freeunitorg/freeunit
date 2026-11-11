# Homebrew formula for unitctl, from the binaries of the unitctl/X.Y.Z
# release.  pkg/brew/update.sh sets the version and the checksums after a
# release.
class Unitctl < Formula
  desc "CLI for the FreeUnit control API"
  homepage "https://github.com/freeunitorg/freeunit/tree/master/tools/unitctl"
  license "Apache-2.0"

  on_macos do
    on_arm do
      url "https://github.com/freeunitorg/freeunit/releases/download/unitctl/1.37.2/unitctl-1.37.2-aarch64-apple-darwin"
      sha256 "e20320870795493025441166dce1c4211d11f30118b3d37d1855d06d043c443f"
    end
    on_intel do
      url "https://github.com/freeunitorg/freeunit/releases/download/unitctl/1.37.2/unitctl-1.37.2-x86_64-apple-darwin"
      sha256 "603f83bfdfe42a6c0f03c49b48c2b0dfb731052f48b4e45099b8d311d050ac6f"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/freeunitorg/freeunit/releases/download/unitctl/1.37.2/unitctl-1.37.2-aarch64-unknown-linux-gnu"
      sha256 "392d8c62b3b3ec599343eb4884907cf4597c7428f7c07302b904d789e6862163"
    end
    on_intel do
      url "https://github.com/freeunitorg/freeunit/releases/download/unitctl/1.37.2/unitctl-1.37.2-x86_64-unknown-linux-gnu"
      sha256 "2136127ee9ef3743c3b552068c0343303438e7be5f69aeeb5d58afc37898f9d0"
    end
  end

  # The release has no man page; take it from the same tag.
  resource "man" do
    url "https://raw.githubusercontent.com/freeunitorg/freeunit/unitctl/1.37.2/tools/unitctl/man/unitctl.1"
    sha256 "459bdeac5dfd72ee47cd26e62c02dc72d50a8c1ef873f983bb9d2212cfd389d7"
  end

  def install
    bin.install Dir["unitctl-*"].first => "unitctl"
    # The man page of the tree is a template that the unitctl build fills.
    resource("man").stage do
      inreplace "unitctl.1", "%%VERSION%%", version.to_s
      man1.install "unitctl.1"
    end
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/unitctl --version")
  end
end
