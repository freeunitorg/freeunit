# macOS and the Homebrew tap

Main is Linux-first. macOS jobs do not run on pull requests. The C macOS
job runs after a merge: a push to `master` or `1.37` that touches C paths,
or by hand. The unitctl darwin builds run on a release tag, on a unitctl
push to `master` or `1.37`, weekly and by hand. Main publishes no macOS
unitd packages.

An earlier plan was to move macOS support to a separate fork,
[freeunitorg/freeunit-macos](https://github.com/freeunitorg/freeunit-macos).
That migration was dropped. The fork holds no macOS work of its own. macOS
packaging lives in the Homebrew tap instead.

The C sources keep the macOS code paths (`NXT_MACOSX`,
`NXT_HAVE_MACOSX_SENDFILE`, `src/nxt_macosx_sendfile.c`). Configure probes
gate this code. A Linux build never compiles it. The post-merge macOS job
compiles and tests these paths on every merge that touches them.

## The Homebrew tap

macOS packaging lives in the Homebrew tap
[freeunitorg/homebrew-unit](https://github.com/freeunitorg/homebrew-unit).
The tap is community maintained. Its README lists the formulas. The tap
was revived on 2026-11-24
([#1](https://github.com/freeunitorg/homebrew-unit/pull/1),
[#2](https://github.com/freeunitorg/homebrew-unit/pull/2)).

The `unit` formula builds `unitd` with njs from `freeunitorg/njs` and pins
the source revision. The `unitctl` resource downloads the released darwin
and linux binaries and pins their sha256 sums. The module formulas build
against the matching `unit` formula. The PHP module formulas use the
`php-embed` formulas of the tap.

Between releases the `unit` formula can track the head of the release
branch. Since
[#4](https://github.com/freeunitorg/homebrew-unit/pull/4) it builds the
`1.37` branch as `1.37.3`, with the `unitctl` 1.37.2 binaries. A release
moves it back to the release tag.

Install:

```console
$ brew tap freeunitorg/unit https://github.com/freeunitorg/homebrew-unit
$ brew install freeunitorg/unit/unit
```

### Tap CI

A pull request runs `brew test-bot` on each changed formula, on macOS and
Linux runners. A first pull request from a new fork needs a maintainer
approval before the run starts. The `Rebuild everything` workflow
rebuilds every formula each week and on a manual dispatch. The tap
workflows name the runners.

### Bump the tap on a release

Each FreeUnit release needs a tap update. Do it after the release tag is
pushed and the `unitctl/X.Y.Z` release exists. The tag push creates that
release.

1. In `Formula/unit.rb`, point the source at the tag `X.Y.Z` and its
   revision, and set the version.
2. In `Formula/unit.rb`, set the `unitctl` version to `X.Y.Z` and update
   the four sha256 sums.
3. Update the module formulas to the same version and revision,
   including `depends_on "unit@X.Y.Z"`.
4. Rename the alias in `Aliases/` to `unit@X.Y.Z`.
5. Open a pull request against `freeunitorg/homebrew-unit`. Its
   `brew test-bot` run checks the formulas it changes. The weekly
   `Rebuild everything` run, or a manual dispatch of it, checks every
   formula.

## Why macOS CI is not on pull requests

macOS CI ran on every pull request that touched C paths. The macOS runners
used Actions minutes on each run.

GitHub hosts the macOS runners, and they are often short of capacity. On
2026-11-24 every macOS job on PR
[#1025](https://github.com/freeunitorg/freeunit/pull/1025) waited for a
runner and was cancelled after about 15 minutes. A busy macOS queue slows
every push.

macOS needs special work. The macOS runners have no OpenSSL headers. Docker
over colima mounts only `$HOME`. The shm page size is 16 KiB on Apple
Silicon ([#595](https://github.com/freeunitorg/freeunit/issues/595)). The
system shell is bash 3.2. Each workaround touched code that Linux shares.

Keeping the job off pull requests removes that queue from the PR path. The
job still runs after each merge that touches C paths, so the macOS code
paths stay tested.

macOS runners are not reliable enough for a required check yet. The macOS
jobs stay optional until reliable runners exist.
