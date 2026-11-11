This document describes the release process for FreeUnit (ex NGINX Unit) and as such is
likely only of interest to FreeUnit maintainers.

# Create a preparatory branch

You should create a new branch for doing this work. E.g.

    $ git checkout -b x.y[.z]-prep master


# Create a set of commits

## Set the version

    $ tools/bump-version.sh X.Y.Z

This is the one way to set the version.  The files that carry it are
listed in `.github/scripts/version-sites`, and the script reads that list.
It sets the version in the `version` file (`NXT_VERSION` and
`NXT_VERNUM`), the title of `docs/unit-openapi.yaml`, the three unitctl
crates and their `Cargo.lock` entries, `tools/unitctl/openapi-config.json`
and `tools/unitctl/unit-openapi/README.md`.  It regenerates the
Dockerfiles in pkg/docker with `make -B dockerfiles`, and sets the default
of `UNIT_VERSION` in the three hand-written files in pkg/docker/local/.

It then runs `.github/scripts/check-version.sh`, which names what still
needs a person: docs/changes.xml, CHANGES, the unitctl CHANGELOG and
SECURITY.md (see below).  Edit those, run the check again until it
passes, and commit.  The script does not commit.  The same check runs on
every pull request, on every push to master and on the release tag.

Earlier releases made one commit per file by hand (3144710fe for
unitctl, 4d627c8f8 for unit-openapi.yaml, f7771378f for the Dockerfiles);
do not edit those files by hand any more.

If pkg/eol.json dropped a runtime version, `make dockerfiles` no longer
writes its Dockerfile, and check-version.sh names the file that is left
over; `git rm` it.

## unitctl CHANGELOG

Add a `## [X.Y.Z]` section and its `[X.Y.Z]:` link to
tools/unitctl/CHANGELOG.md.

## SECURITY.md

For a new minor release, make its `X.Y.x` row `✅ Active` and mark the
previous line EOL, or `✅ LTS` if it becomes a long-term line; one row
says Active.  A patch release of a line that is already Active or LTS
needs no change.

## changes.xml

Create a commit that updates the docs/changes.xml for this release.

As well as adding the various entries also update the 'date' and 'time'
fields.

Each entry is a `<change>` with a `type` of bugfix, feature, change,
security or workaround, and one or more `<para>` elements.  A paragraph
becomes a paragraph of the changelog, so use a second `<para>` for the
paragraphs of one entry that stand apart.

## The CHANGES file

The CHANGES file in the root of the tree is generated from
docs/changes.xml:

    $ make -C docs changes

It writes build/CHANGES and copies it over CHANGES.  Never edit CHANGES by
hand: edit docs/changes.xml and run the command again.  A hand edit is lost
the next time the file is generated, and the two files diverge.

The CI job "Check the generated files" runs "make -C docs check" and compares
the generated file with the committed one.  The job fails when the two differ.
Run the target before the release commit.

The packager of a stanza names the product in the title.  A stanza packed
by "FreeUnit Packaging" is titled "Changes with FreeUnit X.Y.Z".  Any other
packager is titled "Changes with Unit X.Y.Z".
The date of a block comes from the 'date' and 'time' fields of its stanza.

The stylesheet that renders the file, docs/changes.xslt, is the compiled
form of docs/changes.xsls.  xslscript.pl, which compiles it, is not
packaged in most distributions, so the compiled file is committed and the
two must be changed together.  If xslscript.pl is at hand, "make -C docs
xslt XSLS=/path/to/xslscript.pl" rebuilds it.

The target "make -C docs stylesheet-check" compares the named templates of
the two files.  It fails when they differ.  The compiler is not available in
most environments, so the target cannot compare the body of a template.
Run it after a change to either file.


# Merge it

These should be the last commits into the repository before the release
is tagged.


# Tag the release

Once the above has been merged you can tag it with the new version. For
this we create an annotated tag. E.g. On master

    $ git tag -a -m "FreeUnit 1.33.0 release." 1.33.0

This should create a new tag object pointing to the "CHANGES" commit.

The tag can be pushed just as the branch is. E.g.

    $ git push <upstream> 1.33.0

Do not create the `unitctl/X.Y.Z` tag by hand.  Pushing `X.Y.Z` runs
.github/workflows/unitctl.yml, whose release job creates `unitctl/X.Y.Z`
and its release itself; a tag that already exists makes that step fail.
The same push runs build-deb.yml, release-macos.yml, release-docker.yml
and release-source.yml.  release-docker.yml logs in to Docker Hub, so the
DOCKERHUB_USERNAME and DOCKERHUB_TOKEN secrets have to be set before the
tag is pushed.

release-macos.yml attaches freeunit-X.Y.Z-macos-arm64.tar.gz and
-macos-x86_64.tar.gz with their .sha256 files.  macOS runners are
sometimes not available; if the job was cancelled for that, run the
workflow by hand with the tag and "publish" set.  The same run adds the
archives to a release that was made before the workflow existed.  The
build is reproducible, and the job never replaces a file that the
release already has.


# A new 'Release'

After a while the new release should show up at
<https://github.com/freeunitorg/freeunit/releases>


# Tarball

Pushing the tag `X.Y.Z` runs .github/workflows/release-source.yml.  It
runs `make -C pkg dist` and attaches `unit-X.Y.Z.tar.gz` and
`unit-X.Y.Z.tar.gz.sha512` to the published GitHub release for the tag.
If no release exists yet, the job creates a published release with a
one-line body, and build-deb.yml adds its package section later.  The
job fails if the version file at the tag does not name `X.Y.Z`, or if
the release for the tag is still a draft.

To verify the published files:

    $ base=https://github.com/freeunitorg/freeunit/releases/download/X.Y.Z
    $ curl -fLO $base/unit-X.Y.Z.tar.gz
    $ curl -fLO $base/unit-X.Y.Z.tar.gz.sha512
    $ sha512sum -c unit-X.Y.Z.tar.gz.sha512

The tar stream is reproducible: the file times are the commit time.
The .tar.gz is not always the same.  Since git 2.38, `git archive`
compresses .tar.gz inside git with the zlib that git is linked to, and
the gzip header has no time stamp.  So a rebuild from the same tag gives
the same checksum only with git 2.38 or newer linked to zlib, as on the
Ubuntu runner.  A git linked to zlib-ng (Fedora, Arch), an older git, or
a `tar.tar.gz.command` setting gives other bytes with the same content.

The job never replaces a published file.  When the release already has
`unit-X.Y.Z.tar.gz` or its `.sha512`, the job compares the sha512.  If
it is equal, the job does not upload the file again.  If it differs, the
job fails and prints both sums.  Packagers pin this checksum, so do not
replace a published tarball.

If the job failed, re-run it from the default branch: on the Actions tab
pick "Release source tarball", "Run workflow", the default branch, and
enter the release tag `X.Y.Z`.  Dispatching from the default branch runs
the current workflow definition; "Re-run jobs" on the original tag push
repeats the definition at the pushed tag, so a later fix would not apply.
To upload by hand instead, check out the tag and rebuild the files.
Then download the files that are already on the release and compare
them with the rebuilt ones:

    $ git checkout X.Y.Z
    $ make -C pkg dist
    $ rm -rf old
    $ gh release download X.Y.Z -D old -p 'unit-X.Y.Z.tar.gz*'
    $ cmp old/unit-X.Y.Z.tar.gz pkg/unit-X.Y.Z.tar.gz
    $ cmp old/unit-X.Y.Z.tar.gz.sha512 pkg/unit-X.Y.Z.tar.gz.sha512

`gh release download` fails when the release has neither file, and
`cmp` reports a missing file for each file that is not on the release.
If `cmp` reports that a file differs, stop and upload nothing, as the
job does.  Otherwise upload only the files that are missing.  Without
`--clobber`, `gh` refuses the whole command when any file it names is
already on the release, so give each file its own command:

    $ gh release upload X.Y.Z pkg/unit-X.Y.Z.tar.gz
    $ gh release upload X.Y.Z pkg/unit-X.Y.Z.tar.gz.sha512


# Docs

The unit-docs repository needs a copy of CHANGES under
source/CHANGES.txt


# Post release

Immediately after release we should open the next version: add a new
changes header to docs/changes.xml and CHANGES.  Run
`tools/bump-version.sh` for the new version only when the release is
prepared, as above.

See 47d2f933a for an example.


# Appendix: the `version` file and what consumes it

The repository-root `version` file is the single source of truth for the
release number:

    # Copyright (C) FreeUnit Community

    NXT_VERSION=1.36.1
    NXT_VERNUM=13601

- **`NXT_VERSION`** — human-readable dotted string (`MAJOR.MINOR.PATCH`).
- **`NXT_VERNUM`** — the same number for compile-time comparisons:
  `MAJOR * 10000 + MINOR * 100 + PATCH` (e.g. `1.36.1` → `13601`). Keep both in
  lockstep; a mismatch is a silent bug.

## Consumers

- **Build:** `configure` sources `. ./version`; `auto/make` generates
  `build/include/nxt_version.h` (`#define NXT_VERSION` / `NXT_VERNUM`). Every
  object lists that header as a prerequisite, so a bump recompiles everything
  that embeds the version (`Server:` header, `unitd --version`, libunit).
- **Packaging:** `pkg/Makefile` (`VERSION ?= $(NXT_VERSION)`) names the source
  tarball; `pkg/{deb,rpm,docker,npm}/Makefile` each `include ../../version` for
  the package version and the docker image tag.
- **Language modules:** `auto/modules/{java,nodejs}` embed `$NXT_VERSION` into
  artifact names (`*.jar`, `unit-http-*.tgz`, `package.json`).

## Files that must move in lockstep with a bump

The list lives in `.github/scripts/version-sites`, with the reason each
file carries the version.  Run `.github/scripts/check-version.sh` to
check them all.
